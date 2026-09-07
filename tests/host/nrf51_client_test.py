#!/usr/bin/env python3
from __future__ import annotations

import json
import pathlib
import unittest
from collections import deque
from unittest.mock import AsyncMock, MagicMock, patch

from PIL import Image
from epaper_dithering import DitherMode

from opendisplay import OpenDisplayDevice
from opendisplay.exceptions import BLEConnectionError, RefreshTimeoutError
from opendisplay.transport.connection import BLEConnection


ROOT = pathlib.Path(__file__).resolve().parents[2]
VECTORS = ROOT / "tests/vectors/nrf51_dispatch.json"


def config_replies() -> list[bytes]:
    raw = json.loads(VECTORS.read_text(encoding="utf-8"))
    vector = next(
        item for item in raw["vectors"]
        if item["id"] == "dispatch/nrf51-config-read-mtu23-chunks"
    )
    return [bytes.fromhex(value) for value in vector["expect"]["replies"]]


class SlimTransport:
    max_frame = 244
    supports_write_without_response = False
    device_name = "OD-LT213A-TEST"

    def __init__(self, *, refresh_timeout: bool = False) -> None:
        self._connected = False
        self._responses: deque[bytes] = deque()
        self.written: list[bytes] = []
        self.requested_responses: list[bool] = []
        self.effective_responses: list[bool] = []
        self.notification_lengths: list[int] = []
        self.refresh_timeout = refresh_timeout

    async def connect(self) -> None:
        self._connected = True

    async def disconnect(self) -> None:
        self._connected = False

    @property
    def is_connected(self) -> bool:
        return self._connected

    def _queue(self, *frames: bytes) -> None:
        for frame in frames:
            if len(frame) > 20:
                raise AssertionError(f"notification exceeds ATT-MTU-23: {len(frame)}")
            self.notification_lengths.append(len(frame))
            self._responses.append(frame)

    async def write_command(
        self, data: bytes, response: bool = True, drain_stale: bool = True
    ) -> None:
        del drain_stale
        if not self._connected:
            raise BLEConnectionError("Not connected")
        if not 2 <= len(data) <= self.max_frame:
            raise BLEConnectionError(f"invalid GATT value length {len(data)}")
        self.written.append(data)
        self.requested_responses.append(response)
        self.effective_responses.append(response or not self.supports_write_without_response)
        opcode = int.from_bytes(data[:2], "big")
        if opcode == 0x0040:
            self._queue(*config_replies())
        elif opcode == 0x0043:
            self._queue(b"\x00\x43\x02\x00\x08" + b"1a2b3c4d" + b"\x00")
        elif opcode == 0x0044:
            self._queue(b"\x00\x44\x46\x24" + bytes(11) + b"\x52\x00\x10")
        elif opcode == 0x0050:
            self._queue(b"\x00\x50\x03")
        elif opcode in (0x0070, 0x0071, 0x0072):
            self._queue(data[:2])
            if opcode == 0x0072:
                self._queue(b"\x00\x74" if self.refresh_timeout else b"\x00\x73")
        else:
            raise AssertionError(f"unexpected command 0x{opcode:04x}")

    async def read_response(self, timeout: float = 5.0) -> bytes:
        del timeout
        if not self._responses:
            raise AssertionError("client read with no queued slim response")
        return self._responses.popleft()

    def drain_notifications(self) -> int:
        return 0

    async def clear_cache(self) -> bool:
        return False


class ClientIntegrationTest(unittest.IsolatedAsyncioTestCase):
    async def test_interrogate_and_uncompressed_upload(self) -> None:
        transport = SlimTransport()
        with patch("opendisplay.device.BLEConnection", return_value=transport):
            device = OpenDisplayDevice(mac_address="AA:BB:CC:DD:EE:FF", max_queue_size=1)
            async with device:
                self.assertEqual((device.width, device.height), (104, 212))
                version = await device.read_firmware_version()
                self.assertEqual((version["major"], version["minor"]), (2, 0))
                self.assertEqual(version["sha"], "1a2b3c4d")
                await device.upload_image(
                    Image.new("RGB", (104, 212), "white"),
                    compress=True,
                    dither_mode=DitherMode.NONE,
                )

        opcodes = [int.from_bytes(frame[:2], "big") for frame in transport.written]
        self.assertEqual(opcodes.count(0x0070), 1)
        self.assertEqual(opcodes.count(0x0071), 12)
        self.assertEqual(opcodes.count(0x0072), 1)
        self.assertFalse(any(len(frame) > 244 for frame in transport.written))
        data_frames = [frame for frame in transport.written if frame[:2] == b"\x00\x71"]
        self.assertEqual(sum(len(frame) - 2 for frame in data_frames), 2756)
        self.assertTrue(all(len(frame) <= 232 for frame in data_frames))
        data_response_flags = [
            (requested, effective)
            for frame, requested, effective in zip(
                transport.written,
                transport.requested_responses,
                transport.effective_responses,
                strict=True,
            )
            if frame[:2] == b"\x00\x71"
        ]
        self.assertTrue(all(not requested and effective for requested, effective in data_response_flags))
        self.assertTrue(transport.notification_lengths)
        self.assertLessEqual(max(transport.notification_lengths), 20)

    async def test_refresh_timeout_is_typed(self) -> None:
        transport = SlimTransport(refresh_timeout=True)
        with patch("opendisplay.device.BLEConnection", return_value=transport):
            device = OpenDisplayDevice(mac_address="AA:BB:CC:DD:EE:FF", max_queue_size=1)
            async with device:
                with self.assertRaises(RefreshTimeoutError):
                    await device.upload_image(
                        Image.new("RGB", (104, 212), "black"),
                        compress=False,
                        dither_mode=DitherMode.NONE,
                    )

    async def test_att_busy_is_generic_error_and_link_survives(self) -> None:
        connection = BLEConnection("AA:BB:CC:DD:EE:FF")
        client = MagicMock()
        client.is_connected = True
        client.write_gatt_char = AsyncMock(side_effect=[RuntimeError("ATT 0x80"), None])
        connection._client = client
        connection._notification_characteristic = MagicMock()
        connection._write_no_response_supported = False

        with self.assertRaisesRegex(BLEConnectionError, "Write failed"):
            await connection.write_command(bytes(244), response=False)
        self.assertTrue(connection.is_connected)
        await connection.write_command(b"\x00\x43", response=False)
        self.assertTrue(connection.is_connected)
        self.assertTrue(client.write_gatt_char.call_args.kwargs["response"])


if __name__ == "__main__":
    unittest.main()
