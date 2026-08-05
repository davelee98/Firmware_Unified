#!/usr/bin/env bash
# Reset the board over the debug probe and stream SEGGER RTT to stdout.
#
# Usage:
#   ./rtt.sh                     # L15 target (nrf54l), stream until Ctrl-C
#   BUILD_DIR=build-lm20 ./rtt.sh
#   BOARD=xiao_nrf54lm20a/nrf54lm20a/cpuapp ./rtt.sh
#   DURATION=15 ./rtt.sh         # exit after N seconds (0 = forever)
#   TARGET=nrf54lm20a ./rtt.sh   # override pyocd target

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BOARD="${BOARD:-}"
BUILD_DIR="${BUILD_DIR:-build}"
DURATION="${DURATION:-0}"
TARGET="${TARGET:-}"

if [[ -z "${TARGET}" ]]; then
  if [[ "${BOARD}" == *lm20* || "${BUILD_DIR}" == *lm20* ]]; then
    TARGET=nrf54lm20a
  else
    TARGET=nrf54l
  fi
fi

# Do not source ncs-env.sh here: the NCS toolchain ships an older pyocd that
# often lacks nrf54lm20a. Use the user's normal python3/pyocd on PATH.

if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 not found" >&2
  exit 1
fi

export OD_RTT_TARGET="${TARGET}"
export OD_RTT_DURATION="${DURATION}"

# Prefer a pyocd that knows the target; try primary then nrf54l fallback in Python.
exec python3 - <<'PY'
import os
import sys
import time

try:
    from pyocd.core.helpers import ConnectHelper
except ImportError:
    print("pyocd not installed (pip install pyocd)", file=sys.stderr)
    sys.exit(1)

target_name = os.environ.get("OD_RTT_TARGET", "nrf54l")
try:
    duration = float(os.environ.get("OD_RTT_DURATION", "0"))
except ValueError:
    duration = 0.0

def find_rtt_cb(target):
    # Firmware puts the RTT control block in early SRAM; scan in chunks.
    start, end, chunk = 0x20000000, 0x20020000, 4096
    needle = b"SEGGER RTT"
    addr = start
    while addr < end:
        n = min(chunk, end - addr)
        data = bytes(target.read_memory_block8(addr, n))
        idx = data.find(needle)
        if idx >= 0:
            return addr + idx
        # Overlap so a split across chunk boundary is still found
        addr += chunk - len(needle)
    return None

print(f"Connecting ({target_name})…", file=sys.stderr)
try:
    session_cm = ConnectHelper.session_with_chosen_probe(
        target_override=target_name,
        options={"frequency": 4000000},
    )
except Exception as exc:
    if target_name != "nrf54l":
        print(f"{exc}\nRetrying with target nrf54l…", file=sys.stderr)
        target_name = "nrf54l"
        session_cm = ConnectHelper.session_with_chosen_probe(
            target_override=target_name,
            options={"frequency": 4000000},
        )
    else:
        raise

with session_cm as session:
    target = session.board.target
    cb = find_rtt_cb(target)
    if cb is None:
        print("SEGGER RTT control block not found in RAM", file=sys.stderr)
        sys.exit(1)

    # aUp[0] follows id[16], MaxNumUpBuffers, MaxNumDownBuffers
    up0 = cb + 24
    buf_ptr = target.read32(up0 + 4)
    size = target.read32(up0 + 8)
    if not buf_ptr or not size:
        print(f"RTT up-buffer invalid (ptr={buf_ptr:#x} size={size})", file=sys.stderr)
        sys.exit(1)

    print(
        f"RTT @ {cb:#x}  buf={buf_ptr:#x} size={size} — reset + stream"
        + (f" ({duration:g}s)" if duration > 0 else " (Ctrl-C to stop)"),
        file=sys.stderr,
    )

    target.reset_and_halt()
    target.write32(up0 + 16, 0)  # RdOff
    target.resume()

    end = time.time() + duration if duration > 0 else None
    try:
        while end is None or time.time() < end:
            wroff = target.read32(up0 + 12)
            rdoff = target.read32(up0 + 16)
            if wroff == rdoff:
                time.sleep(0.02)
                continue
            if wroff > rdoff:
                chunk = bytes(target.read_memory_block8(buf_ptr + rdoff, wroff - rdoff))
            else:
                chunk = bytes(target.read_memory_block8(buf_ptr + rdoff, size - rdoff))
                chunk += bytes(target.read_memory_block8(buf_ptr, wroff))
            target.write32(up0 + 16, wroff)
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
    except KeyboardInterrupt:
        print("\n[rtt] stopped", file=sys.stderr)
PY
