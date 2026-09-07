#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

MODULE_PATH = pathlib.Path(__file__).parents[2] / "targets/nrf51-s130/tools/check_image.py"
SPEC = importlib.util.spec_from_file_location("nrf51_check_image", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
CHECK_IMAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK_IMAGE)


class ImageTest(unittest.TestCase):
    def valid_image(self) -> dict[int, int]:
        image: dict[int, int] = {}
        vectors = CHECK_IMAGE.RAM_END.to_bytes(4, "little") + (
            CHECK_IMAGE.APP_START + 9
        ).to_bytes(4, "little")
        for offset, value in enumerate(vectors):
            image[CHECK_IMAGE.APP_START + offset] = value
        return image

    def test_hex_round_trip_and_vectors(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "app.hex"
            expected = self.valid_image()
            CHECK_IMAGE.write_hex(path, expected)
            actual = CHECK_IMAGE.read_hex(path)
            self.assertEqual(actual, expected)
            self.assertEqual(
                CHECK_IMAGE.validate_app(actual),
                (CHECK_IMAGE.APP_START, CHECK_IMAGE.APP_START + 8),
            )

    def test_wrong_stack_is_rejected(self) -> None:
        image = self.valid_image()
        image[CHECK_IMAGE.APP_START + 3] = 0
        with self.assertRaisesRegex(ValueError, "initial stack"):
            CHECK_IMAGE.validate_app(image)

    def test_reserved_tail_and_conflicting_merge_are_rejected(self) -> None:
        image = self.valid_image()
        image[CHECK_IMAGE.APP_END] = 0xAA
        with self.assertRaisesRegex(ValueError, "outside"):
            CHECK_IMAGE.validate_app(image)
        with self.assertRaisesRegex(ValueError, "conflict"):
            CHECK_IMAGE.merge_images({1: 2}, {1: 3})

    def test_bad_hex_checksum_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "bad.hex"
            path.write_text(":00000001FE\n", encoding="ascii")
            with self.assertRaisesRegex(ValueError, "checksum"):
                CHECK_IMAGE.read_hex(path)

    def test_wrong_softdevice_hash_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            app = root / "app.hex"
            softdevice = root / "s130.hex"
            elf = root / "app.elf"
            CHECK_IMAGE.write_hex(app, self.valid_image())
            CHECK_IMAGE.write_hex(softdevice, {0: 0})
            elf.write_bytes(b"")
            argv = [
                "check_image.py", "--app", str(app), "--elf", str(elf),
                "--softdevice", str(softdevice), "--merged", str(root / "full.hex"),
            ]
            with mock.patch.object(sys, "argv", argv), mock.patch.object(
                CHECK_IMAGE, "size_values", return_value=(8, 0, 0)
            ), mock.patch.object(
                CHECK_IMAGE, "validate_symbols", return_value=None
            ):
                self.assertEqual(CHECK_IMAGE.main(), 1)
            self.assertFalse((root / "full.hex").exists())

    def test_forbidden_symbols_are_rejected(self) -> None:
        with mock.patch.object(
            CHECK_IMAGE.subprocess, "check_output", return_value="00000000 T od_session_init\n"
        ):
            with self.assertRaisesRegex(ValueError, "od_session_init"):
                CHECK_IMAGE.validate_symbols("nm", pathlib.Path("app.elf"))


if __name__ == "__main__":
    unittest.main()
