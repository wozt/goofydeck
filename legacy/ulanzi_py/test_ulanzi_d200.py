import io
import json
import struct
import zipfile

import pytest

import ulanzi_d200
from ulanzi_d200 import CommandProtocol


class FakeDevice:
    def __init__(self):
        self.writes = []
        self.read_queue = []
        self.closed = False
        self.nonblocking = None
        self.opened_path = None

    def open_path(self, path):
        self.opened_path = path

    def set_nonblocking(self, flag):
        self.nonblocking = flag

    def read(self, size):
        if self.read_queue:
            return self.read_queue.pop(0)
        return []

    def queue_read(self, payload):
        self.read_queue.append(payload)

    def write(self, data):
        self.writes.append(bytes(data))
        return len(data)

    def close(self):
        self.closed = True


class FakeHid:
    def __init__(self):
        self.devices = []

    def enumerate(self, vid, pid):
        return [{'path': b'fake-path'}]

    def device(self):
        device = FakeDevice()
        self.devices.append(device)
        return device


@pytest.fixture
def fake_hid(monkeypatch):
    fake = FakeHid()
    monkeypatch.setattr(ulanzi_d200, "hid", fake)
    return fake


@pytest.fixture
def controller(fake_hid):
    return ulanzi_d200.UlanziDevice()


def reconstruct_zip_bytes(device):
    first_packet = device.writes[0]
    total_length = struct.unpack('<I', first_packet[4:8])[0]
    first_data = first_packet[8:1024]
    if len(device.writes) > 1:
        remaining = b"".join(device.writes[1:])
    else:
        remaining = b""
    return (first_data + remaining)[:total_length]


def test_build_packet_structure(controller):
    payload = b"abc"
    packet = controller._build_packet(CommandProtocol.OUT_SET_BRIGHTNESS, payload, len(payload))

    assert packet[:2] == controller.HEADER
    assert struct.unpack('>H', packet[2:4])[0] == CommandProtocol.OUT_SET_BRIGHTNESS
    assert struct.unpack('<I', packet[4:8])[0] == len(payload)
    assert packet[8:11] == payload


def test_set_brightness_clamps_and_sends_payload(controller):
    controller.set_brightness(150)

    sent = controller.device.writes[-1]
    length = struct.unpack('<I', sent[4:8])[0]
    payload = sent[8:8 + length]
    assert payload == b"100"


def test_set_label_style_merges_defaults(controller):
    custom_style = {"FontName": "Mono", "ShowTitle": False}
    controller.set_label_style(custom_style)

    sent = controller.device.writes[-1]
    length = struct.unpack('<I', sent[4:8])[0]
    payload = json.loads(sent[8:8 + length].decode("utf-8"))

    assert payload["FontName"] == "Mono"
    assert payload["ShowTitle"] is False
    assert payload["Align"] == "bottom"
    assert payload["Size"] == 10


def test_set_small_window_data_builds_expected_payload(controller):
    controller.set_small_window_data({"mode": 2, "cpu": 10, "mem": 20, "gpu": 30, "time": "12:00:00"})

    sent = controller.device.writes[-1]
    length = struct.unpack('<I', sent[4:8])[0]
    payload = sent[8:8 + length]
    assert payload == b"2|10|20|12:00:00|30"


def test_read_button_press_parses_packet_and_invokes_callback(controller):
    packet = bytearray(controller.PACKET_SIZE)
    packet[0:2] = controller.HEADER
    packet[2:4] = struct.pack('>H', CommandProtocol.IN_BUTTON)
    packet[8:12] = bytes([5, 7, 0, 0x01])  # state, index, ?, pressed flag

    received = []
    controller.set_button_callback(lambda press: received.append(press))
    controller.device.queue_read(list(packet))

    press = controller.read_button_press()

    assert press is not None
    assert press.index == 7
    assert press.state == 5
    assert press.pressed is True
    assert received and received[0].index == 7


def test_set_buttons_builds_zip_with_manifest_and_images(controller, tmp_path):
    icon_path = tmp_path / "icon.png"
    icon_path.write_bytes(b"PNGDATA")
    buttons = {
        0: {"label": "Hello", "image": str(icon_path)},
        2: {"state": 3},
    }

    controller.set_buttons(buttons)
    zip_bytes = reconstruct_zip_bytes(controller.device)

    with zipfile.ZipFile(io.BytesIO(zip_bytes), "r") as zf:
        names = set(zf.namelist())
        assert "manifest.json" in names
        assert "dummy.txt" in names
        assert f"icons/{icon_path.name}" in names

        manifest = json.loads(zf.read("manifest.json"))
        assert manifest["0_0"]["ViewParam"][0]["Text"] == "Hello"
        assert manifest["0_0"]["ViewParam"][0]["Icon"] == f"icons/{icon_path.name}"
        assert manifest["2_0"]["State"] == 3

        icon_bytes = zf.read(f"icons/{icon_path.name}")
        assert icon_bytes == b"PNGDATA"


def test_close_marks_device_closed(controller):
    controller.close()
    assert controller.device.closed is True
