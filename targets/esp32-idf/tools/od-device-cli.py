#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["bleak", "pyyaml", "cryptography"]
# ///
"""
Read/edit/write an OpenDisplay device's config, live over BLE or offline as hex.

Decodes the binary config packet (see loadGlobalConfig(), src/config_parser.cpp) into
YAML with named fields for every block type in include/opendisplay_structs.h, and
re-encodes edited YAML back to the same wire format:
    [2B LE length] [1B version] { [1B separator] [1B tag] [NB struct] }* [2B LE CRC-16]

Commands: read-config, write-config, decode-config, encode-config, add-sensor, read-msd
Run `od-device-cli.py <command> --help` for command-specific examples.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import re
import sys
import time
from contextlib import asynccontextmanager
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, AsyncIterator, Callable, NamedTuple

import bleak
import yaml
from cryptography.hazmat.primitives import cmac
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import AESCCM

CHAR_UUID = "00002446-0000-1000-8000-00805f9b34fb"
WRITE_CHUNK_SIZE = 200

# --- inlined from config_packet.py (still used standalone by provision_firmware.py) ---

MAX_PACKET = 4096


def crc16ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= (byte << 8) & 0xFFFF
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def outer_packet_crc(body: bytes) -> int:
    """CRC16-CCITT over outer packet body (length field zeroed)."""
    if len(body) < 2:
        return crc16ccitt(body)
    tmp = bytearray(body)
    tmp[0] = 0
    tmp[1] = 0
    return crc16ccitt(bytes(tmp))


def parse_hex_bytes(text: str) -> bytes:
    text = text.strip()
    if not text:
        raise ValueError("empty config hex string")
    if re.search(r"[\s,]", text):
        parts = re.split(r"[\s,]+", text)
        return bytes(int(p, 16) for p in parts if p)
    if len(text) % 2:
        raise ValueError("hex string has odd length")
    return bytes.fromhex(text)


def read_hex_arg(value: str | None, file_path: str | None) -> bytes:
    if bool(value) == bool(file_path):
        raise ValueError("specify exactly one of --config-hex or --config-file")
    packet = parse_hex_bytes(value) if value else open(file_path, "rb").read()
    validate_packet(packet)
    return packet


def validate_packet(packet: bytes) -> None:
    if len(packet) < 4:
        raise ValueError("config packet too short")
    if len(packet) > MAX_PACKET:
        raise ValueError(f"config packet too large ({len(packet)} bytes, max {MAX_PACKET})")
    declared = packet[0] | (packet[1] << 8)
    if declared != len(packet):
        raise ValueError(f"length field {declared} does not match packet size {len(packet)}")
    given = packet[-2] | (packet[-1] << 8)
    calc = outer_packet_crc(packet[:-2])
    if given != calc:
        raise ValueError(f"CRC mismatch (given 0x{given:04X}, expected 0x{calc:04X})")


# --- inlined from ble_crypto.py ---
# Mirrors src/encryption.cpp: AES-128-CMAC challenge/response, session key/id
# derivation, and AES-CCM (13-byte nonce, 12-byte tag) command/response framing.

SESSION_LABEL = b"OpenDisplay session"


def aes_cmac(key: bytes, message: bytes) -> bytes:
    c = cmac.CMAC(algorithms.AES(key))
    c.update(message)
    return c.finalize()


def aes_ecb_encrypt_block(key: bytes, block: bytes) -> bytes:
    if len(block) != 16:
        raise ValueError("aes_ecb_encrypt_block: block must be exactly 16 bytes")
    encryptor = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
    return encryptor.update(block) + encryptor.finalize()


def aes_ccm_encrypt(key: bytes, nonce: bytes, plaintext: bytes, ad: bytes, tag_length: int = 12) -> tuple[bytes, bytes]:
    ct_and_tag = AESCCM(key, tag_length=tag_length).encrypt(nonce, plaintext, ad)
    return ct_and_tag[:-tag_length], ct_and_tag[-tag_length:]


def aes_ccm_decrypt(key: bytes, nonce: bytes, ciphertext: bytes, tag: bytes, ad: bytes) -> bytes:
    return AESCCM(key, tag_length=len(tag)).decrypt(nonce, ciphertext + tag, ad)


def derive_session_key(master_key: bytes, client_nonce: bytes, server_nonce: bytes, device_id: bytes) -> bytes:
    cmac_input = SESSION_LABEL + b"\x00" + device_id + client_nonce + server_nonce + b"\x00\x80"
    intermediate = aes_cmac(master_key, cmac_input)
    ecb_input = (1).to_bytes(8, "big") + intermediate[:8]
    return aes_ecb_encrypt_block(master_key, ecb_input)


def derive_session_id(session_key: bytes, client_nonce: bytes, server_nonce: bytes) -> bytes:
    return aes_cmac(session_key, client_nonce + server_nonce)[:8]


def compute_challenge_response(master_key: bytes, server_nonce: bytes, client_nonce: bytes, device_id: bytes) -> bytes:
    return aes_cmac(master_key, server_nonce + client_nonce + device_id)


def compute_server_response(session_key: bytes, server_nonce: bytes, client_nonce: bytes, device_id: bytes) -> bytes:
    return aes_cmac(session_key, server_nonce + client_nonce + device_id)


def encrypt_command(session_key: bytes, session_id: bytes, counter: int, cmd_hi: int, cmd_lo: int, payload: bytes) -> tuple[bytes, int]:
    if len(payload) > 255:
        raise ValueError("encrypt_command: payload exceeds 255 bytes")
    nonce_full = session_id + counter.to_bytes(8, "big")
    ccm_nonce = nonce_full[3:]
    ad = bytes([cmd_hi, cmd_lo])
    plaintext = bytes([len(payload)]) + payload
    ciphertext, tag = aes_ccm_encrypt(session_key, ccm_nonce, plaintext, ad)
    wire = bytes([cmd_hi, cmd_lo]) + nonce_full + ciphertext + tag
    return wire, counter + 1


def decrypt_response(session_key: bytes, data: bytes) -> bytes:
    if len(data) < 2 + 16 + 1 + 12:
        raise ValueError("decrypt_response: frame too short to be an encrypted response")
    cmd = data[:2]
    nonce_full = data[2:18]
    tag = data[-12:]
    ciphertext = data[18:-12]
    ccm_nonce = nonce_full[3:]
    plaintext = aes_ccm_decrypt(session_key, ccm_nonce, ciphertext, tag, cmd)
    payload_len = plaintext[0]
    return cmd + plaintext[1 : 1 + payload_len]


def response_needs_decryption(data: bytes) -> bool:
    if len(data) < 2:
        return False
    if data[0] == 0xFF:
        return False
    command = (data[0] << 8) | data[1]
    if command in (0x0050, 0x0043):
        return False
    if len(data) < 2 + 16 + 1 + 12:
        # Shorter than any valid CCM frame (2B cmd + 16B nonce + >=1B ciphertext +
        # 12B tag) - can only be a plaintext status/ack, e.g. the final config-write
        # ack sent after reloadConfigAfterSave() clears the session server-side.
        return False
    return True


@dataclass
class BleSession:
    master_key: bytes
    session_key: bytes | None = field(default=None)
    session_id: bytes | None = field(default=None)
    counter: int = 0
    authenticated: bool = False

    def encrypt(self, cmd_hi: int, cmd_lo: int, payload: bytes) -> bytes:
        assert self.session_key is not None and self.session_id is not None
        wire, self.counter = encrypt_command(self.session_key, self.session_id, self.counter, cmd_hi, cmd_lo, payload)
        return wire

    def decrypt(self, data: bytes) -> bytes:
        assert self.session_key is not None
        return decrypt_response(self.session_key, data)

    def needs_decryption(self, data: bytes) -> bool:
        return self.authenticated and response_needs_decryption(data)


class Field(NamedTuple):
    name: str
    kind: str  # "u8" | "u16" | "u16be" | "u24" | "u32" | "u48" | "hex" | "str"
    size: int  # exact byte width of this field
    needs_nul: bool = True  # "str" fields only: must leave room for a NUL terminator


class Block(NamedTuple):
    key: str
    singleton: bool
    fields: list[Field]
    size: int
    max_instances: int


# Field layouts mirror the packed structs in include/opendisplay_structs.h exactly
# (byte-for-byte, verified against sizeof() for each struct). "reserved*" fields
# are omitted from decoded YAML when all-zero, and default to all-zero when
# absent on encode.
BLOCKS: dict[int, Block] = {
    0x01: Block(
        "system_config",
        True,
        [
            Field("ic_type", "u16", 2),
            Field("communication_modes", "u8", 1),
            Field("device_flags", "u8", 1),
            Field("pwr_pin", "u8", 1),
            Field("reserved", "hex", 15),
            Field("pwr_pin_2", "u8", 1),
            Field("pwr_pin_3", "u8", 1),
        ],
        22,
        1,
    ),
    0x02: Block(
        "manufacturer_data",
        True,
        [
            Field("manufacturer_id", "u16", 2),
            Field("board_type", "u8", 1),
            Field("board_revision", "u8", 1),
            Field("simple_config_driver_index", "u16", 2),
            Field("simple_config_display_index", "u16", 2),
            Field("simple_config_power_index", "u16", 2),
            Field("simple_config_configured_at", "u48", 6),  # 48-bit LE Unix time (s)
            Field("reserved", "hex", 6),
        ],
        22,
        1,
    ),
    0x04: Block(
        "power_option",
        True,
        [
            Field("power_mode", "u8", 1),
            Field("battery_capacity_mah", "u24", 3),  # 24-bit LE per opendisplay_structs.h
            Field("sleep_timeout_ms", "u16", 2),
            Field("tx_power", "u8", 1),
            Field("sleep_flags", "u8", 1),
            Field("battery_sense_pin", "u8", 1),
            Field("battery_sense_enable_pin", "u8", 1),
            Field("battery_sense_flags", "u8", 1),
            Field("capacity_estimator", "u8", 1),
            Field("voltage_scaling_factor", "u16", 2),
            Field("deep_sleep_current_ua", "u32", 4),
            Field("deep_sleep_time_seconds", "u16", 2),
            Field("charge_enable_pin", "u8", 1),
            Field("charge_state_pin", "u8", 1),
            Field("charger_flags", "u8", 1),
            Field("min_wake_time_seconds", "u16", 2),
            Field("screen_timeout_seconds", "u8", 1),
            Field("reserved", "hex", 4),
        ],
        30,
        1,
    ),
    0x20: Block(
        "displays",
        False,
        [
            Field("instance_number", "u8", 1),
            Field("display_technology", "u8", 1),
            Field("panel_ic_type", "u16", 2),
            Field("pixel_width", "u16", 2),
            Field("pixel_height", "u16", 2),
            Field("active_width_mm", "u16", 2),
            Field("active_height_mm", "u16", 2),
            Field("legacy_tag_type", "u16", 2),
            Field("rotation", "u8", 1),
            Field("reset_pin", "u8", 1),
            Field("busy_pin", "u8", 1),
            Field("dc_pin", "u8", 1),
            Field("cs_pin", "u8", 1),
            Field("data_pin", "u8", 1),
            Field("partial_update_support", "u8", 1),
            Field("color_scheme", "u8", 1),
            Field("transmission_modes", "u8", 1),
            Field("clk_pin", "u8", 1),
            Field("cs_pin_2", "u8", 1),
            Field("reserved_pin_3", "u8", 1),
            Field("reserved_pin_4", "u8", 1),
            Field("reserved_pin_5", "u8", 1),
            Field("reserved_pin_6", "u8", 1),
            Field("reserved_pin_7", "u8", 1),
            Field("reserved_pin_8", "u8", 1),
            Field("full_update_mC", "u16", 2),
            Field("reserved", "hex", 13),
        ],
        46,
        4,
    ),
    0x21: Block(
        "leds",
        False,
        [
            Field("instance_number", "u8", 1),
            Field("led_type", "u8", 1),
            Field("led_1_r", "u8", 1),
            Field("led_2_g", "u8", 1),
            Field("led_3_b", "u8", 1),
            Field("led_4", "u8", 1),
            Field("led_flags", "u8", 1),
            Field("reserved", "hex", 15),
        ],
        22,
        4,
    ),
    0x23: Block(
        "sensors",
        False,
        [
            Field("instance_number", "u8", 1),
            Field("sensor_type", "u16", 2),
            Field("bus_id", "u8", 1),
            Field("i2c_addr_7bit", "u8", 1),
            Field("msd_data_start_byte", "u8", 1),
            Field("reserved", "hex", 24),
        ],
        30,
        4,
    ),
    0x24: Block(
        "data_buses",
        False,
        [
            Field("instance_number", "u8", 1),
            Field("bus_type", "u8", 1),
            Field("pin_1", "u8", 1),
            Field("pin_2", "u8", 1),
            Field("pin_3", "u8", 1),
            Field("pin_4", "u8", 1),
            Field("pin_5", "u8", 1),
            Field("pin_6", "u8", 1),
            Field("pin_7", "u8", 1),
            Field("bus_speed_hz", "u32", 4),
            Field("bus_flags", "u8", 1),
            Field("pullups", "u8", 1),
            Field("pulldowns", "u8", 1),
            Field("reserved", "hex", 14),
        ],
        30,
        4,
    ),
    0x25: Block(
        "binary_inputs",
        False,
        [
            Field("instance_number", "u8", 1),
            Field("input_type", "u8", 1),
            Field("display_as", "u8", 1),
            Field("input_pin_1", "u8", 1),
            Field("input_pin_2", "u8", 1),
            Field("input_pin_3", "u8", 1),
            Field("input_pin_4", "u8", 1),
            Field("input_pin_5", "u8", 1),
            Field("input_pin_6", "u8", 1),
            Field("input_pin_7", "u8", 1),
            Field("input_pin_8", "u8", 1),
            Field("pins_used", "u8", 1),
            Field("invert", "u8", 1),
            Field("pullups", "u8", 1),
            Field("pulldowns", "u8", 1),
            Field("button_data_byte_index", "u8", 1),
            Field("power_off_flags", "u8", 1),
            Field("power_off_hold_sec", "u8", 1),
            Field("reserved", "hex", 12),
        ],
        30,
        4,
    ),
    0x26: Block(
        "wifi_config",
        True,
        [
            # firmware copies the full 32 bytes into a separate 33-byte buffer and
            # NUL-terminates independently (config_parser.cpp) - no terminator needed on the wire
            Field("ssid", "str", 32, needs_nul=False),
            Field("password", "str", 32, needs_nul=False),
            Field("encryption_type", "u8", 1),
            # unlike ssid/password, needs an on-wire NUL: config_parser.cpp's isStringFormat
            # scans server_host for an embedded 0x00 to tell a hostname from a raw IP
            Field("server_host", "str", 64),
            Field("server_port", "u16be", 2),
            Field("reserved", "hex", 29),
        ],
        160,
        1,
    ),
    0x27: Block(
        "security_config",
        True,
        [
            Field("encryption_enabled", "u8", 1),
            Field("encryption_key", "hex", 16),
            Field("session_timeout_seconds", "u16", 2),
            Field("flags", "u8", 1),
            Field("reset_pin", "u8", 1),
            Field("reserved", "hex", 43),
        ],
        64,
        1,
    ),
    0x28: Block(
        "touch_controllers",
        False,
        [
            Field("instance_number", "u8", 1),
            Field("touch_ic_type", "u16", 2),
            Field("bus_id", "u8", 1),
            Field("i2c_addr_7bit", "u8", 1),
            Field("int_pin", "u8", 1),
            Field("rst_pin", "u8", 1),
            Field("display_instance", "u8", 1),
            Field("flags", "u8", 1),
            Field("poll_interval_ms", "u8", 1),
            Field("touch_data_start_byte", "u8", 1),
            Field("enable_pin", "u8", 1),
            Field("reserved", "hex", 20),
        ],
        32,
        4,
    ),
    0x29: Block(
        "passive_buzzers",
        False,
        [
            Field("instance_number", "u8", 1),
            Field("drive_pin", "u8", 1),
            Field("enable_pin", "u8", 1),
            Field("flags", "u8", 1),
            Field("duty_percent", "u8", 1),
            Field("reserved", "hex", 27),
        ],
        32,
        4,
    ),
    0x2A: Block(
        "nfc_configs",
        False,
        [
            Field("instance_number", "u8", 1),
            Field("nfc_ic_type", "u8", 1),
            Field("bus_instance", "u8", 1),
            Field("flags", "u8", 1),
            Field("field_detect_pin", "u8", 1),
            Field("field_detect_mode", "u8", 1),
            Field("field_detect_active", "u8", 1),
            Field("field_detect_debounce_ms", "u8", 1),
            Field("power_pin", "u8", 1),
            Field("power_active", "u8", 1),
            Field("power_on_delay_ms", "u8", 1),
            Field("power_off_delay_ms", "u8", 1),
            Field("adv_button_byte_index", "u8", 1),
            Field("adv_button_button_id", "u8", 1),
            Field("reserved_pin_1", "u8", 1),
            Field("reserved_pin_2", "u8", 1),
            Field("reserved", "hex", 16),
        ],
        32,
        2,
    ),
    0x2B: Block(
        "flash_configs",
        False,
        [
            Field("instance_number", "u8", 1),
            Field("flash_ic_type", "u8", 1),
            Field("bus_instance", "u8", 1),
            Field("flags", "u8", 1),
            Field("mosi_pin", "u8", 1),
            Field("sck_pin", "u8", 1),
            Field("cs_pin", "u8", 1),
            Field("power_pin", "u8", 1),
            Field("power_active", "u8", 1),
            Field("power_on_delay_ms", "u8", 1),
            Field("power_off_delay_ms", "u8", 1),
            Field("mode", "u8", 1),
            Field("reserved", "hex", 20),
        ],
        32,
        2,
    ),
    0x2C: Block(
        "data_extended",
        True,
        [
            Field("manufacturer_name", "str", 32),
            Field("model_name", "str", 32),
            Field("serial_number", "str", 32),
            Field("friendly_name", "str", 32),
            Field("device_location", "str", 32),
            Field("device_id", "str", 32),
            Field("custom_string_1", "str", 32),
            Field("custom_string_2", "str", 32),
            Field("custom_string_3", "str", 32),
        ],
        288,
        1,
    ),
}

for _tag, _block in BLOCKS.items():
    _computed = sum(f.size for f in _block.fields)
    if _computed != _block.size:
        raise AssertionError(f"tag 0x{_tag:02X} ({_block.key}): fields sum to {_computed} bytes, expected {_block.size}")

# Known enum values, from the #define constants in src/structs.h. Values outside these
# sets only trigger a warning (not an error) on encode - the firmware may define values
# this tool doesn't know about yet.
VALID_ENUMS: dict[tuple[str, str], set[int]] = {
    ("sensors", "sensor_type"): {1, 2, 3, 4, 5},  # SENSOR_TYPE_TEMPERATURE..BQ27220
    ("displays", "color_scheme"): {0, 1, 2, 3, 4, 5, 6, 7, 8, 100, 101, 102},  # COLOR_SCHEME_MONO..BWGBRY_SPLIT, RGB565/RGB888/RGB16BPC
    ("touch_controllers", "touch_ic_type"): {0, 1},  # TOUCH_IC_NONE, TOUCH_IC_GT911
    ("power_option", "capacity_estimator"): {1, 2, 3, 4, 5},  # CAPACITY_EST_LI_ION..SEEED_LI_ION
}


def decode_fields(fields: list[Field], payload: bytes) -> dict[str, Any]:
    result: dict[str, Any] = {}
    offset = 0
    for f in fields:
        raw = payload[offset : offset + f.size]
        offset += f.size
        if f.kind == "hex":
            value: Any = raw.hex()
            nonzero = any(raw)
        elif f.kind == "str":
            value = raw.split(b"\x00", 1)[0].decode("utf-8", errors="replace")
            nonzero = bool(value)
        else:
            value = int.from_bytes(raw, "big" if f.kind.endswith("be") else "little")
            nonzero = value != 0
        if f.name.startswith("reserved") and not nonzero:
            continue
        result[f.name] = value
    return result


def encode_fields(block_key: str, fields: list[Field], data: dict[str, Any], expected_size: int) -> bytes:
    known_names = {f.name for f in fields}
    unknown = set(data) - known_names
    if unknown:
        raise ValueError(f"{block_key}: unknown field(s) {sorted(unknown)!r}")

    out = bytearray()
    for f in fields:
        value = data.get(f.name)
        if f.kind == "hex":
            raw = bytes.fromhex(value) if value is not None else bytes(f.size)
        elif f.kind == "str":
            encoded = str(value if value is not None else "").encode("utf-8")
            max_len = f.size - 1 if f.needs_nul else f.size
            if len(encoded) > max_len:
                raise ValueError(f"{block_key}.{f.name}: value too long for {f.size} bytes")
            raw = encoded + bytes(f.size - len(encoded))
        else:
            int_value = int(value if value is not None else 0)
            if int_value < 0:
                raise ValueError(f"{block_key}.{f.name}: value {int_value} is negative, field is unsigned")
            try:
                raw = int_value.to_bytes(f.size, "big" if f.kind.endswith("be") else "little")
            except OverflowError:
                max_value = (1 << (f.size * 8)) - 1
                raise ValueError(
                    f"{block_key}.{f.name}: value {int_value} does not fit in {f.size} byte(s) "
                    f"({f.kind}, max {max_value})"
                ) from None
            enum_values = VALID_ENUMS.get((block_key, f.name))
            if enum_values is not None and int_value not in enum_values:
                print(
                    f"WARNING: {block_key}.{f.name}: value {int_value} is not a known enum value "
                    f"({sorted(enum_values)})",
                    file=sys.stderr,
                )
        if len(raw) != f.size:
            raise ValueError(f"{block_key}.{f.name}: expects {f.size} bytes, got {len(raw)}")
        out += raw
    if len(out) != expected_size:
        raise AssertionError(f"encoded size {len(out)} != expected {expected_size}")
    return bytes(out)


def decode_packet(packet: bytes) -> dict[str, Any]:
    singles: dict[int, dict[str, Any]] = {}
    lists: dict[int, list[dict[str, Any]]] = {}
    offset = 3  # 2-byte length + 1 version byte
    end = len(packet) - 2  # trailing 2-byte CRC
    while offset < end:
        block_start = offset
        offset += 1  # separator
        if offset >= end:
            raise ValueError(f"truncated block header at offset {block_start}")
        tag = packet[offset]
        offset += 1
        block = BLOCKS.get(tag)
        if block is None:
            raise ValueError(f"unknown tag 0x{tag:02X} at offset {block_start}")
        if offset + block.size > end:
            raise ValueError(f"truncated payload for tag 0x{tag:02X} at offset {block_start}")
        payload = packet[offset : offset + block.size]
        offset += block.size
        fields = decode_fields(block.fields, payload)
        if block.singleton:
            if tag in singles:
                print(f"WARNING: duplicate singleton tag 0x{tag:02X} ({block.key}), using the last one", file=sys.stderr)
            singles[tag] = fields
        else:
            lists.setdefault(tag, []).append(fields)

    doc: dict[str, Any] = {"version": packet[2]}
    for tag in sorted(BLOCKS):
        block = BLOCKS[tag]
        if block.singleton:
            if tag in singles:
                doc[block.key] = singles[tag]
        elif tag in lists:
            doc[block.key] = lists[tag]
    return doc


def repack(packet: bytes) -> bytes:
    """Recompute the length prefix and CRC-16 trailer for a packet body."""
    body = bytearray(packet)
    total_len = len(body)
    body[0] = total_len & 0xFF
    body[1] = (total_len >> 8) & 0xFF
    crc = outer_packet_crc(bytes(body[:-2]))
    body[-2] = crc & 0xFF
    body[-1] = (crc >> 8) & 0xFF
    return bytes(body)


def encode_packet(doc: dict[str, Any]) -> bytes:
    known_keys = {"version"} | {block.key for block in BLOCKS.values()}
    unknown_keys = set(doc) - known_keys
    if unknown_keys:
        raise ValueError(f"unknown top-level key(s) {sorted(unknown_keys)!r}")

    body = bytearray([0x00, 0x00, int(doc.get("version", 1)) & 0xFF])
    for tag in sorted(BLOCKS):
        block = BLOCKS[tag]
        if block.singleton:
            value = doc.get(block.key)
            if value is not None:
                body += bytes([0x00, tag])
                body += encode_fields(block.key, block.fields, value, block.size)
        else:
            items = doc.get(block.key) or []
            if len(items) > block.max_instances:
                raise ValueError(f"{block.key}: {len(items)} instances exceeds max {block.max_instances}")
            for item in items:
                body += bytes([0x00, tag])
                body += encode_fields(block.key, block.fields, item, block.size)
    body += b"\x00\x00"
    if len(body) > 0xFFFF:
        raise ValueError(f"resulting packet too large ({len(body)} bytes)")
    packet = repack(bytes(body))
    validate_packet(packet)
    return packet


def decode_msd_payload(payload: bytes) -> dict[str, Any]:
    """Decode the 16-byte MSD buffer built by updatemsdata() (display_service.cpp)."""
    if len(payload) != 16:
        raise ValueError(f"MSD payload must be 16 bytes, got {len(payload)}")
    company_id = payload[0] | (payload[1] << 8)
    status = payload[15]
    battery_raw_10mv = ((status & 0x01) << 8) | payload[14]
    return {
        "company_id": company_id,
        "dynamic_data_hex": payload[2:13].hex(),
        "temperature_c": (payload[13] / 2.0) - 40.0,
        "battery_voltage_v": battery_raw_10mv / 100.0 if battery_raw_10mv > 0 else None,
        "battery_raw_10mv": battery_raw_10mv,
        "reboot_flag": bool((status >> 1) & 0x01),
        "connection_requested": bool((status >> 2) & 0x01),
        "loop_counter": (status >> 4) & 0x0F,
    }


def format_hex(packet: bytes) -> str:
    return " ".join(f"{b:02X}" for b in packet)


def auto_int(value: str) -> int:
    return int(value, 0)


def dump_yaml(doc: dict[str, Any]) -> str:
    class IndentedDumper(yaml.SafeDumper):
        def increase_indent(self, flow: bool = False, indentless: bool = False) -> None:
            return super().increase_indent(flow, False)

    return "---\n" + yaml.dump(doc, Dumper=IndentedDumper, sort_keys=False, default_flow_style=False)


def load_yaml_doc(text: str) -> dict[str, Any]:
    data = yaml.safe_load(text)
    if not isinstance(data, dict):
        raise ValueError("YAML document must be a mapping at the top level")
    return data


def _status(message: str) -> None:
    print(message, file=sys.stderr, flush=True)


class _BleCtx:
    """A connected BLE session: raw transport plus optional encryption state."""

    def __init__(self, client: Any, session: BleSession | None):
        self.client = client
        self.session = session
        self.notify_handler: Callable[[bytes], None] | None = None
        # Bench instrumentation, off for every ordinary command. It receives frames BEFORE
        # decryption, because what the bench has to prove is about the bytes on air: a replayed
        # frame is byte-identical to its original, and a decrypted view cannot show that.
        self.raw_log: Callable[[str, bytes], None] | None = None
        self.notify_enabled = False

    def _log_raw(self, direction: str, data: bytes) -> None:
        if self.raw_log:
            self.raw_log(direction, data)

    def _on_notify(self, _sender: object, payload: bytearray) -> None:
        data = bytes(payload)
        self._log_raw("d2h", data)
        if self.session and self.session.needs_decryption(data):
            try:
                data = self.session.decrypt(data)
            except Exception:
                return
        if self.notify_handler:
            self.notify_handler(data)

    async def send_command(self, cmd_hi: int, cmd_lo: int, payload: bytes = b"") -> None:
        if self.session and self.session.authenticated:
            wire = self.session.encrypt(cmd_hi, cmd_lo, payload)
        else:
            wire = bytes([cmd_hi, cmd_lo]) + payload
        self._log_raw("h2d", wire)
        await self.client.write_gatt_char(CHAR_UUID, wire)

    async def seal_command(self, cmd_hi: int, cmd_lo: int, payload: bytes = b"") -> bytes:
        """Seal a command and RETURN the wire bytes without sending them.

        The counter advances exactly once, here. Everything the replay test rests on follows from
        that: the caller sends these same bytes twice, so the second copy carries the same session
        id and the same counter and is a replay by construction. Sealing twice would produce a
        fresh nonce, and that tests PIPE duplicate handling rather than the replay window."""
        if not (self.session and self.session.authenticated):
            raise RuntimeError("seal_command requires an authenticated session")
        return self.session.encrypt(cmd_hi, cmd_lo, payload)

    async def send_raw(self, wire: bytes) -> None:
        """Write bytes exactly as given -- no sealing, no counter movement."""
        self._log_raw("h2d", wire)
        await self.client.write_gatt_char(CHAR_UUID, wire)

    async def set_notify(self, enabled: bool) -> None:
        """Subscribe or unsubscribe the notification characteristic.

        WITHHOLDING IS THE POINT. With the CCCD disabled the device's radio HAL reports RETRY
        rather than sending, which is the arm that exercises egress backpressure -- and it does so
        without a fault hook or a firmware build that differs from the one under test."""
        if enabled == self.notify_enabled:
            return
        if enabled:
            await self.client.start_notify(CHAR_UUID, self._on_notify)
        else:
            await self.client.stop_notify(CHAR_UUID)
        self.notify_enabled = enabled


async def _ble_authenticate(ctx: _BleCtx, session: BleSession, timeout: float) -> None:
    response_box: dict[str, bytes] = {}
    event = asyncio.Event()
    ctx.notify_handler = lambda data: (response_box.__setitem__("data", data), event.set())

    async def exchange(payload: bytes) -> bytes:
        event.clear()
        await ctx.client.write_gatt_char(CHAR_UUID, payload)
        try:
            await asyncio.wait_for(event.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            raise RuntimeError("timed out waiting for authentication response") from None
        return response_box["data"]

    try:
        resp1 = await exchange(bytes([0x00, 0x50, 0x00]))
        status1 = resp1[2] if len(resp1) > 2 else 0xFF
        if status1 == 0x03:
            raise RuntimeError("device does not have encryption enabled")
        if status1 == 0x04:
            raise RuntimeError("authentication rate-limited by device, try again later")
        if status1 != 0x00 or len(resp1) < 23:
            raise RuntimeError(f"authentication challenge failed (status 0x{status1:02X})")
        server_nonce = resp1[3:19]
        device_id = resp1[19:23]

        client_nonce = os.urandom(16)
        challenge_response = compute_challenge_response(session.master_key, server_nonce, client_nonce, device_id)
        resp2 = await exchange(bytes([0x00, 0x50]) + client_nonce + challenge_response)
        status2 = resp2[2] if len(resp2) > 2 else 0xFF
        if status2 == 0x01:
            raise RuntimeError("authentication failed (wrong key)")
        if status2 != 0x00 or len(resp2) < 19:
            raise RuntimeError(f"authentication response failed (status 0x{status2:02X})")
        server_response = resp2[3:19]

        session.session_key = derive_session_key(session.master_key, client_nonce, server_nonce, device_id)
        session.session_id = derive_session_id(session.session_key, client_nonce, server_nonce)
        expected = compute_server_response(session.session_key, server_nonce, client_nonce, device_id)
        if expected != server_response:
            raise RuntimeError("mutual authentication failed (unexpected server response)")
        session.authenticated = True
        session.counter = 0
        _status("Authenticated (encrypted session established).")
    finally:
        ctx.notify_handler = None


@asynccontextmanager
async def _ble_connection(addr: str, key: bytes | None = None, auth_timeout: float = 10.0) -> AsyncIterator[_BleCtx]:
    session = BleSession(master_key=key) if key else None
    _status(f"Connecting to {addr}...")
    async with bleak.BleakClient(addr) as client:
        _status("Connected.")
        ctx = _BleCtx(client, session)
        await client.start_notify(CHAR_UUID, ctx._on_notify)
        ctx.notify_enabled = True
        try:
            if session:
                _status("Authenticating...")
                await _ble_authenticate(ctx, session, auth_timeout)
            yield ctx
        finally:
            if ctx.notify_enabled:
                await client.stop_notify(CHAR_UUID)
                ctx.notify_enabled = False


async def _do_read_config(ctx: _BleCtx, timeout: float) -> bytes:
    chunks: dict[int, bytes] = {}
    state: dict[str, int] = {}
    done = asyncio.Event()
    error = False

    def handle_notify(payload: bytes) -> None:
        nonlocal error
        if len(payload) < 1:
            return
        if payload[0] == 0xFF:
            error = True
            done.set()
            return
        if len(payload) < 4:
            return
        chunk_num = payload[2] | (payload[3] << 8)
        if chunk_num == 0:
            if len(payload) < 6:
                return
            state["total_len"] = payload[4] | (payload[5] << 8)
            chunks[0] = bytes(payload[6:])
        else:
            chunks[chunk_num] = bytes(payload[4:])
        if "total_len" in state and sum(len(b) for b in chunks.values()) >= state["total_len"]:
            done.set()

    ctx.notify_handler = handle_notify
    _status("Reading config...")
    try:
        await ctx.send_command(0x00, 0x40)
        try:
            await asyncio.wait_for(done.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            raise RuntimeError("timed out waiting for config read response") from None
    finally:
        ctx.notify_handler = None

    if error:
        raise RuntimeError("device reported an error reading config (0x0040)")
    if "total_len" not in state:
        raise RuntimeError("no response from device (config read failed)")
    if sorted(chunks) != list(range(len(chunks))):
        raise RuntimeError(f"missing chunk(s) in config read response, got chunk numbers {sorted(chunks)}")
    ordered = bytearray()
    for i in sorted(chunks):
        ordered += chunks[i]
    packet = bytes(ordered[: state["total_len"]])
    validate_packet(packet)
    _status(f"Read {len(packet)} bytes ({len(chunks)} chunk(s)).")
    return packet


async def _do_write_config(ctx: _BleCtx, config: bytes, timeout: float) -> None:
    if len(config) > 4096:
        raise ValueError(f"config too large ({len(config)} bytes, max 4096)")
    ack = asyncio.Event()
    nack = False

    def handle_notify(payload: bytes) -> None:
        nonlocal nack
        if len(payload) < 1:
            return
        nack = payload[0] == 0xFF
        ack.set()

    ctx.notify_handler = handle_notify

    async def send_and_wait(cmd_byte: int, body: bytes) -> None:
        nonlocal nack
        ack.clear()
        nack = False
        await ctx.send_command(0x00, cmd_byte, body)
        try:
            await asyncio.wait_for(ack.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            raise RuntimeError(f"timed out waiting for ack on command 0x{cmd_byte:02X}") from None
        if nack:
            raise RuntimeError(f"device rejected command 0x{cmd_byte:02X}")

    _status(f"Writing config ({len(config)} bytes)...")
    try:
        if len(config) <= WRITE_CHUNK_SIZE:
            await send_and_wait(0x41, config)
        else:
            total = len(config)
            num_chunks = 1 + -(-(total - WRITE_CHUNK_SIZE) // WRITE_CHUNK_SIZE)
            header = bytes([total & 0xFF, (total >> 8) & 0xFF]) + config[:WRITE_CHUNK_SIZE]
            await send_and_wait(0x41, header)
            _status(f"Sent chunk 1/{num_chunks}.")
            offset = WRITE_CHUNK_SIZE
            chunk_num = 1
            while offset < total:
                chunk = config[offset : offset + WRITE_CHUNK_SIZE]
                await send_and_wait(0x42, chunk)
                offset += len(chunk)
                chunk_num += 1
                _status(f"Sent chunk {chunk_num}/{num_chunks}.")
    finally:
        ctx.notify_handler = None
    _status("Write complete.")


async def _do_read_msd(ctx: _BleCtx, timeout: float) -> bytes:
    response_box: dict[str, bytes] = {}
    done = asyncio.Event()

    def handle_notify(payload: bytes) -> None:
        if len(payload) < 1:
            return
        response_box["data"] = payload
        done.set()

    ctx.notify_handler = handle_notify
    _status("Reading MSD data...")
    try:
        await ctx.send_command(0x00, 0x44)
        try:
            await asyncio.wait_for(done.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            raise RuntimeError("timed out waiting for MSD read response") from None
    finally:
        ctx.notify_handler = None

    data = response_box.get("data")
    if data is None:
        raise RuntimeError("no response from device (MSD read failed)")
    # Two known error shapes: a bare 0xFF generic-error byte (per the 0x0040 convention),
    # or a 3-byte {cmd_hi, cmd_lo, status} NACK with status 0xFE/0xFF (e.g. not authenticated).
    if len(data) == 1 and data[0] == 0xFF:
        raise RuntimeError("device reported a generic error reading MSD data (0x0044)")
    if len(data) == 3 and data[0] == 0x00 and data[1] == 0x44 and data[2] in (0xFE, 0xFF):
        raise RuntimeError(f"device rejected MSD read (status 0x{data[2]:02X} - possibly not authenticated)")
    if len(data) < 2 + 16 or data[0] != 0x00 or data[1] != 0x44:
        raise RuntimeError(f"unexpected MSD response (expected 18 bytes starting with 00 44), got: {data.hex()}")
    return data[2:18]


async def ble_read_msd(addr: str, timeout: float = 10.0, key: bytes | None = None) -> bytes:
    async with _ble_connection(addr, key=key) as ctx:
        return await _do_read_msd(ctx, timeout)


async def ble_read_config(addr: str, timeout: float = 10.0, key: bytes | None = None) -> bytes:
    async with _ble_connection(addr, key=key) as ctx:
        return await _do_read_config(ctx, timeout)


async def ble_write_config(addr: str, config: bytes, timeout: float = 5.0, key: bytes | None = None) -> None:
    async with _ble_connection(addr, key=key) as ctx:
        await _do_write_config(ctx, config, timeout)


async def _ble_read_modify_write(
    addr: str, modify_fn: Callable[[dict[str, Any]], None], key: bytes | None = None, timeout: float = 10.0
) -> bytes:
    async with _ble_connection(addr, key=key) as ctx:
        packet = await _do_read_config(ctx, timeout)
        doc = decode_packet(packet)
        modify_fn(doc)
        new_packet = encode_packet(doc)
        await _do_write_config(ctx, new_packet, timeout)
        return new_packet


# ============================================================================================
# BENCH ONLY -- `dispatch-gate`
#
# The two hardware rows nothing else can produce. Both are in the C12 plan's exit matrix, and both
# fail QUIETLY if driven approximately, which is why they are a tool rather than a checklist.
#
#   OD-S1 REPLAY. A replayed encrypted 0x0081 must draw NO response, be logged, and leave the
#   transfer alive. The frame has to be byte-identical to one the device already accepted -- same
#   session id, same counter -- so the tool seals ONCE and writes those retained bytes twice.
#   Re-sealing the same sequence number produces a fresh nonce and tests PIPE duplicate handling
#   instead, which is the easy mistake and looks identical in a log.
#
#   EGRESS BACKPRESSURE. Disabling the CCCD makes the device's radio HAL report RETRY rather than
#   send, so queued replies accumulate instead of being dropped. Re-enabling must deliver the whole
#   config read intact. No fault hook, no altered firmware: the build under test is the build that
#   ships.
#
# WHAT THE TOOL WILL NOT DO is claim a pass it did not observe. Silence proves nothing on its own --
# a disconnected notify path is also silent -- so the replay phase ends with a deliberately corrupt
# frame that MUST draw a plaintext NACK. Without that control, "no notification" is unfalsifiable.
# ============================================================================================

PIPE_VERSION = 1
PIPE_ACK_EVERY = 1
PIPE_MAX_ON_WIRE = 244
# [cmd:2][session id + counter:16][plaintext length:1][PIPE seq:1][CCM tag:12].
# `--chunk-bytes` is only the DATA bytes, while PIPE_MAX_FRAME is the complete sealed value.
PIPE_ENCRYPTED_DATA_OVERHEAD = 32


class _FrameLog:
    """Every frame, both directions, timestamped from a monotonic clock and recorded BEFORE
    decryption. The raw bytes are the evidence: a replay is only a replay if the second copy is
    identical to the first, and a decrypted view cannot show that."""

    def __init__(self) -> None:
        self._t0 = time.monotonic()
        self.records: list[dict[str, Any]] = []

    def __call__(self, direction: str, data: bytes) -> None:
        self.records.append({
            "t_ms": round((time.monotonic() - self._t0) * 1000.0, 3),
            "dir": direction,
            "len": len(data),
            "hex": data.hex(),
        })

    def since(self, t_ms: float, direction: str = "d2h") -> list[dict[str, Any]]:
        return [r for r in self.records if r["dir"] == direction and r["t_ms"] >= t_ms]

    def now_ms(self) -> float:
        return round((time.monotonic() - self._t0) * 1000.0, 3)


def _pipe_start_request(total_size: int, window: int, ack_every: int, max_frame: int) -> bytes:
    """ver(1) flags(1) req_window(1) req_ack_every(1) client_max_frame(2 LE) total_size(4 LE)."""
    return (bytes([PIPE_VERSION, 0x00, window, ack_every])
            + max_frame.to_bytes(2, "little")
            + total_size.to_bytes(4, "little"))


def _replies_matching(log: _FrameLog, since_ms: float, prefix: bytes) -> list[dict[str, Any]]:
    want = prefix.hex()
    return [r for r in log.since(since_ms) if r["hex"].startswith(want)]


async def _bench_replay(ctx: _BleCtx, log: _FrameLog, args: argparse.Namespace) -> dict[str, Any]:
    """Phase 1: OD-S1.

    EVERY STEP MUST BE POSITIVELY OBSERVED. Silence after a replay is not a result on its own: the
    session gate drops a replayed nonce BEFORE PIPE state is consulted, so a transfer that never
    opened is silent too, and a phase that checked only for silence would pass having proved
    nothing. So the transfer is established, the first frame is confirmed accepted, the control is
    matched byte-for-byte, and the transfer is carried through to a rendered refresh -- which is
    the half that shows the replay left it ALIVE rather than merely quiet.
    """
    out: dict[str, Any] = {"phase": "od-s1-replay"}
    chunk = bytes([0x41]) * args.chunk_bytes
    frames_total = (args.total_size + args.chunk_bytes - 1) // args.chunk_bytes

    # --- 1. open the transfer, and require an ACK ------------------------------------------
    _status("OD-S1: opening an encrypted PIPE transfer...")
    t = log.now_ms()
    await ctx.send_command(0x00, 0x80,
                           _pipe_start_request(args.total_size, args.window,
                                               PIPE_ACK_EVERY, args.max_frame))
    await asyncio.sleep(args.settle)
    start_ack = _replies_matching(log, t, bytes([0x00, 0x80]))
    out["start_acked"] = len(start_ack) > 0
    out["start_replies"] = log.since(t)
    if not out["start_acked"]:
        # A NACKed START leaves no transfer, and every later step would then be measuring the
        # gate rather than PIPE. Stop rather than report a pass built on that.
        out["pass"] = False
        out["stopped_at"] = "start-not-acked"
        return out

    # --- 2. seal ONE frame, send it, and require it to be accepted ---------------------------
    counter_before = ctx.session.counter if ctx.session else -1
    wire = await ctx.seal_command(0x00, 0x81, bytes([0x00]) + chunk)
    counter_after = ctx.session.counter if ctx.session else -1
    out["counter_delta_for_one_frame"] = counter_after - counter_before

    t_first = log.now_ms()
    await ctx.send_raw(wire)
    await asyncio.sleep(args.settle)
    first_sack = _replies_matching(log, t_first, bytes([0x00, 0x81]))
    out["first_frame_acked"] = len(first_sack) > 0
    if not out["first_frame_acked"]:
        out["pass"] = False
        out["stopped_at"] = "first-data-not-acked"
        return out

    # --- 3. the replay: identical bytes, no wire reply, and a FRESH device-side report --------
    # The target rate-limits replay/out-of-window telemetry in a five-second bucket. Waiting it
    # out makes a missing line a failure of this stimulus rather than residue from an earlier event.
    _status("OD-S1: waiting beyond the replay telemetry throttle...")
    await asyncio.sleep(args.telemetry_wait)
    replay_log_mark = _device_log_checkpoint(args.device_log) if args.device_log else {}
    _status("OD-S1: re-sending the IDENTICAL sealed bytes...")
    t_replay = log.now_ms()
    await ctx.send_raw(wire)                      # same session id, same counter, same tag
    await asyncio.sleep(args.observe)
    replay_replies = log.since(t_replay)
    out["replay_replies"] = replay_replies
    out["replay_drew_silence"] = (len(replay_replies) == 0)
    out["replay_telemetry"] = (
        _device_log_match(args.device_log, replay_log_mark,
                          ("0x0081", "decrypt failed", "nonce_reason=3"), "replay")
        if args.device_log else {"found": False, "error": "no --device-log supplied"})

    # --- 4. the control, matched exactly -----------------------------------------------------
    # A tag failure is tamper evidence and keeps its NACK, so the gate answers [00][81][FF]
    # (shared/core/od_gate.c queue_status). Accepting "any notification" would let a late SACK from
    # step 2, or an unrelated frame, stand in for it -- and then the silence above is unfalsifiable.
    _status("OD-S1: control -- a corrupted tag must draw the plaintext 0081ff...")
    bad = bytearray(await ctx.seal_command(0x00, 0x81, bytes([0x01]) + chunk))
    bad[-1] ^= 0xFF
    t_ctrl = log.now_ms()
    await ctx.send_raw(bytes(bad))
    await asyncio.sleep(args.observe)
    out["control_replies"] = log.since(t_ctrl)
    out["control_answered_exactly"] = any(r["hex"] == "0081ff" for r in log.since(t_ctrl))

    # --- 5. continuation: the transfer must still be alive -----------------------------------
    # THE HALF THAT PROVES SURVIVAL. Silence is the required behaviour only if the upload then
    # completes; a device that went quiet because the transfer died would pass steps 3 and 4.
    _status(f"OD-S1: continuing with fresh counters ({frames_total - 1} frame(s)) "
            "through automatic END...")
    t_cont = log.now_ms()
    remaining = args.total_size - args.chunk_bytes
    seq = 1
    while remaining > 0:
        part = chunk[:min(args.chunk_bytes, remaining)]
        await ctx.send_command(0x00, 0x81, bytes([seq & 0xFF]) + part)
        await asyncio.sleep(args.frame_gap)
        remaining -= len(part)
        seq += 1
    # Uncompressed full-frame PIPE auto-completes on the final DATA and emits 0x82 itself. Sending
    # another explicit END after that is a protocol error and receives a hard [FF][82] NACK.
    await asyncio.sleep(args.refresh_wait)
    out["end_acked"] = len(_replies_matching(log, t_cont, bytes([0x00, 0x82]))) > 0
    # 0x73 refresh complete, 0x74 refresh timed out. Both mean the panel was driven; only their
    # absence means the transfer did not finish.
    out["refresh_reported"] = (len(_replies_matching(log, t_cont, bytes([0x00, 0x73]))) > 0
                               or len(_replies_matching(log, t_cont, bytes([0x00, 0x74]))) > 0)
    out["continuation_replies"] = log.since(t_cont)

    out["pass"] = bool(out["start_acked"] and out["first_frame_acked"]
                       and out["replay_drew_silence"] and out["replay_telemetry"].get("found")
                       and out["control_answered_exactly"]
                       and out["end_acked"] and out["refresh_reported"]
                       and out["counter_delta_for_one_frame"] == 1)
    return out


async def _read_config_bytes(ctx: _BleCtx, timeout: float) -> bytes:
    """A plain config read, used to take the baseline the withheld read is compared against."""
    return await _do_read_config(ctx, timeout)


def _device_log_checkpoint(path: str) -> dict[str, int | None]:
    """Identify the end of the log before a stimulus, so old evidence cannot satisfy a new run."""
    try:
        with open(path, "rb") as fh:
            st = os.fstat(fh.fileno())
            fh.seek(0, os.SEEK_END)
            return {"device": st.st_dev, "inode": st.st_ino, "offset": fh.tell()}
    except OSError:
        # A capture process may create the file only after the run starts. In that case every byte
        # in the eventual file is new evidence.
        return {"device": None, "inode": None, "offset": 0}


def _device_log_match(path: str, mark: dict[str, int | None], required: tuple[str, ...],
                      label: str) -> dict[str, Any]:
    """Find a line appended after `mark` containing every case-insensitive required fragment."""
    out: dict[str, Any] = {"log_path": path, f"{label}_line": None, "found": False}
    try:
        with open(path, "r", errors="replace") as fh:
            st = os.fstat(fh.fileno())
            offset = int(mark.get("offset") or 0)
            same_file = (mark.get("device") == st.st_dev and mark.get("inode") == st.st_ino)
            # A replaced or truncated capture starts a new epoch; none of its content predates the
            # checkpoint. Otherwise only scan the appended suffix.
            if same_file and st.st_size >= offset:
                fh.seek(offset)
            needles = tuple(s.lower() for s in required)
            for line in fh:
                if all(s in line.lower() for s in needles):
                    out[f"{label}_line"] = line.strip()
                    out["found"] = True
                    break
    except OSError as exc:
        out["error"] = str(exc)
    return out


def _canary_in_device_log(path: str, mark: dict[str, int | None], canary_hex: str) -> dict[str, Any]:
    """Scan NEW operator-captured device-log bytes for the unknown-opcode line.

    THIS IS THE ONLY EVIDENCE THAT THE CANARY REACHED DISPATCH, and it cannot come from the wire:
    an unknown opcode is answered with silence by design, and the host is unsubscribed anyway. RX
    is FIFO, so the line proves the CONFIG_READ queued ahead of it had already been dispatched --
    which is what puts the device in the RETRY arm rather than merely idle.

    Without the log the phase cannot pass. A tool that inferred it from a complete config read
    afterwards would be unable to tell "queued after dispatch" from "processed late", and those are
    the two outcomes the row exists to separate.

    A line from an earlier run is not evidence. `mark` is captured before the commands are sent and
    the scan is restricted to the suffix appended after it."""
    return _device_log_match(path, mark, (canary_hex, "unknown"), "canary")


async def _bench_withhold(ctx: _BleCtx, log: _FrameLog, args: argparse.Namespace) -> dict[str, Any]:
    """Phase 2: egress backpressure. Withhold notifications, drive a multi-frame read, restore."""
    out: dict[str, Any] = {"phase": "withhold-notify"}
    chunks: dict[int, bytes] = {}
    state: dict[str, int] = {}
    done = asyncio.Event()

    # THE BASELINE, read normally first. Comparing the withheld read against known bytes is what
    # makes "exact reassembly" an assertion; contiguous chunk indexes summing to an advertised
    # length would also hold for a read that came back subtly wrong.
    _status("backpressure: taking a baseline config read...")
    try:
        baseline = await _read_config_bytes(ctx, args.timeout)
    except (RuntimeError, ValueError) as exc:
        out["pass"] = False
        out["stopped_at"] = f"baseline-read-failed: {exc}"
        return out
    out["baseline_len"] = len(baseline)

    def handle(payload: bytes) -> None:
        if len(payload) < 4 or payload[0] == 0xFF:
            return
        n = payload[2] | (payload[3] << 8)
        if n == 0:
            if len(payload) < 6:
                return
            state["total_len"] = payload[4] | (payload[5] << 8)
            chunks[0] = bytes(payload[6:])
        else:
            chunks[n] = bytes(payload[4:])
        if "total_len" in state and sum(len(b) for b in chunks.values()) >= state["total_len"]:
            done.set()

    _status("backpressure: disabling notifications...")
    log_mark = _device_log_checkpoint(args.device_log) if args.device_log else {}
    await ctx.set_notify(False)
    t_withheld = log.now_ms()

    await ctx.send_command(0x00, 0x40)
    await ctx.send_command(0x00, 0x60)          # the canary, FIFO behind the read
    await asyncio.sleep(args.withhold)
    out["frames_during_withhold"] = len(log.since(t_withheld))

    # The device log is captured by the operator over RTT/serial; the tool reads it rather than
    # taking anyone's word for it.
    out["canary"] = (_canary_in_device_log(args.device_log, log_mark, "0x0060") if args.device_log
                     else {"found": False, "error": "no --device-log supplied"})

    _status("backpressure: re-enabling notifications...")
    ctx.notify_handler = handle
    await ctx.set_notify(True)
    try:
        await asyncio.wait_for(done.wait(), timeout=args.timeout)
        out["config_reassembled"] = True
    except asyncio.TimeoutError:
        out["config_reassembled"] = False
    finally:
        ctx.notify_handler = None

    ordered = bytearray()
    for i in sorted(chunks):
        ordered += chunks[i]
    got = bytes(ordered[: state.get("total_len", 0)])
    out["chunks"] = sorted(chunks)
    out["contiguous"] = (sorted(chunks) == list(range(len(chunks)))) if chunks else False
    out["bytes_match_baseline"] = (got == baseline)
    out["withheld_len"] = len(got)

    out["pass"] = bool(out["frames_during_withhold"] == 0
                       and out["canary"].get("found")
                       and out["config_reassembled"] and out["contiguous"]
                       and out["bytes_match_baseline"])
    return out


# Fields whose VALUE is raw wire bytes. They are kept deliberately -- the replay row's entire
# claim is that two frames were byte-identical, and a redacted transcript cannot show that -- so
# the limitation is named here rather than contradicted by a comment elsewhere. A sealed frame
# carries its session id and counter in the clear by construction, so an evidence file IS
# session-identifying and must be handled as such.
_WIRE_FIELDS = ("hex",)
_SECRET_HINTS = ("key", "master", "nonce", "psk", "secret", "addr", "address", "mac", "token")


def _redact(value: Any) -> Any:
    """Redact secret-looking fields anywhere in the record, including inside lists.

    The earlier version recursed through dicts only, so `phases` and `frames` -- both lists --
    carried their nested objects through untouched. That is the shape of a leak that survives
    review: the top level looks scrubbed."""
    if isinstance(value, dict):
        out: dict[str, Any] = {}
        for k, v in value.items():
            if k in _WIRE_FIELDS:
                out[k] = v                       # raw wire, kept on purpose; see above
            elif any(h in k.lower() for h in _SECRET_HINTS):
                out[k] = "<redacted>"
            else:
                out[k] = _redact(v)
        return out
    if isinstance(value, list):
        return [_redact(v) for v in value]
    return value


async def _do_dispatch_gate(ctx: _BleCtx, args: argparse.Namespace) -> dict[str, Any]:
    log = _FrameLog()
    ctx.raw_log = log
    phases: list[dict[str, Any]] = []
    try:
        if args.phase in ("all", "replay"):
            phases.append(await _bench_replay(ctx, log, args))
        if args.phase in ("all", "withhold"):
            phases.append(await _bench_withhold(ctx, log, args))
    finally:
        ctx.raw_log = None
    record = {
        "tool": "od-device-cli dispatch-gate",
        "schema": "opendisplay-bench-evidence/1",
        "phases": phases,
        "frames": log.records,
        # Provenance the evidence commit needs. Filled by the operator rather than guessed: the
        # tool cannot know which board or which firmware SHA it is talking to, and inventing them
        # is worse than leaving them blank.
        "target": args.target,
        "firmware_sha": args.firmware_sha,
        # Stated in the artifact rather than assumed by whoever reads it: the transcript holds raw
        # sealed frames, which carry session ids and counters in the clear.
        "handling": "contains raw wire frames; session-identifying, treat as sensitive",
        "pass": all(p.get("pass") for p in phases) if phases else False,
    }
    return _redact(record)


def cmd_dispatch_gate(args: argparse.Namespace) -> int:
    key = _parse_key_arg(args.key)
    if key is None:
        _status("dispatch-gate needs --key: both phases require an authenticated session.")
        return 2

    # REFUSED BEFORE CONNECTING, because these produce a run that looks like a pass. A zero or
    # mismatched total is rejected by production PIPE START, and the replay phase would then be
    # measuring the session gate with no transfer open -- silent, and meaningless.
    if args.phase in ("all", "replay"):
        if args.total_size <= 0 or args.total_size > 0xFFFFFFFF:
            _status("dispatch-gate: --total-size must be the panel's expected transfer size. "
                    "A START whose total does not match is rejected, and the replay phase would "
                    "then prove nothing.")
            return 2
        if args.max_frame <= PIPE_ENCRYPTED_DATA_OVERHEAD or args.max_frame > 0xFFFF:
            _status("dispatch-gate: --max-frame cannot carry an encrypted PIPE DATA frame.")
            return 2
        wire_limit = min(args.max_frame, PIPE_MAX_ON_WIRE)
        if (args.chunk_bytes <= 0
                or args.chunk_bytes + PIPE_ENCRYPTED_DATA_OVERHEAD > wire_limit):
            _status("dispatch-gate: encrypted PIPE DATA is --chunk-bytes plus 32 bytes of "
                    "opcode/session/length/sequence/tag overhead; the complete frame must fit "
                    f"the negotiated {wire_limit}-byte on-wire ceiling.")
            return 2
        if args.total_size <= args.chunk_bytes:
            _status("dispatch-gate: --total-size must exceed --chunk-bytes so the first accepted "
                    "DATA frame leaves the transfer live for the replay stimulus.")
            return 2
    if not args.device_log:
        _status("dispatch-gate: --device-log is required. The replay phase must observe a fresh "
                "nonce-replay report, and the withhold phase must observe its dispatch canary; "
                "neither property is visible on the wire.")
        return 2

    async def run() -> dict[str, Any]:
        async with _ble_connection(args.address, key=key) as ctx:
            return await _do_dispatch_gate(ctx, args)

    record = asyncio.run(run())
    text = json.dumps(record, indent=2)
    _write_output(text, Path(args.output) if args.output else None)
    for p in record["phases"]:
        _status(f"{p['phase']}: {'PASS' if p.get('pass') else 'FAIL'}")
    return 0 if record["pass"] else 1


def _parse_key_arg(key_hex: str | None) -> bytes | None:
    if not key_hex:
        return None
    try:
        key = bytes.fromhex(key_hex)
    except ValueError:
        raise ValueError("--key must be a hex string") from None
    if len(key) != 16:
        raise ValueError(f"--key must be exactly 32 hex characters (16 bytes), got {len(key_hex)}")
    return key


def _write_output(text: str, output: Path | None) -> None:
    if output:
        output.write_text(text)
    else:
        print(text, end="" if text.endswith("\n") else "\n")


# ---------------------------------------------------------------------------- nfc-read ------
#
# THE ONLY WAY TO EXERCISE THE READ HALF OF 0x0083. py-opendisplay implements no NFC_SUB_READ
# (commands.py:103, "not built here"), so before this existed no NFC read could be driven through
# CMD_NFC_ENDPOINT at all.
#
# AN INDEPENDENT NFC READER IS NOT A SUBSTITUTE, and the distinction is the whole reason this is
# here rather than a phone app. A reader talks to the tag directly: it bypasses the command
# endpoint, dispatch, the od_nfc_app seam and response framing, so it can confirm what a WRITE left
# on the tag but proves nothing about the path under test. Its role in the hardware rows is
# stimulus and oracle. A read row backed by a reader alone is not a pass.

NFC_CMD_HI = 0x00
NFC_CMD_LO = 0x83
NFC_SUB_READ = 0x00
NFC_STATUS_READ_DATA = 0x80

NFC_ERRORS = {
    0x01: "MALFORMED",
    0x02: "READ_FAILED",
    0x03: "TAG_WRITE_FAILED",
    0x04: "UNKNOWN_SUBCMD",
    0x05: "INVALID_REC_TYPE",
    0x06: "BAD_TOTAL_LEN",
    0x07: "CHUNK_NO_START",
    0x08: "CHUNK_OVERFLOW",
    0x09: "END_LEN_MISMATCH",
}
NFC_REC_TYPES = {0: "TEXT", 1: "URI", 2: "WELL_KNOWN_RAW", 3: "MIME", 4: "RAW_NDEF"}


def _decode_nfc_read(frame: bytes) -> dict[str, Any]:
    """Decode one 0x0083 reply. Returns a dict; never raises on a short or malformed frame."""
    if len(frame) < 3:
        return {"ok": False, "reason": f"short reply ({len(frame)} bytes)", "raw": frame.hex()}
    status, cmd = frame[0], frame[1]
    if cmd != 0x83:
        return {"ok": False, "reason": f"not a 0x83 reply (cmd=0x{cmd:02x})", "raw": frame.hex()}
    if status == 0xFF:
        err = frame[3] if len(frame) >= 4 else None
        return {
            "ok": False,
            "nack": True,
            "error_code": err,
            "error": NFC_ERRORS.get(err, f"unknown(0x{err:02x})" if err is not None else "absent"),
            "raw": frame.hex(),
        }
    if frame[2] != NFC_STATUS_READ_DATA:
        return {"ok": False, "reason": f"unexpected status byte 0x{frame[2]:02x}", "raw": frame.hex()}
    if len(frame) < 6:
        return {"ok": False, "reason": "read reply truncated before its length field",
                "raw": frame.hex()}
    rec_type = frame[3]
    declared = (frame[4] << 8) | frame[5]
    data = frame[6:]
    result = {
        "ok": True,
        "rec_type": rec_type,
        "rec_type_name": NFC_REC_TYPES.get(rec_type, f"unknown({rec_type})"),
        "declared_len": declared,
        "actual_len": len(data),
        "data": data.hex(),
    }
    # THE LENGTH FIELD IS CHECKED, not trusted. A device that framed the header correctly and
    # carried the wrong number of bytes would otherwise read as a clean pass.
    if declared != len(data):
        result["ok"] = False
        result["reason"] = f"declared {declared} bytes, frame carried {len(data)}"
    return result


async def _do_nfc_read(ctx: _BleCtx, timeout: float) -> dict[str, Any]:
    got: list[bytes] = []
    done = asyncio.Event()

    def on_frame(frame: bytes) -> None:
        if len(frame) >= 2 and frame[1] == 0x83:
            got.append(frame)
            done.set()

    ctx.notify_handler = on_frame
    await ctx.send_command(NFC_CMD_HI, NFC_CMD_LO, bytes([NFC_SUB_READ]))
    try:
        await asyncio.wait_for(done.wait(), timeout)
    except asyncio.TimeoutError:
        # SILENCE IS A RESULT, and a meaningful one: it is what a capability-off target answers,
        # and what py-opendisplay turns into NfcNotSupportedError. Reported rather than raised.
        return {"ok": False, "silent": True,
                "reason": f"no 0x83 reply within {timeout:.1f}s "
                          "(expected on a target built with OD_CAP_NFC=0)"}
    finally:
        ctx.notify_handler = None
    return _decode_nfc_read(got[0])


def cmd_nfc_read(args: argparse.Namespace) -> int:
    key = _parse_key_arg(args.key)

    async def run() -> dict[str, Any]:
        async with _ble_connection(args.addr, key=key) as ctx:
            return await _do_nfc_read(ctx, args.timeout)

    result = asyncio.run(run())
    result["encrypted"] = key is not None
    print(json.dumps(result, indent=2))
    if args.expect_silence:
        return 0 if result.get("silent") else 1
    return 0 if result.get("ok") else 1


def cmd_read_config(args: argparse.Namespace) -> int:
    key = _parse_key_arg(args.key)
    packet = asyncio.run(ble_read_config(args.addr, key=key))
    doc = decode_packet(packet)
    _write_output(dump_yaml(doc), args.output)
    return 0


def cmd_write_config(args: argparse.Namespace) -> int:
    key = _parse_key_arg(args.key)
    doc = load_yaml_doc(args.input.read_text())
    packet = encode_packet(doc)
    asyncio.run(ble_write_config(args.addr, packet, key=key))
    print(f"Wrote config to {args.addr} ({len(packet)} bytes)")
    return 0


def cmd_decode_config(args: argparse.Namespace) -> int:
    packet = read_hex_arg(args.config_hex, str(args.config_file) if args.config_file else None)
    doc = decode_packet(packet)
    _write_output(dump_yaml(doc), args.output)
    return 0


def cmd_encode_config(args: argparse.Namespace) -> int:
    doc = load_yaml_doc(args.input.read_text())
    packet = encode_packet(doc)
    print(format_hex(packet))
    return 0


def cmd_read_msd(args: argparse.Namespace) -> int:
    key = _parse_key_arg(args.key)
    payload = asyncio.run(ble_read_msd(args.addr, key=key))
    if args.raw:
        print(payload.hex())
        return 0
    info = decode_msd_payload(payload)
    voltage = info["battery_voltage_v"]
    print(f"Battery voltage: {f'{voltage:.2f} V' if voltage is not None else 'unknown (raw=0 - unconfigured or not yet sampled)'}")
    print(f"Chip temperature: {info['temperature_c']:.1f} C")
    print(f"Reboot flag: {info['reboot_flag']}")
    print(f"Connection requested: {info['connection_requested']}")
    print(f"Loop counter: {info['loop_counter']}")
    print(f"Dynamic data (bytes 2-12): {info['dynamic_data_hex']}")
    return 0


def cmd_add_sensor(args: argparse.Namespace) -> int:
    if not args.addr and not (args.config_hex or args.config_file):
        raise ValueError("specify --addr or --config-hex/--config-file")
    key = _parse_key_arg(args.key)

    def add_sensor(doc: dict[str, Any]) -> None:
        sensors = doc.setdefault("sensors", [])
        if len(sensors) >= BLOCKS[0x23].max_instances:
            raise ValueError(f"packet already has {len(sensors)} sensors (max {BLOCKS[0x23].max_instances})")
        sensors.append(
            {
                "instance_number": args.instance,
                "sensor_type": args.sensor_type,
                "bus_id": args.bus_id,
                "i2c_addr_7bit": args.i2c_addr,
                "msd_data_start_byte": args.msd_start,
            }
        )

    if args.addr:
        new_packet = asyncio.run(_ble_read_modify_write(args.addr, add_sensor, key=key))
        doc = decode_packet(new_packet)
        print(f"Wrote updated config to {args.addr} ({len(new_packet)} bytes, {len(doc.get('sensors', []))} sensor(s))")
    else:
        packet = read_hex_arg(args.config_hex, str(args.config_file) if args.config_file else None)
        doc = decode_packet(packet)
        add_sensor(doc)
        new_packet = encode_packet(doc)
        print(format_hex(new_packet))
    return 0


def add_hex_input_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--config-hex", help="Existing config packet as hex bytes")
    parser.add_argument("--config-file", type=Path, help="Read the existing packet from a file")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p_read = sub.add_parser(
        "read-config",
        help="Read a device's config over BLE and print it as YAML\n"
        "  e.g. od-device-cli.py read-config --addr AA:BB:CC:DD:EE:FF -o config.yaml\n",
        description="Read a device's config over BLE (command 0x0040) and decode it to YAML.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
Examples:
    od-device-cli.py read-config --addr AA:BB:CC:DD:EE:FF -o config.yaml
        Read the config and save it as YAML instead of printing to stdout.

    od-device-cli.py read-config --addr AA:BB:CC:DD:EE:FF --key 1e6d01ca00803339d31ee98ca052da71
        Same, for a device with BLE encryption enabled (security_config.encryption_enabled) -
        authenticates via the 0x0050 handshake first, then prints YAML to stdout.
""",
    )
    p_read.add_argument("--addr", required=True, help="BLE device address")
    p_read.add_argument("-o", "--output", type=Path, help="Write YAML to a file instead of stdout")
    p_read.add_argument("--key", metavar="HEX", help="16-byte master key (32 hex chars) for encrypted BLE")
    p_read.set_defaults(func=cmd_read_config)

    p_nfc = sub.add_parser(
        "nfc-read",
        help="Read the tag through CMD_NFC_ENDPOINT (0x0083) and decode the reply\n"
        "  e.g. od-device-cli.py nfc-read --addr AA:BB:CC:DD:EE:FF\n",
        description="Read the device's NDEF record through the 0x0083 command endpoint.\n\n"
        "This drives the path under test -- command endpoint, dispatch, the od_nfc_app seam and "
        "response framing. An independent NFC reader talks to the tag directly and bypasses all "
        "of it, so it can confirm what a WRITE committed but cannot stand in for this.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
Examples:
    od-device-cli.py nfc-read --addr AA:BB:CC:DD:EE:FF
        Plaintext read. Prints the decoded record type, declared length and payload as JSON.

    od-device-cli.py nfc-read --addr AA:BB:CC:DD:EE:FF --key 1e6d01ca00803339d31ee98ca052da71
        The same read inside an encrypted session, for the rows that require both.

    od-device-cli.py nfc-read --addr AA:BB:CC:DD:EE:FF --expect-silence
        For a target built with OD_CAP_NFC=0: succeeds only if NOTHING is answered, which is
        the behaviour py-opendisplay turns into NfcNotSupportedError.
""",
    )
    p_nfc.add_argument("--addr", required=True, help="BLE device address")
    p_nfc.add_argument("--key", metavar="HEX", help="16-byte master key (32 hex chars) for encrypted BLE")
    p_nfc.add_argument("--timeout", type=float, default=5.0, help="seconds to wait for the reply")
    p_nfc.add_argument("--expect-silence", action="store_true",
                       help="invert the verdict: succeed only if no reply arrives (capability-off)")
    p_nfc.set_defaults(func=cmd_nfc_read)

    p_write = sub.add_parser(
        "write-config",
        help="Write a YAML config to a device over BLE\n"
        "  e.g. od-device-cli.py write-config --addr AA:BB:CC:DD:EE:FF --input config.yaml\n",
        description="Encode a YAML config and push it to a device over BLE (commands 0x0041/0x0042).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
Examples:
    od-device-cli.py write-config --addr AA:BB:CC:DD:EE:FF --input config.yaml
        Push config.yaml to the device, chunked automatically if it exceeds 200 bytes.

    od-device-cli.py write-config --addr AA:BB:CC:DD:EE:FF --input config.yaml --key 1e6d01ca00803339d31ee98ca052da71
        Same, authenticating first for a device with BLE encryption enabled.
""",
    )
    p_write.add_argument("--addr", required=True, help="BLE device address")
    p_write.add_argument("-i", "--input", required=True, type=Path, help="YAML config file")
    p_write.add_argument("--key", metavar="HEX", help="16-byte master key (32 hex chars) for encrypted BLE")
    p_write.set_defaults(func=cmd_write_config)

    p_decode = sub.add_parser(
        "decode-config",
        help="Decode a hex config packet to YAML (offline)\n"
        '  e.g. od-device-cli.py decode-config --config-hex "1D 00 01 ... EC 58"\n',
        description="Decode a config packet's hex bytes to YAML, offline - no device or BLE required.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
Examples:
    od-device-cli.py decode-config --config-hex "1D 00 01 ... EC 58"
        Decode a hex string pasted directly on the command line.

    od-device-cli.py decode-config --config-file dump.bin -o config.yaml
        Decode a binary config dump file and save the result as YAML.
""",
    )
    add_hex_input_args(p_decode)
    p_decode.add_argument("-o", "--output", type=Path, help="Write YAML to a file instead of stdout")
    p_decode.set_defaults(func=cmd_decode_config)

    p_encode = sub.add_parser(
        "encode-config",
        help="Encode a YAML config to a hex packet (offline)\n"
        "  e.g. od-device-cli.py encode-config --input config.yaml\n",
        description="Encode a YAML config to hex packet bytes, offline - no device or BLE required.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
Examples:
    od-device-cli.py encode-config --input config.yaml
        Print the encoded packet as hex bytes to stdout, e.g. for OPENDISPLAY_FACTORY_CONFIG_HEX.
""",
    )
    p_encode.add_argument("-i", "--input", required=True, type=Path, help="YAML config file")
    p_encode.set_defaults(func=cmd_encode_config)

    p_add = sub.add_parser(
        "add-sensor",
        help="Add a sensor_data (0x23) block, over BLE or offline\n"
        "  e.g. od-device-cli.py add-sensor --addr AA:BB:CC:DD:EE:FF --sensor-type 0x0003 --bus-id 0\n",
        description="Add a sensor_data (0x23) block to a config, over a live BLE device or offline hex.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
Examples:
    od-device-cli.py add-sensor --addr AA:BB:CC:DD:EE:FF --sensor-type 0x0003 --bus-id 0
        Read the device's config over BLE, add an AXP2101 sensor entry, and write it back
        in one step.

    od-device-cli.py add-sensor --config-hex "1D 00 01 ... EC 58" --sensor-type 0x0005 --bus-id 0
        Offline: add a sensor entry to a hex packet and print the updated hex, without
        touching a live device.
""",
    )
    p_add.add_argument("--addr", help="BLE device address (reads and writes back over BLE)")
    add_hex_input_args(p_add)
    p_add.add_argument("--instance", type=auto_int, default=0, help="instance_number (default: 0)")
    p_add.add_argument("--sensor-type", type=auto_int, required=True, help="sensor_type, e.g. 0x0003 for AXP2101")
    p_add.add_argument("--bus-id", type=auto_int, required=True, help="I2C bus instance id")
    p_add.add_argument("--i2c-addr", type=auto_int, default=0xFF, help="i2c_addr_7bit (default: 0xFF)")
    p_add.add_argument("--msd-start", type=auto_int, default=0xFF, help="msd_data_start_byte (default: 0xFF)")
    p_add.add_argument("--key", metavar="HEX", help="16-byte master key (32 hex chars) for encrypted BLE")
    p_add.set_defaults(func=cmd_add_sensor)

    p_msd = sub.add_parser(
        "read-msd",
        help="Read live MSD data (battery voltage, chip temperature) via command 0x0044\n"
        "  e.g. od-device-cli.py read-msd --addr AA:BB:CC:DD:EE:FF\n",
        description="Read the device's live 16-byte MSD buffer (command 0x0044) and decode battery "
        "voltage, chip temperature, and status bits.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
Examples:
    od-device-cli.py read-msd --addr AA:BB:CC:DD:EE:FF
        Print decoded battery voltage/temperature/status for an unencrypted device.

    od-device-cli.py read-msd --addr AA:BB:CC:DD:EE:FF --key 1e6d01ca00803339d31ee98ca052da71 --raw
        Same, for a device with BLE encryption enabled, printing the raw 16-byte payload
        as hex instead of decoding it.
""",
    )
    p_msd.add_argument("--addr", required=True, help="BLE device address")
    p_msd.add_argument("--key", metavar="HEX", help="16-byte master key (32 hex chars) for encrypted BLE")
    p_msd.add_argument("--raw", action="store_true", help="Print the raw 16-byte MSD payload as hex instead of decoding")
    p_msd.set_defaults(func=cmd_read_msd)

    p_gate = sub.add_parser(
        "dispatch-gate",
        help="BENCH ONLY: drive the two hardware rows nothing else can produce\n"
        "  e.g. od-device-cli.py dispatch-gate --addr AA:BB:CC:DD:EE:FF --key <hex> "
        "--total-size 48000 --device-log device.log\n",
        description="Run the C12 exit-matrix rows that need deliberate stimulus rather than a "
        "normal session: the OD-S1 encrypted-replay silence path, and egress backpressure driven "
        "by withholding notifications. Writes a JSON evidence record with secrets redacted.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
This is a BENCH tool. It deliberately replays an already-sealed frame and unsubscribes mid-session;
neither is something a conforming client does, and neither belongs in a script that talks to a
deployed device.

WHY IT EXISTS. Both rows fail quietly when driven approximately:

  * A replay must be BYTE-IDENTICAL to a frame the device already accepted. Re-sealing the same
    PIPE sequence number produces a fresh nonce and tests duplicate handling instead -- which looks
    the same in a log and proves nothing about the replay window. This tool seals once and writes
    the retained bytes twice.

  * Silence is not evidence on its own: a disconnected notify path is also silent. The replay phase
    therefore requires a fresh device-side nonce-replay report and uses a deliberately corrupted
    tag that MUST draw a plaintext NACK. If either control is absent, the run fails.

Examples:
    od-device-cli.py dispatch-gate --addr AA:BB:CC:DD:EE:FF --key <hex> --total-size 48000 \\
        --device-log device.log --target xiao_nrf52840 --firmware-sha ab4ff36 -o evidence.json
        Both phases, recording provenance for the evidence commit.

    od-device-cli.py dispatch-gate --addr ... --key <hex> --phase withhold --device-log device.log
        Backpressure only; needs no transfer and no --total-size.
""",
    )
    p_gate.add_argument("--addr", required=True, dest="address", help="BLE device address")
    p_gate.add_argument("--key", metavar="HEX", required=True,
                        help="16-byte master key (32 hex chars); both phases need a session")
    p_gate.add_argument("--phase", choices=("all", "replay", "withhold"), default="all")
    p_gate.add_argument("--total-size", type=int, default=0,
                        help="bytes the PIPE transfer declares; must match what the panel expects")
    p_gate.add_argument("--window", type=int, default=4, help="requested PIPE window")
    p_gate.add_argument("--max-frame", type=int, default=244, help="requested client max frame")
    p_gate.add_argument("--chunk-bytes", type=int, default=64, help="DATA payload size per frame")
    p_gate.add_argument("--settle", type=float, default=0.5,
                        help="seconds to wait for a reply that IS expected")
    p_gate.add_argument("--observe", type=float, default=2.0,
                        help="bounded window for a reply that must NOT arrive; too short turns a "
                             "slow device into a false pass")
    p_gate.add_argument("--telemetry-wait", type=float, default=5.1,
                        help="seconds to wait before replaying, beyond the target's 5-second "
                             "replay-log throttle")
    p_gate.add_argument("--withhold", type=float, default=2.0,
                        help="seconds to hold notifications off while commands are dispatched")
    p_gate.add_argument("--timeout", type=float, default=15.0, help="config reassembly timeout")
    p_gate.add_argument("--frame-gap", type=float, default=0.02,
                        help="pause between continuation DATA frames")
    p_gate.add_argument("--refresh-wait", type=float, default=90.0,
                        help="seconds to wait for END ack and the refresh status; a panel refresh "
                             "can take a minute, and a short wait reads as a failed transfer")
    p_gate.add_argument("--device-log", default=None,
                        help="operator-captured RTT/serial log, scanned only after per-phase "
                             "checkpoints for fresh replay telemetry and the unknown-opcode canary")
    p_gate.add_argument("--target", default=None, help="board id, recorded in the evidence file")
    p_gate.add_argument("--firmware-sha", default=None, help="firmware SHA under test, recorded")
    p_gate.add_argument("-o", "--output", help="write the JSON evidence record to a file")
    p_gate.set_defaults(func=cmd_dispatch_gate)

    args = parser.parse_args()
    try:
        return args.func(args)
    except (ValueError, RuntimeError, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
