#!/usr/bin/env bash
# Reset the board over the debug probe and stream SEGGER RTT to stdout.
#
# Usage:
#   ./rtt.sh lm20                     # XIAO nRF54LM20A, stream until Ctrl-C
#   ./rtt.sh l15                      # XIAO nRF54L15
#   PROFILE=debug ./rtt.sh lm20       # read the -debug build instead
#   DURATION=15 ./rtt.sh lm20         # exit after N seconds (0 = forever)
#   OD_RTT_CB=0x20000470 ./rtt.sh lm20  # skip discovery, use this control block
#   BUILD_DIR=... TARGET=... ./rtt.sh   # bypass resolution entirely
#   OD_RTT_RESOLVE_ONLY=1 ./rtt.sh lm20 # print what it would read, touch no hardware
#
# THE VARIANT PICKS THE ELF, AND THE ELF MUST MATCH WHAT IS FLASHED. The control block
# address is read from ${BUILD_DIR}/zephyr/zephyr/zephyr.elf, and the same directory
# supplies the SRAM bounds and the pyocd target. Point them at another board's build and
# every step still "works": the address is wrong, the symbol lookup fails quietly, and the
# fallback scan then runs against the wrong memory map. Resolving all three from the
# registry the flash scripts use is what keeps them in step.
#
# Do NOT reach for `pyocd rtt` instead: it searches the target's whole declared
# RAM region, and pyocd's nrf54lm20a map claims 512 KB (0x20000000+0x80000)
# while cpuapp SRAM ends at 0x2007fc00. The last 1 KB is unmapped, so the search
# dies with "Memory transfer fault (FAULT ACK) @ 0x2007fc00-0x2007ffff". If you
# must use it, bound the range: pyocd rtt -t nrf54lm20a -a 0x20000000 -s 0x20000

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/target-registry.sh
source "${SCRIPT_DIR}/scripts/target-registry.sh"

