"""Device manager for Ulanzi D200 to serialize access and provide helpers."""

import fcntl
import os
from contextlib import contextmanager
from pathlib import Path
from typing import Dict, Optional, Tuple

from ulanzi_d200 import UlanziDevice

LOCK_PATH = Path("/tmp/ulanzi_device.lock")
LOCK_DISABLED = os.getenv("ULANZI_LOCK_DISABLED") == "1"


@contextmanager
def device_session(device_path: Optional[str] = None, block: bool = True):
    """Acquire an exclusive lock and yield a connected device. Set ULANZI_LOCK_DISABLED=1 to skip locking."""
    if LOCK_DISABLED:
        dev = UlanziDevice(device_path=device_path)
        try:
            yield dev
        finally:
            dev.close()
        return

    LOCK_PATH.parent.mkdir(parents=True, exist_ok=True)
    with LOCK_PATH.open("w") as lock_fd:
        try:
            fcntl.flock(lock_fd, fcntl.LOCK_EX | (0 if block else fcntl.LOCK_NB))
        except BlockingIOError:
            raise
        dev = UlanziDevice(device_path=device_path)
        try:
            yield dev
        finally:
            try:
                dev.close()
            finally:
                fcntl.flock(lock_fd, fcntl.LOCK_UN)


def send_buttons(buttons: Dict[int, Dict], device_path: Optional[str] = None) -> Dict:
    """Send buttons payload; returns summary info."""
    with device_session(device_path=device_path) as dev:
        # Wrap to capture file size
        class WriteProxy:
            def __init__(self, real_dev):
                self.dev = real_dev
                self.file_size = None

            def write(self, data):
                if self.file_size is None and len(data) >= 8:
                    import struct

                    self.file_size = struct.unpack("<I", data[4:8])[0]
                return self.dev.write(data)

            def __getattr__(self, item):
                return getattr(self.dev, item)

        dev.device = WriteProxy(dev.device)
        dev.set_buttons(buttons)
        size = dev.device.file_size
        patch_note = getattr(dev, "_last_patch_note", "") or ""
        patch_bytes = getattr(dev, "_last_patched_bytes", 0) or 0
        zip_count = getattr(dev, "_last_zip_count", None)
    return {
        "zip_size": size,
        "patch_note": patch_note,
        "patch_bytes": patch_bytes,
        "zip_count": zip_count,
        "buttons": sorted([b + 1 for b in buttons.keys()]),
    }


def ping_keep_alive(device_path: Optional[str] = None, best_effort: bool = False) -> Tuple[bool, str]:
    """Single keep-alive ping. If best_effort and device busy, returns (False, \"\")."""
    try:
        with device_session(device_path=device_path, block=not best_effort) as dev:
            import datetime

            now = datetime.datetime.now().strftime("%H:%M:%S")
            dev.set_small_window_data({"mode": 1, "time": now}, force=True)
            return True, now
    except BlockingIOError:
        return False, ""
