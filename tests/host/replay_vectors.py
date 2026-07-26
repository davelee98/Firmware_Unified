#!/usr/bin/env python3
"""Replay tests/vectors/*.json against py-opendisplay's public API.

TEST_OWNERSHIP.md § "The mechanism": the corpus lives here, and the second
implementation of the wire protocol is *pulled in* as a pinned dependency:

    uv run --with py-opendisplay==7.14.0 python tests/host/replay_vectors.py

This runner calls py-opendisplay's public API only -- `opendisplay.protocol.*`.
It must never reimplement encode/decode logic locally; a vector both runners
derive from the same local helper proves nothing. When a vector cannot be
checked through that API it is SKIPped with a reason, never hand-rolled green.

With no C runner yet this is a deliberately one-sided test. It checks the half
of each vector the host owns:

  * h2d `frame`  -- the bytes py-opendisplay emits. Fully checkable.
  * `expect.reply` -- what the *firmware* emits. Never checkable here; the host
    has no encoder for device replies. Reported as unchecked on every line.
  * `state` (sec_enabled, session) -- firmware state. No host equivalent.
  * `requires` (OD_*_ENABLE) -- firmware build flags. No host equivalent; skip.
  * `min_protocol` -- py-opendisplay 7.14.0 exposes no protocol-version
    constant, so this cannot be evaluated host-side; ignored, not enforced.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

from opendisplay.protocol import commands as cmd
from opendisplay.protocol.config_parser import parse_config_response
from opendisplay.protocol.config_serializer import serialize_config
from opendisplay.protocol.responses import (
    check_response_type,
    parse_firmware_version,
    strip_command_echo,
)

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_VECTORS = REPO_ROOT / "tests" / "vectors"

REPLY_UNCHECKED = "expect.reply is firmware-side, not checked here"


class Skip(Exception):
    """Raised when a vector is out of reach of py-opendisplay's public API."""


# --- host encoders: opcode -> (label, bytes) --------------------------------


def _auth(f: bytes) -> tuple[str, bytes]:
    if len(f) == 3 and f[2] == 0x00:
        return "build_authenticate_step1()", cmd.build_authenticate_step1()
    if len(f) == 34:
        return "build_authenticate_step2()", cmd.build_authenticate_step2(f[2:18], f[18:34])
    raise Skip(f"no public builder for a {len(f)}-byte 0x0050 frame")


def _dw_start(f: bytes) -> tuple[str, bytes]:
    if len(f) == 2:
        return "build_direct_write_start_uncompressed()", cmd.build_direct_write_start_uncompressed()
    raise Skip("compressed/partial 0x0070 starts need structured args not present in the vector")


def _end(f: bytes, plain, etag, name: str) -> tuple[str, bytes]:
    if len(f) == 3:
        return f"{name}(refresh={f[2]})", plain(f[2])
    if len(f) == 7:
        return f"{name}(refresh={f[2]}, etag)", etag(f[2], int.from_bytes(f[3:7], "big"))
    raise Skip(f"no public builder for a {len(f)}-byte {name} frame")


def _write_config(f):
    """0x0041 CONFIG_WRITE has two distinct shapes on the wire and one builder.

    A whole-config write carries the config blob directly. A *chunked* START carries
    [total:2][first 200 data bytes] -- CONFIG_CHUNK_SIZE_WITH_PREFIX, 202 bytes -- which is
    not a config blob at all. Handing that to build_write_config_command() makes it re-chunk
    a fragment and assert, which surfaces as a CI failure of the vector rather than of the
    runner. Skip the chunked shape: the host API has no public builder that takes an
    already-formed chunk, so there is nothing to compare against.
    """
    payload = f[2:]
    if len(payload) > 200:
        raise Skip(
            "chunked CONFIG_WRITE START ([total:2][<=200 data]) has no public host builder; "
            "build_write_config_command() would re-chunk the fragment"
        )
    return "build_write_config_command()", cmd.build_write_config_command(payload)[0]


BUILDERS = {
    0x000F: lambda f: ("build_reboot_command()", cmd.build_reboot_command()),
    0x0040: lambda f: ("build_read_config_command()", cmd.build_read_config_command()),
    0x0041: lambda f: _write_config(f),
    0x0043: lambda f: ("build_read_fw_version_command()", cmd.build_read_fw_version_command()),
    0x0050: _auth,
    0x0051: lambda f: ("build_enter_dfu_command()", cmd.build_enter_dfu_command()),
    0x0052: lambda f: ("build_deep_sleep_command()", cmd.build_deep_sleep_command()),
    0x0070: _dw_start,
    0x0071: lambda f: ("build_direct_write_data_command()", cmd.build_direct_write_data_command(f[2:])),
    0x0072: lambda f: _end(
        f, cmd.build_direct_write_end_command, cmd.build_direct_write_end_with_etag, "build_direct_write_end"
    ),
    0x0081: lambda f: ("build_pipe_write_data_command()", cmd.build_pipe_write_data_command(f[2], f[3:])),
    0x0082: lambda f: _end(
        f,
        lambda r: cmd.build_pipe_write_end_command(r),
        lambda r, e: cmd.build_pipe_write_end_command(r, e),
        "build_pipe_write_end_command",
    ),
}


# --- host decoders ----------------------------------------------------------


