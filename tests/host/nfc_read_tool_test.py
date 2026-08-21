#!/usr/bin/env python3
"""nfc_read_tool_test.py -- the nfc-read reply decoder, against handcrafted frames.

WHY THIS IS TESTED. `nfc-read` exists because py-opendisplay implements no NFC_SUB_READ, so it is
the only way any NFC read row can be driven through the command endpoint. That makes its decoder
an ORACLE for hardware evidence, and an oracle that is wrong produces confident wrong verdicts:
a decoder that ignored the declared length would pass a device that framed 218 in its header and
sent 200 bytes, which is exactly the class of defect the 218/219 rows exist to catch.

Nothing here touches a radio. bleak is stubbed rather than required, because a check that skips on
a machine without the BLE stack is a check that eventually means nothing.
"""

from __future__ import annotations

import importlib.util
import pathlib
import sys
import types

sys.modules.setdefault("bleak", types.SimpleNamespace(BleakClient=object))

_CLI = pathlib.Path(__file__).resolve().parents[2] / "targets" / "esp32-idf" / "tools"
_spec = importlib.util.spec_from_file_location("od_device_cli", _CLI / "od-device-cli.py")
assert _spec is not None and _spec.loader is not None
cli = importlib.util.module_from_spec(_spec)
sys.modules["od_device_cli"] = cli
_spec.loader.exec_module(cli)
cli._status = lambda _message: None

FAILURES: list[str] = []
CHECKS = 0


def check(cond: bool, what: str) -> None:
    global CHECKS
    CHECKS += 1
    if not cond:
        FAILURES.append(what)


def read_reply(rec_type: int, payload: bytes, declared: int | None = None) -> bytes:
    n = len(payload) if declared is None else declared
    return bytes([0x00, 0x83, 0x80, rec_type, (n >> 8) & 0xFF, n & 0xFF]) + payload


def main() -> int:
    # A well-formed read at the cap: the row every 218-byte hardware case rests on.
    r = cli._decode_nfc_read(read_reply(1, b"\x5a" * 218))
    check(r["ok"], "218-byte read accepted")
    check(r["rec_type"] == 1 and r["rec_type_name"] == "URI", "record type decoded")
    check(r["declared_len"] == 218 and r["actual_len"] == 218, "lengths decoded")

    # Zero-length and single-byte records are legal.
    check(cli._decode_nfc_read(read_reply(0, b""))["ok"], "empty record accepted")
    check(cli._decode_nfc_read(read_reply(4, b"\x01"))["ok"], "one-byte record accepted")

    # THE LENGTH FIELD IS CHECKED, NOT TRUSTED. Without this a device that framed the header
    # correctly and sent the wrong number of bytes reads as a clean pass -- and that is precisely
    # what a truncating adapter looks like from the host.
    r = cli._decode_nfc_read(read_reply(1, b"\x5a" * 200, declared=218))
    check(not r["ok"], "declared 218 with 200 bytes is refused")
    check("declared 218" in r.get("reason", ""), "the mismatch is named in the reason")

    # Every NACK code is reported by name rather than as a number an operator has to look up.
    for code, name in ((0x02, "READ_FAILED"), (0x01, "MALFORMED"), (0x05, "INVALID_REC_TYPE")):
        r = cli._decode_nfc_read(bytes([0xFF, 0x83, 0xFF, code]))
        check(not r["ok"] and r.get("nack") and r["error"] == name, f"NACK {name} decoded")

    # Malformed replies are reported, never raised: this runs at a bench, and a traceback loses
    # the frame that caused it.
    for frame, why in (
        (b"", "empty"),
        (b"\x00", "one byte"),
        (b"\x00\x40\x80", "wrong command"),
        (b"\x00\x83\x81", "not a read status"),
        (b"\x00\x83\x80\x01", "truncated before the length field"),
        (b"\xff\x83\xff", "NACK with no error byte"),
    ):
        r = cli._decode_nfc_read(frame)
        check(not r["ok"], f"{why} refused")
        check("raw" in r or "reason" in r, f"{why} reports something actionable")

    print(f"nfc_read_tool: {CHECKS} checks, {len(FAILURES)} failures")
    for f in FAILURES:
        print(f"  FAIL {f}")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
