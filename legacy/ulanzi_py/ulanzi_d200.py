"""USB device communication for Ulanzi D200"""

import os
import struct
import io, time, random
import zipfile
import json
import logging
import sys
from pathlib import Path
from typing import Dict, Optional, Callable
from dataclasses import dataclass
from enum import IntEnum
from deepdiff import DeepDiff

try:
    import hid
except ImportError:
    hid = None

logger = logging.getLogger(__name__)


def _configure_logging_from_env():
    """Enable console logging when ULANZI_DEBUG/ULANZI_LOG_LEVEL is set."""
    debug_flag = os.getenv("ULANZI_DEBUG")
    level_name = os.getenv("ULANZI_LOG_LEVEL")
    if not debug_flag and not level_name:
        return

    level = logging.DEBUG if debug_flag else getattr(logging, level_name.upper(), logging.INFO)
    logging.basicConfig(
        level=level,
        format="%(asctime)s %(levelname)s %(name)s - %(message)s",
    )
    logger.setLevel(level)


_configure_logging_from_env()

# USB IDs
VENDOR_ID = 0x2207
PRODUCT_ID = 0x0019

# Command protocols
class CommandProtocol(IntEnum):
    OUT_SET_BUTTONS = 0x0001
    OUT_SET_SMALL_WINDOW_DATA = 0x0006
    OUT_SET_BRIGHTNESS = 0x000a
    OUT_SET_LABEL_STYLE = 0x000b
    OUT_PARTIALLY_UPDATE_BUTTONS = 0x000d
    IN_BUTTON = 0x0101
    IN_BUTTON_2 = 0x0102
    IN_DEVICE_INFO = 0x0303


@dataclass
class ButtonPress:
    """Button press event"""
    index: int
    pressed: bool
    state: int