DURATION="${DURATION:-0}"
PROFILE="${PROFILE:-battery}"
VARIANT="${1:-}"
if [[ $# -gt 0 ]]; then
  shift
fi

if [[ -z "${VARIANT}" && -z "${BUILD_DIR:-}" ]]; then
  echo "usage: ./rtt.sh <l15|lm20>   (PROFILE=debug for the -debug build)" >&2
  exit 2
fi

if [[ -n "${VARIANT}" ]]; then
  od_target_load "${VARIANT}"
  if [[ "${OD_TARGET_BOOTLOADER}" != mcuboot ]]; then
    echo "rtt.sh serves the nRF54 boards only. The nRF52840 has no debug probe and logs" >&2
    echo "over USB CDC -- read that port directly." >&2
    exit 2
  fi
  if [[ "${OD_TARGET_BOARD}" == *lm20* ]]; then
    TARGET="${TARGET:-nrf54lm20a}"
  else
    TARGET="${TARGET:-nrf54l}"
  fi
  if [[ -z "${BUILD_DIR:-}" ]]; then
    # Same two naming schemes the flash engine resolves: build-nrf54.sh writes the
    # registry name plus -debug, build.sh --all writes build-<board-tag>-debug.
    if [[ "${PROFILE}" == debug ]]; then
      for d in "${SCRIPT_DIR}/${OD_TARGET_BUILD_DIR}-debug" \
               "${SCRIPT_DIR}/build-${OD_TARGET_BOARD%%/*}-debug"; do
        if [[ -f "${d}/zephyr/zephyr/.config" ]]; then
          BUILD_DIR="${d}"
          break
        fi
      done
    else
      BUILD_DIR="${SCRIPT_DIR}/${OD_TARGET_BUILD_DIR}"
    fi
  fi
fi

BUILD_DIR="${BUILD_DIR:-}"
CONFIG="${BUILD_DIR}/zephyr/zephyr/.config"
if [[ -z "${BUILD_DIR}" || ! -f "${CONFIG}" ]]; then
  echo "No ${PROFILE} build for ${VARIANT:-?} (looked for ${BUILD_DIR:-<unset>})." >&2
  echo "Build one with:  PROFILE=${PROFILE} ./build-nrf54.sh ${VARIANT:-l15}" >&2
  exit 1
fi

# The ELF is only useful if it is this board's. Streaming another board's log addresses
# reads plausible garbage rather than failing, so check before connecting.
if [[ -n "${VARIANT}" ]]; then
  WANT_SOC="$(echo "${OD_TARGET_BOARD}" | cut -d/ -f2)"
  if ! grep -q "^CONFIG_SOC=\"${WANT_SOC}\"\$" "${CONFIG}"; then
    echo "Refusing: ${BUILD_DIR} is a $(sed -n 's/^CONFIG_SOC=//p' "${CONFIG}") build," >&2
    echo "expected \"${WANT_SOC}\" for variant '${VARIANT}'." >&2
    exit 1
  fi
fi

TARGET="${TARGET:-nrf54l}"

# Do not source ncs-env.sh here: the NCS toolchain ships an older pyocd that
# often lacks nrf54lm20a. Use the user's own pyocd install.

# pyocd is commonly installed with pipx, where it is importable only from its
# own venv interpreter and never from `python3` on PATH. Fall back to the
# interpreter in the pyocd launcher's shebang (flash.sh searches the same list).
find_python() {
  local candidate interp
  if command -v python3 >/dev/null 2>&1 && python3 -c 'import pyocd' 2>/dev/null; then
    command -v python3
    return 0
  fi
  for candidate in \
    "${HOME}/.local/bin/pyocd" \
    /usr/local/bin/pyocd \
    "$(command -v pyocd 2>/dev/null || true)"; do
    [[ -n "${candidate}" && -x "${candidate}" ]] || continue
    interp="$(head -n 1 "${candidate}" | sed -n 's|^#!\s*||p')"
    [[ -n "${interp}" && -x "${interp}" ]] || continue
    if "${interp}" -c 'import pyocd' 2>/dev/null; then
      echo "${interp}"
      return 0
    fi
  done
  return 1
}

if ! PYTHON="$(find_python)"; then
  echo "pyocd not importable from any python3 found (pipx install pyocd)" >&2
  exit 1
fi

# The linked address of _SEGGER_RTT beats scanning: no blind reads, so no
# faulting off the end of mapped SRAM.
find_rtt_symbol() {
  local elf="${BUILD_DIR}/zephyr/zephyr/zephyr.elf" nm addr
  [[ -f "${elf}" ]] || return 1
  # arm-none-eabi-nm is frequently absent; the Zephyr SDK's copy is not on PATH but is
  # always installed with the toolchain, and a host nm reads ARM ELF symbol tables fine.
  for nm in arm-zephyr-eabi-nm arm-none-eabi-nm nm \
            "$(ls -1 "${HOME}"/ncs/toolchains/*/opt/zephyr-sdk/arm-zephyr-eabi/bin/arm-zephyr-eabi-nm 2>/dev/null | head -1)"; do
    [[ -n "${nm}" ]] || continue
    command -v "${nm}" >/dev/null 2>&1 || continue
    addr="$("${nm}" "${elf}" 2>/dev/null | awk '$3 == "_SEGGER_RTT" { print $1; exit }')"
    if [[ -n "${addr}" ]]; then
      echo "0x${addr}"
      return 0
    fi
  done
  return 1
}

if [[ -z "${OD_RTT_CB:-}" ]]; then
  OD_RTT_CB="$(find_rtt_symbol || true)"
  if [[ -z "${OD_RTT_CB}" ]]; then
    echo "No _SEGGER_RTT in ${BUILD_DIR}/zephyr/zephyr/zephyr.elf; falling back to a scan." >&2
  fi
fi

# Scan window from THIS build's SRAM, not a constant. pyocd's declared RAM region is
# larger than cpuapp SRAM on the LM20A (512 KB claimed, 511 KB real), and a read past the
# end faults the probe rather than returning short -- the FAULT ACK the header describes.
SRAM_BASE="$(sed -n 's/^CONFIG_SRAM_BASE_ADDRESS=//p' "${CONFIG}" | head -1)"
SRAM_KB="$(sed -n 's/^CONFIG_SRAM_SIZE=//p' "${CONFIG}" | head -1)"
if [[ -n "${SRAM_BASE}" && -n "${SRAM_KB}" ]]; then
  OD_RTT_SCAN_START="${OD_RTT_SCAN_START:-${SRAM_BASE}}"
  OD_RTT_SCAN_END="${OD_RTT_SCAN_END:-$(printf '0x%X' $(( SRAM_BASE + SRAM_KB * 1024 )))}"
fi

export OD_RTT_TARGET="${TARGET}"
export OD_RTT_DURATION="${DURATION}"
export OD_RTT_CB="${OD_RTT_CB:-}"
export OD_RTT_SCAN_START="${OD_RTT_SCAN_START:-0x20000000}"
export OD_RTT_SCAN_END="${OD_RTT_SCAN_END:-0x2002f000}"

echo "Board:  ${OD_TARGET_BOARD:-<BUILD_DIR override>} (pyocd ${TARGET})" >&2
echo "Build:  ${BUILD_DIR}" >&2
echo "RTT cb: ${OD_RTT_CB:-<scan ${OD_RTT_SCAN_START}..${OD_RTT_SCAN_END}>}" >&2

# Answering "which build would this stream?" must not require a board: connecting resets
# the target, and on nRF54L a halted CPU also stops the radio.
if [[ -n "${OD_RTT_RESOLVE_ONLY:-}" ]]; then
  exit 0
fi

# Prefer a pyocd that knows the target; try primary then nrf54l fallback in Python.
exec env -u PYTHONHOME -u PYTHONPATH -u PYTHONSTARTUP "${PYTHON}" - <<'PY'
import os
import sys
import time

try:
    from pyocd.core.helpers import ConnectHelper
    from pyocd.core.exceptions import TransferError
except ImportError:
    print("pyocd not installed (pipx install pyocd)", file=sys.stderr)
    sys.exit(1)

MAGIC = b"SEGGER RTT"

target_name = os.environ.get("OD_RTT_TARGET", "nrf54l")
try:
    duration = float(os.environ.get("OD_RTT_DURATION", "0"))
except ValueError:
    duration = 0.0

def env_addr(name, default):
    value = os.environ.get(name, "")
    try:
        return int(value, 0)
    except ValueError:
        return default

def check_rtt_cb(target, addr):
    try:
        return bytes(target.read_memory_block8(addr, len(MAGIC))) == MAGIC
    except TransferError:
        return False

def find_rtt_cb(target, start, end):
    # Chunked scan of early SRAM. A transfer fault means the window runs past
    # mapped RAM (pyocd's RAM region is larger than cpuapp SRAM on LM20A), so
    # stop rather than let the fault escape as a probe failure.
    chunk = 4096
    addr = start
    while addr < end:
        n = min(chunk, end - addr)
        try:
            data = bytes(target.read_memory_block8(addr, n))
        except TransferError as exc:
            print(
                f"scan stopped at {addr:#x}: {exc}\n"
                "Unmapped RAM — set OD_RTT_SCAN_END lower, or OD_RTT_CB directly.",
                file=sys.stderr,
            )
            return None
        idx = data.find(MAGIC)
        if idx >= 0:
            return addr + idx
        # Overlap so a split across chunk boundary is still found
        addr += chunk - len(MAGIC)
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

    cb = env_addr("OD_RTT_CB", 0)
    if cb and not check_rtt_cb(target, cb):
        print(
            f"No RTT control block at {cb:#x} (stale build dir?) — scanning",
            file=sys.stderr,
        )
        cb = 0
    if not cb:
        cb = find_rtt_cb(
            target,
            env_addr("OD_RTT_SCAN_START", 0x20000000),
            env_addr("OD_RTT_SCAN_END", 0x2002F000),
        )
    if not cb:
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
