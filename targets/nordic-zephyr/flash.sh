#!/usr/bin/env bash
# Flash OpenDisplay nRF54 firmware.
#
# Modes:
#   factory (default) — chip erase + merged.hex (MCUboot + primary app).
#                       Wipes settings_storage (display config, bonds).
#   update            — sector erase + zephyr.signed.hex (primary slot only).
#                       Preserves MCUboot, secondary slot, and settings_storage.
#
# Usage:
#   ./flash.sh              # factory (defaults MATCH build.sh: xiao_nrf54l15 / build)
#   ./flash.sh factory
#   ./flash.sh update
#   MODE=update ./flash.sh
#   BUILD_DIR=build BOARD=xiao_nrf54l15/nrf54l15/cpuapp ./flash.sh update
set -euo pipefail

# SCRIPT_DIR FIRST: BUILD_DIR interpolates it. It used to be assigned two lines LATER, so the
# default expanded to a bare "/build" -- harmless only because the default BUILD_DIR was a
# relative "build-lm20" that never referenced it.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# DEFAULTS MATCH build.sh, deliberately. They used to differ -- flash.sh defaulted to
# xiao_nrf54lm20a / build-lm20 while build.sh defaulted to xiao_nrf54l15 / build -- so a plain
# `./build.sh && ./flash.sh` built one board and tried to flash a DIFFERENT one from a directory
# that did not exist. Inherited from the source repo, and exactly the kind of thing that eats
# the first hour of a bench session. Override both together for the LM20:
#   BOARD=xiao_nrf54lm20a/nrf54lm20a/cpuapp BUILD_DIR=build-lm20 ./flash.sh
BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/build}"
BOARD="${BOARD:-xiao_nrf54l15/nrf54l15/cpuapp}"

if [[ "${BOARD}" == xiao_ble* ]]; then
  echo "Refusing nRF52840 in the MCUboot/nRF54 flasher." >&2
  echo "Use ./flash-nrf52840.sh with the Adafruit UF2 bootloader instead." >&2
  exit 2
fi

MODE="${MODE:-}"
if [[ $# -ge 1 ]]; then
  MODE="$1"
  shift
fi
MODE="${MODE:-factory}"
MODE="$(echo "${MODE}" | tr '[:upper:]' '[:lower:]')"

case "${MODE}" in
  factory|full|chip) MODE=factory ;;
  update|slot|app|ota) MODE=update ;;
  *)
    echo "Unknown mode '${MODE}'. Use: factory | update" >&2
    exit 1
    ;;
esac

pyocd_target() {
  if [[ "${BOARD}" == *lm20* || "${BUILD_DIR}" == *lm20* ]]; then
    echo nrf54lm20a
  else
    echo nrf54l
  fi
}

find_pyocd() {
  local candidate
  for candidate in \
    "${HOME}/.local/bin/pyocd" \
    /usr/local/bin/pyocd \
    "$(command -v pyocd 2>/dev/null || true)"; do
    if [[ -n "${candidate}" && -x "${candidate}" ]]; then
      echo "${candidate}"
      return 0
    fi
  done
  return 1
}

resolve_image() {
  if [[ "${MODE}" == "factory" ]]; then
    HEX="${BUILD_DIR}/merged.hex"
    if [[ ! -f "${HEX}" ]]; then
      echo "Missing ${HEX} (factory flash needs merged MCUboot+app image)" >&2
      exit 1
    fi
    ERASE=chip
  else
    HEX="${BUILD_DIR}/zephyr/zephyr/zephyr.signed.hex"
    if [[ ! -f "${HEX}" ]]; then
      echo "Missing ${HEX} (update flash needs signed primary-slot image)" >&2
      echo "Build with MCUboot enabled, then retry." >&2
      exit 1
    fi
    # Sector erase only touches sectors covered by the hex (primary slot).
    # settings_storage sits after both slots and is left alone.
    ERASE=sector
  fi
}

resolve_image

# west flash via Seeed CMSIS-DAP can leave the last bytes unprogrammed; that
# shows up as a BUS FAULT in net_buf during bt_enable (no advertising).
if PYOCD="$(find_pyocd)"; then
  TARGET="$(pyocd_target)"
  echo "Mode:   ${MODE}"
  echo "Image:  ${HEX}"
  echo "Erase:  ${ERASE}"
  if [[ "${MODE}" == "update" ]]; then
    echo "Note:   primary slot only — display config in settings_storage is kept"
  else
    echo "Note:   chip erase — display config / NVS settings will be wiped"
  fi
  env -u PYTHONHOME -u PYTHONPATH -u PYTHONSTARTUP \
    "${PYOCD}" flash -t "${TARGET}" --erase "${ERASE}" "${HEX}"
  env -u PYTHONHOME -u PYTHONPATH -u PYTHONSTARTUP \
    "${PYOCD}" reset -t "${TARGET}" 2>/dev/null || true
  exit 0
fi

# Fallback: west flash (needs NCS toolchain on PATH).
# shellcheck source=ncs-env.sh
source "${SCRIPT_DIR}/ncs-env.sh"
if [[ "${MODE}" == "update" ]]; then
  echo "pyocd not found; west flash always programs the sysbuild runners (factory-like)." >&2
  echo "Install pyocd for MODE=update (primary-slot-only)." >&2
  exit 1
fi
west flash -d "${BUILD_DIR}" "$@"
if command -v nrfjprog >/dev/null 2>&1; then
  nrfjprog --reset 2>/dev/null || true
fi
