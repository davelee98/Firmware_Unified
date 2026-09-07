#!/usr/bin/env python3
"""Validate and optionally merge the nRF51822 application and pinned S130 image."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys

APP_START = 0x0001B000
APP_END = 0x0001F800
RAM_START = 0x20001F00
RAM_END = 0x20004000
SOFTDEVICE_END = APP_START
S130_SHA256 = "17ccba6573c51b386ccacae96d2eb428162737ef1cbe2f38059fd44e224078e7"
FORBIDDEN_SYMBOL = re.compile(
    r"^_*(?:(?:malloc|calloc|realloc|free)(?:_r)?$|"
    r"od_(?:session|gate|pipe|nfc|config_asm|config_store)(?:$|_)|"
    r"(?:od_)?(?:inflate|zlib)(?:$|_)|framebuffer(?:$|_))"
)


def read_hex(path: pathlib.Path) -> dict[int, int]:
    image: dict[int, int] = {}
    base = 0
    eof = False
    for line_number, raw in enumerate(path.read_text(encoding="ascii").splitlines(), 1):
        if not raw.startswith(":"):
            raise ValueError(f"{path}:{line_number}: not Intel HEX")
        record = bytes.fromhex(raw[1:])
        if len(record) < 5 or len(record) != record[0] + 5 or sum(record) & 0xFF:
            raise ValueError(f"{path}:{line_number}: invalid record/checksum")
        length = record[0]
        offset = int.from_bytes(record[1:3], "big")
        kind = record[3]
        data = record[4 : 4 + length]
        if kind == 0:
            for index, value in enumerate(data):
                address = base + offset + index
                if address in image and image[address] != value:
                    raise ValueError(f"{path}:{line_number}: conflicting byte at 0x{address:08x}")
                image[address] = value
        elif kind == 1:
            eof = True
        elif kind == 2:
            if length != 2:
                raise ValueError(f"{path}:{line_number}: malformed segment address")
            base = int.from_bytes(data, "big") << 4
        elif kind == 4:
            if length != 2:
                raise ValueError(f"{path}:{line_number}: malformed linear address")
            base = int.from_bytes(data, "big") << 16
        elif kind not in (3, 5):
            raise ValueError(f"{path}:{line_number}: unsupported record type {kind}")
    if not eof:
        raise ValueError(f"{path}: missing EOF record")
    return image


def record(address: int, kind: int, data: bytes) -> str:
    body = bytes((len(data),)) + address.to_bytes(2, "big") + bytes((kind,)) + data
    checksum = (-sum(body)) & 0xFF
    return ":" + (body + bytes((checksum,))).hex().upper()


def write_hex(path: pathlib.Path, image: dict[int, int]) -> None:
    lines: list[str] = []
    current_upper: int | None = None
    addresses = sorted(image)
    at = 0
    while at < len(addresses):
        start = addresses[at]
        upper = start >> 16
        if upper != current_upper:
            lines.append(record(0, 4, upper.to_bytes(2, "big")))
            current_upper = upper
        chunk = bytearray((image[start],))
        at += 1
        while (
            at < len(addresses)
            and len(chunk) < 16
            and addresses[at] == start + len(chunk)
            and addresses[at] >> 16 == upper
        ):
            chunk.append(image[addresses[at]])
            at += 1
        lines.append(record(start & 0xFFFF, 0, bytes(chunk)))
    lines.append(record(0, 1, b""))
    path.write_text("\n".join(lines) + "\n", encoding="ascii")


def u32(image: dict[int, int], address: int) -> int:
    try:
        raw = bytes(image[address + index] for index in range(4))
    except KeyError as error:
        raise ValueError(f"vector missing byte at 0x{error.args[0]:08x}") from error
    return int.from_bytes(raw, "little")


def validate_app(image: dict[int, int]) -> tuple[int, int]:
    if not image:
        raise ValueError("application image is empty")
    low = min(image)
    high = max(image) + 1
    if low != APP_START or high > APP_END:
        raise ValueError(
            f"application range 0x{low:08x}..0x{high - 1:08x} is outside "
            f"0x{APP_START:08x}..0x{APP_END - 1:08x}"
        )
    stack = u32(image, APP_START)
    reset = u32(image, APP_START + 4)
    if stack != RAM_END:
        raise ValueError(f"initial stack 0x{stack:08x}, expected 0x{RAM_END:08x}")
    if reset & 1 == 0 or not APP_START <= (reset & ~1) < APP_END:
        raise ValueError(f"reset vector 0x{reset:08x} is not Thumb code in app flash")
    if any(APP_END <= address < 0x20000 for address in image):
        raise ValueError("application writes reserved tail pages")
    return low, high


def size_values(tool: str, elf: pathlib.Path) -> tuple[int, int, int]:
    output = subprocess.check_output((tool, str(elf)), text=True).splitlines()
    if len(output) < 2:
        raise ValueError(f"unexpected output from {tool}")
    fields = output[-1].split()
    text, data, bss = (int(fields[index]) for index in range(3))
    if text + data > APP_END - APP_START:
        raise ValueError("ELF loadable size exceeds 18,432-byte app flash")
    if data + bss > 5888:
        raise ValueError("ELF .data + .bss exceeds 5,888-byte static-RAM goal")
    return text, data, bss


def validate_symbols(tool: str, elf: pathlib.Path) -> None:
    output = subprocess.check_output((tool, "-a", str(elf)), text=True)
    found = []
    for line in output.splitlines():
        fields = line.split()
        if fields and FORBIDDEN_SYMBOL.search(fields[-1]):
            found.append(fields[-1])
    if found:
        raise ValueError(f"forbidden linked symbol(s): {', '.join(sorted(set(found)))}")


def merge_images(first: dict[int, int], second: dict[int, int]) -> dict[int, int]:
    merged = dict(first)
    for address, value in second.items():
        if address in merged and merged[address] != value:
            raise ValueError(f"images conflict at 0x{address:08x}")
        merged[address] = value
    return merged


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", required=True, type=pathlib.Path)
    parser.add_argument("--elf", required=True, type=pathlib.Path)
    parser.add_argument("--size-tool", default="arm-none-eabi-size")
    parser.add_argument("--nm-tool", default="arm-none-eabi-nm")
    parser.add_argument("--manifest", type=pathlib.Path)
    parser.add_argument("--softdevice", type=pathlib.Path)
    parser.add_argument("--merged", type=pathlib.Path)
    args = parser.parse_args()

    try:
        app = read_hex(args.app)
        low, high = validate_app(app)
        text, data, bss = size_values(args.size_tool, args.elf)
        validate_symbols(args.nm_tool, args.elf)
        manifest: dict[str, object] = {
            "target": "nrf51-s130",
            "softdevice": "S130 2.0.1",
            "application_start": f"0x{low:08x}",
            "application_end_exclusive": f"0x{high:08x}",
            "application_hex_sha256": hashlib.sha256(args.app.read_bytes()).hexdigest(),
            "text_bytes": text,
            "data_bytes": data,
            "bss_bytes": bss,
            "static_ram_bytes": data + bss,
            "stack_bytes": 2048,
            "ram_margin_bytes": RAM_END - RAM_START - 2048 - data - bss,
            "flash_margin_bytes": APP_END - APP_START - text - data,
        }
        if args.softdevice is not None:
            digest = hashlib.sha256(args.softdevice.read_bytes()).hexdigest()
            if digest != S130_SHA256:
                raise ValueError(
                    f"S130 SHA-256 {digest}, expected {S130_SHA256}; refusing merge"
                )
            softdevice = read_hex(args.softdevice)
            if softdevice and max(softdevice) >= SOFTDEVICE_END:
                raise ValueError("SoftDevice overlaps application region")
            if args.merged is None:
                raise ValueError("--softdevice requires --merged")
            write_hex(args.merged, merge_images(softdevice, app))
            manifest["softdevice_hex_sha256"] = digest
            manifest["merged_hex"] = str(args.merged)
        elif args.merged is not None:
            raise ValueError("--merged requires --softdevice")
        if args.manifest is not None:
            args.manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(manifest, sort_keys=True))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"check_image.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
