#!/usr/bin/env bash
# Attach GDB to an nRF54 debug build (l15 | lm20) with Zephyr thread awareness.
#
#   ./debug-nrf54.sh lm20            # interactive gdb
#   ./debug-nrf54.sh lm20 -ex 'info threads' -ex 'thread apply all bt' -ex detach
#
# Once attached, `info threads` lists every Zephyr thread by name and
# `thread apply all bt` backtraces all of them.
#
# HALTING STOPS THE RADIO. The SoftDevice Controller and MPSL share the core with the
# application on nRF54L, so a halted CPU is a silent one: advertising stops and the device
# vanishes from scanners until it resumes. Memory reads and RTT do not halt; `halt`, `step`
# and breakpoints do.
#
# GDB's `detach` does NOT reliably resume this target -- verify with `monitor targets` (or
# ./flash.sh's openocd) and issue `monitor resume` if it comes back halted. Repeated
# backtraces from a target that never resumed look exactly like a spinning thread.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/target-registry.sh
source "${SCRIPT_DIR}/scripts/target-registry.sh"

VARIANT="${1:-l15}"
if [[ $# -gt 0 ]]; then
  shift
fi
od_target_load "${VARIANT}"
if [[ "${OD_TARGET_BOOTLOADER}" != mcuboot ]]; then
  echo "debug-nrf54.sh accepts only l15 or lm20" >&2
  exit 2
fi

if [[ -z "${BUILD_DIR:-}" ]]; then
  REGISTRY_DIR="${SCRIPT_DIR}/${OD_TARGET_BUILD_DIR}-debug"
  ALL_DIR="${SCRIPT_DIR}/build-${OD_TARGET_BOARD%%/*}-debug"
  if [[ -f "${REGISTRY_DIR}/zephyr/zephyr/.config" ]]; then
    BUILD_DIR="${REGISTRY_DIR}"
  elif [[ -f "${ALL_DIR}/zephyr/zephyr/.config" ]]; then
    BUILD_DIR="${ALL_DIR}"
  else
    echo "No debug build for ${OD_TARGET_NAME}. Build one with:" >&2
    echo "  ./build-nrf54-debug.sh ${VARIANT}" >&2
    exit 1
  fi
fi

ELF="${BUILD_DIR}/zephyr/zephyr/zephyr.elf"
CONFIG="${BUILD_DIR}/zephyr/zephyr/.config"
RUNNERS="${BUILD_DIR}/zephyr/zephyr/runners.yaml"
for f in "${ELF}" "${CONFIG}" "${RUNNERS}"; do
  if [[ ! -f "${f}" ]]; then
    echo "Missing ${f}" >&2
    exit 1
  fi
done

# Thread awareness is a build-time export, not a debugger setting: without these tables
# OpenOCD can only ever report the thread the CPU happened to be in.
if ! grep -q '^CONFIG_DEBUG_THREAD_INFO=y$' "${CONFIG}"; then
  echo "WARNING: ${BUILD_DIR} was built without CONFIG_DEBUG_THREAD_INFO=y." >&2
  echo "         'info threads' will show a single 'Remote target'. Rebuild with" >&2
  echo "         ./build-nrf54-debug.sh ${VARIANT} to get per-thread backtraces." >&2
fi

runners_config() { sed -n "s|^  $1: ||p" "${RUNNERS}" | head -1; }
BOARD_DIR="$(runners_config board_dir)"
OOCD_TARGET="$(sed -n 's|^    - targets ||p' "${RUNNERS}" | head -1)"
CFG="${BOARD_DIR}/support/openocd.cfg"
if [[ -z "${BOARD_DIR}" || -z "${OOCD_TARGET}" || ! -f "${CFG}" ]]; then
  echo "Could not resolve the OpenOCD board config from ${RUNNERS}" >&2
  exit 1
fi

OPENOCD="${OPENOCD:-$(runners_config openocd)}"
if [[ -z "${OPENOCD}" ]]; then
  OPENOCD="$(command -v openocd || true)"
fi
GDB="${GDB:-$(ls ~/ncs/toolchains/*/opt/zephyr-sdk/arm-zephyr-eabi/bin/arm-zephyr-eabi-gdb 2>/dev/null | head -1)}"
for t in "${OPENOCD}" "${GDB}"; do
  if [[ -z "${t}" || ! -x "${t}" ]]; then
    echo "Missing tool (set OPENOCD= / GDB=): openocd='${OPENOCD}' gdb='${GDB}'" >&2
    exit 1
  fi
done

OOCD_LOG="$(mktemp -t od-openocd-XXXXXX.log)"
# gdb_memory_map disable is REQUIRED, not tidiness: the board cfg ends with a
# `flash bank ... nrf5` line, and GDB asking for a memory map makes OpenOCD auto_probe that
# driver, which cannot read an nRF54L ("Couldn't read CONFIGID register") and rejects the
# connection outright. Nothing here programs flash, so the map buys nothing.
"${OPENOCD}" -f "${CFG}" \
  -c "${OOCD_TARGET} configure -rtos Zephyr" \
  -c 'gdb_memory_map disable' \
  -c 'gdb_flash_program disable' \
  -c init >"${OOCD_LOG}" 2>&1 &
OOCD_PID=$!
cleanup() {
  kill "${OOCD_PID}" 2>/dev/null || true
  wait "${OOCD_PID}" 2>/dev/null || true
  rm -f "${OOCD_LOG}"
}
trap cleanup EXIT

for _ in $(seq 1 40); do
  if grep -q "Listening on port 3333" "${OOCD_LOG}" 2>/dev/null; then
    break
  fi
  if ! kill -0 "${OOCD_PID}" 2>/dev/null; then
    echo "openocd exited during startup:" >&2
    cat "${OOCD_LOG}" >&2
    exit 1
  fi
  sleep 0.25
done

echo "Board:  ${OD_TARGET_BOARD} (${OOCD_TARGET}, -rtos Zephyr)"
echo "ELF:    ${ELF}"
echo

"${GDB}" -q \
  -ex 'set confirm off' -ex 'set pagination off' \
  -ex 'target extended-remote :3333' \
  "$@" "${ELF}"