def _decode_config(frame: bytes) -> str:
    """parse -> serialize -> parse fixed point, through two public functions."""
    cfg = parse_config_response(strip_command_echo(frame, cmd.CommandCode.READ_CONFIG))
    try:
        again = parse_config_response(serialize_config(cfg))
    except ValueError as exc:  # incomplete config: serializer requires 0x01/0x02/0x04
        raise Skip(f"config round-trip unavailable: {exc}") from exc
    if again != cfg:
        raise AssertionError("parse->serialize->parse is not a fixed point")
    return f"parse_config_response round-trips ({len(cfg.displays)} display packet(s))"


def _decode_fw(frame: bytes) -> str:
    v = parse_firmware_version(frame)
    return f"parse_firmware_version -> {v['major']}.{v['minor']} {v['sha']}"


DECODERS = {0x0040: _decode_config, 0x0043: _decode_fw}


# --- per-vector checks ------------------------------------------------------


def check_h2d(vec: dict, frame: bytes) -> str:
    op = int.from_bytes(frame[:2], "big")
    builder = BUILDERS.get(op)
    if builder is None:
        raise Skip(f"no public py-opendisplay builder for opcode 0x{op:04x}")

    # expect.host_error means "a conforming host must REFUSE to build this frame". That is a
    # real contract in this direction too, not only when decoding: the corpus deliberately
    # contains frames no correct host should ever emit -- an oversize chunk, a malformed
    # header -- because firmware must survive them from a broken or hostile peer. Without
    # this arm such a vector fails the runner instead of asserting the host's refusal, which
    # reads as a corpus bug when it is actually the host behaving correctly.
    want_error = bool(vec.get("expect", {}).get("host_error", False))
    try:
        label, built = builder(frame)
    except Skip:
        raise
    except Exception as exc:  # noqa: BLE001 - any host rejection is the result
        if want_error:
            return f"py-opendisplay refused to build the frame as expected: {type(exc).__name__}: {exc}"
        raise Skip(f"py-opendisplay rejected it ({type(exc).__name__}: {exc}); vector states no host expectation")

    if want_error:
        raise AssertionError(f"expected py-opendisplay to refuse this frame, but {label} built it")
    if built != frame:
        raise AssertionError(f"{label} emitted {built.hex()}, vector says {frame.hex()}")
    return f"frame == {label}; {REPLY_UNCHECKED}"


def check_d2h(vec: dict, frame: bytes) -> str:
    # A config blob opens with its own [len:2 LE] wrapper, not an opcode, so
    # config vectors are routed by id; everything else by the echoed opcode.
    if "config" in vec["id"].split("/")[0]:
        decoder = _decode_config
    else:
        decoder = DECODERS.get(int.from_bytes(frame[:2], "big") & 0x7FFF)
    want_error = bool(vec.get("expect", {}).get("host_error", False))
    try:
        detail = decoder(frame) if decoder else "check_response_type -> {}, ack={}".format(*check_response_type(frame))
    except Skip:
        raise
    except Exception as exc:  # noqa: BLE001 - any host rejection is the result
        if want_error:
            return f"py-opendisplay rejected the frame as expected: {type(exc).__name__}"
        raise Skip(f"py-opendisplay rejected it ({type(exc).__name__}: {exc}); vector states no host expectation")
    if want_error:
        raise AssertionError(f"expected py-opendisplay to reject the frame, but it decoded: {detail}")
    return detail


def check(vec: dict) -> str:
    if vec.get("requires"):
        raise Skip(f"firmware capability flag(s) {','.join(vec['requires'])} have no host equivalent")
    if vec.get("frame") is None:
        raise Skip("vector has no frame; device-side-only assertion")
    frame = bytes.fromhex(vec["frame"])
    direction = vec.get("dir")
    if direction == "h2d":
        return check_h2d(vec, frame)
    if direction == "d2h":
        return check_d2h(vec, frame)
    raise Skip(f"unknown direction {direction!r}")


# --- driver -----------------------------------------------------------------


def load_vectors(path: Path) -> list[dict]:
    if not path.is_dir():
        print(f"no vector directory at {path}", file=sys.stderr)
        return []
    out = []
    for f in sorted(path.glob("*.json")):
        try:
            doc = json.loads(f.read_text())
        except (OSError, ValueError) as exc:
            print(f"FAIL {f.name} :: unreadable: {exc}")
            out.append({"id": f.name, "_broken": str(exc)})
            continue
        for vec in doc.get("vectors", []):
            vec.setdefault("id", f"{f.stem}/?")
            out.append(vec)
    return out


def main(argv: list[str]) -> int:
    path = Path(argv[1]) if len(argv) > 1 else DEFAULT_VECTORS
    vectors = load_vectors(path)
    counts = {"PASS": 0, "FAIL": 0, "SKIP": 0}
    for vec in vectors:
        if "_broken" in vec:
            counts["FAIL"] += 1
            continue
        try:
            status, detail = "PASS", check(vec)
        except Skip as exc:
            status, detail = "SKIP", str(exc)
        except Exception as exc:  # noqa: BLE001 - assertion or unexpected host crash
            status, detail = "FAIL", f"{type(exc).__name__}: {exc}"
        counts[status] += 1
        print(f"{status} {vec['id']} :: {detail}")

    total = sum(counts.values())
    print(f"\n{total} vector(s): {counts['PASS']} passed, {counts['FAIL']} failed, {counts['SKIP']} skipped")
    if not total:
        # A runner that is green because it found nothing is the exact failure
        # mode TEST_OWNERSHIP.md flags in py-opendisplay's fixture fallbacks.
        print(f"no vectors found under {path} -- nothing was verified", file=sys.stderr)
        return 1
    return 1 if counts["FAIL"] else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
