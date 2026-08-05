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
#   ./flash.sh              # factory (LM20 / build-lm20)
#   ./flash.sh factory
#   ./flash.sh update
#   MODE=update ./flash.sh
#   BUILD_DIR=build BOARD=xiao_nrf54l15/nrf54l15/cpuapp ./flash.sh update
set -euo pipefail

# Default to XIAO nRF54LM20A (override with BUILD_DIR=build BOARD=... for L15).
BUILD_DIR="${BUILD_DIR:-build-lm20}"
BOARD="${BOARD:-xiao_nrf54lm20a/nrf54lm20a/cpuapp}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

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