class UlanziDevice:
    """Ulanzi D200 device controller"""

    PACKET_SIZE = 1024
    CHUNK_SIZE = 1016
    HEADER = b'\x7c\x7c'
    BUTTON_COUNT = 14  # 13 regular buttons (0-12) + 1 clock button (13)
    ICON_SIZE = 196

    def __init__(self, device_path: Optional[str] = None):
        """Initialize device connection"""
        if hid is None:
            raise ImportError("hidapi not installed. Run: pip install hidapi")

        self.device = None
        self.device_path = device_path
        self._button_callback: Optional[Callable[[ButtonPress], None]] = None
        self._patched_bytes_total = 0
        self._last_patched_bytes = 0
        self._last_patch_note: str = ""
        self._zip_send_count = 0
        self._last_zip_count = 0
        self._connect()

    def _connect(self):
        """Connect to device"""
        if self.device_path:
            self.device = hid.device()
            logger.debug(f"Opening device by explicit path: {self.device_path}")
            self.device.open_path(self.device_path.encode())
        else:
            # Find device by vendor/product ID
            devices = hid.enumerate(VENDOR_ID, PRODUCT_ID)
            if not devices:
                raise RuntimeError(
                    f"Ulanzi D200 device not found (VID: {VENDOR_ID:04x}, PID: {PRODUCT_ID:04x})"
                )
            device_info = devices[0]
            logger.debug(f"Discovered device path: {device_info['path']}")
            self.device = hid.device()
            self.device.open_path(device_info['path'])

        self.device.set_nonblocking(True)
        logger.info("Connected to Ulanzi D200 device")

    def close(self):
        """Close device connection"""
        if self.device:
            self.device.close()
            logger.info("Disconnected from device")

    def set_button_callback(self, callback: Callable[[ButtonPress], None]):
        """Set callback for button presses"""
        self._button_callback = callback

    def read_button_press(self) -> Optional[ButtonPress]:
        """Read button press from device (non-blocking)"""
        if not self.device:
            return None

        try:
            data = self.device.read(self.PACKET_SIZE)
            if not data or len(data) < 8:
                return None

            # Parse packet header
            header = bytes(data[0:2])
            if header != self.HEADER:
                return None

            command = struct.unpack('>H', bytes(data[2:4]))[0]
            if command != CommandProtocol.IN_BUTTON and command != CommandProtocol.IN_BUTTON_2:
                return None

            # Parse button data
            button_data = bytes(data[8:12])
            state = button_data[0]
            index = button_data[1]
            pressed = button_data[3] == 0x01

            button_press = ButtonPress(index=index, pressed=pressed, state=state)
            if self._button_callback:
                self._button_callback(button_press)
            return button_press

        except Exception as e:
            logger.debug(f"Error reading button press: {e}")
            return None

    def set_brightness(self, brightness: int):
        """Set display brightness (0-100)"""
        brightness = max(0, min(100, brightness))
        payload = str(brightness).encode('ascii')
        self._send_command(CommandProtocol.OUT_SET_BRIGHTNESS, payload)
        logger.debug(f"Set brightness to {brightness}%")

    def set_label_style(self, style: Dict):
        """Set label styling for buttons"""
        default_style = {
            'Align': 'bottom',
            'Color': 0xFFFFFF,
            'FontName': 'Roboto',
            'ShowTitle': True,
            'Size': 10,
            'Weight': 80,
        }
        default_style.update(style)
        payload = json.dumps(default_style).encode('utf-8')
        self._send_command(CommandProtocol.OUT_SET_LABEL_STYLE, payload)
        logger.debug("Set label style")

    def set_small_window_data(self, data: Dict, force=False):
        """Set small window data (status display)"""
        from datetime import datetime, timezone
        #if not force and not DeepDiff(self._small_window_data, data):
        #    return False

        mode = data.get('mode', 1)  # 0=STATS, 1=CLOCK, 2=BACKGROUND
        cpu = data.get('cpu', 0)
        mem = data.get('mem', 0)
        gpu = data.get('gpu', 0)
        time_str = data.get('time', datetime.now().strftime('%H:%M:%S'))

        payload = f'{mode}|{cpu}|{mem}|{time_str}|{gpu}'.encode('utf-8')
        self._send_command(CommandProtocol.OUT_SET_SMALL_WINDOW_DATA, payload)

    def set_buttons(self, buttons: Dict[int, Dict]) -> bool:
        """Set button configuration with images."""
        manifest = {}
        images_added = 0
        invalid_bytes = {b'\x00'[0], b'\x7c'[0]}
        dummy_str = ''
        dummy_retries = 0

        logger.info(f"Preparing to send {len(buttons)} button(s)")
        while True:
            # Create ZIP with button data
            zip_buffer = io.BytesIO()
            with zipfile.ZipFile(zip_buffer, 'w', zipfile.ZIP_DEFLATED) as zf:
                manifest = {}

                for idx, config in buttons.items():
                    row = idx // 5
                    col = idx % 5
                    key = f"{col}_{row}"

                    button_data = {
                        'State': config.get('state', 0),
                        'ViewParam': [{}],
                    }

                    if config:
                        if 'label' in config:
                            button_data['ViewParam'][0]['Text'] = config['label']

                        if 'image' in config:
                            image_path = config['image']
                            image_path_obj = Path(image_path)
                            if image_path_obj.exists():
                                icon_name = image_path_obj.name
                                with open(image_path, 'rb') as f:
                                    zf.writestr(f'icons/{icon_name}', f.read())
                                button_data['ViewParam'][0]['Icon'] = f'icons/{icon_name}'
                                images_added += 1
                                logger.debug(f"Added image for button {idx}: {image_path}")
                            else:
                                logger.warning(f"Image not found for button {idx}: {image_path}")

                    manifest[key] = button_data

                # Add manifest
                zf.writestr('manifest.json', json.dumps(manifest, sort_keys=True, separators=(',', ':'), indent=2))
                logger.debug(f"Manifest: {json.dumps(manifest, indent=2)}")

                # Add dummy file to avoid protocol bug
                if dummy_retries > 0:
                    dummy_str += ''.join(random.choices('abcdefghijklmnopqrstuvwxyz', k=8*dummy_retries))

                zf.writestr('dummy.txt', dummy_str)

            # Get ZIP data and patch systematically
            zip_data = zip_buffer.getvalue()
            file_size = len(zip_data)
            logger.debug(f"Built button ZIP (size={file_size} bytes, dummy_retries={dummy_retries})")

            # Patch until clean, surface a single notice with cumulative total
            zip_data, fixed_offsets = self._patch_invalid_bytes(zip_data, invalid_bytes)
            self._last_patch_note = ""
            if fixed_offsets:
                self._last_patched_bytes = len(fixed_offsets)
                self._patched_bytes_total += self._last_patched_bytes
                self._last_patch_note = self._emit_patch_notice(fixed_offsets, self._last_patched_bytes)
            break

        # Send ZIP data
        self._send_file(zip_data)
        self._zip_send_count += 1
        self._last_zip_count = self._zip_send_count
        logger.info(f"Set {len(buttons)} button(s) with {images_added} image(s)")

        return True

    def _apply_protocol_workaround(self, data: bytes) -> bytes:
        """Apply workaround for protocol bug with certain byte values"""
        # Check for problematic bytes at 1024-byte boundaries
        invalid_bytes = {b'\x00'[0], b'\x7c'[0]}

        for i in range(1016, len(data), 1024):
            if i < len(data) and data[i] in invalid_bytes:
                # Need to regenerate with larger padding
                logger.debug(f"Protocol bug detected at offset {i}, regenerating ZIP")
                # For now, just log it - the dummy.txt padding should handle most cases
                pass

        return data

    def _send_file(self, data: bytes):
        """Send file data in chunks"""
        chunk_size = 1024
        file_size = len(data)

        # First chunk with header (1016 bytes of data)
        first_chunk = data[:chunk_size - 8]
        packet = self._build_packet(CommandProtocol.OUT_SET_BUTTONS, first_chunk.ljust(chunk_size - 8, b'\x00'), file_size)
        packets = [packet]

        # Remaining chunks (raw, no header)
        for i in range(chunk_size - 8, len(data), chunk_size):
            chunk = data[i:i + chunk_size]
            chunk = chunk.ljust(chunk_size, b'\x00')
            packets.append(chunk)

        logger.debug(f"Sending {file_size} bytes across {len(packets)} packet(s)")
        # Write all packets at once
        for packet in packets:
            self.device.write(packet)

        logger.debug(f"Sent {file_size} bytes in {len(packets)} chunks")

    def _send_command(self, command: CommandProtocol, payload: bytes):
        """Send command to device"""
        packet = self._build_packet(command, payload, len(payload))
        self.device.write(packet)

    def _build_packet(self, command: CommandProtocol, data: bytes, length: int) -> bytes:
        """Build USB packet"""
        packet = bytearray(self.PACKET_SIZE)

        # Header
        packet[0:2] = self.HEADER

        # Command protocol
        packet[2:4] = struct.pack('>H', command)

        # Length (big-endian)
        packet[4:8] = struct.pack('<I', length)

        # Data
        packet[8:8 + len(data)] = data

        return bytes(packet)

    def _patch_invalid_bytes(self, zip_data: bytes, invalid_bytes: set) -> tuple[bytes, list[int]]:
        """Patch problematic bytes at 1024-aligned offsets until clean."""
        patched = bytearray(zip_data)
        fixed_offsets: list[int] = []
        while True:
            current = [i for i in range(1016, len(patched), 1024) if patched[i] in invalid_bytes]
            if not current:
                break
            for idx in current:
                patched[idx] = 0x01
            fixed_offsets.extend(current)
        return bytes(patched), fixed_offsets

    def _emit_patch_notice(self, fixed_offsets: list[int], patched_bytes: int) -> str:
        """Surface patch info (stored for caller to display cleanly)."""
        note = f"invalid bytes patch {fixed_offsets} (fix:{patched_bytes})"
        logger.debug(note)
        return note
