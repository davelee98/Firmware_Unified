#!/usr/bin/env bash
# Build OpenDisplay nRF54 firmware with NCS/west.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="${APP_DIR:-${SCRIPT_DIR}/zephyr}"
BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/build}"
BOARD="${BOARD:-xiao_nrf54l15/nrf54l15/cpuapp}"
PROFILE="${PROFILE:-battery}"   # battery | uart | quiet
PURGE="${PURGE:-always}"        # always | never | auto

export PROFILE

# shellcheck source=ncs-env.sh
source "${SCRIPT_DIR}/ncs-env.sh"

CMAKE_ARGS=(-DBOARD_ROOT="${SCRIPT_DIR}")
if [[ "${PROFILE}" == "uart" ]]; then
  CMAKE_ARGS+=(-DEXTRA_CONF_FILE="${APP_DIR}/prj_uart.conf")
fi
# PROFILE=quiet is picked up from the environment in CMakeLists.txt
# (prj_quiet.conf + OD_LOW_POWER_QUIET).
FW_VER="${OD_FW_VERSION:-${BUILD_VERSION:-}}"
if [[ -n "${FW_VER}" ]]; then
  # Export for app CMakeLists (sysbuild-safe). Also pass cmake arg for completeness.
  export OD_FW_VERSION="${FW_VER}"
  export BUILD_VERSION="${FW_VER}"
  CMAKE_ARGS+=(-Dzephyr_OD_FW_VERSION="${FW_VER}")
fi
if [[ -n "${SHA:-}" ]]; then
  export SHA
  CMAKE_ARGS+=(-Dzephyr_GIT_SHA="${SHA}")
fi
if [[ -n "${FACTORY_CONFIG_HEX:-}" ]]; then
  CMAKE_ARGS+=(-DFACTORY_CONFIG_HEX="${FACTORY_CONFIG_HEX}")
fi
if [[ "${OPENDISPLAY_FACTORY_CLEAR_CONFIG:-}" =~ ^(1|true|yes|on)$ ]]; then
  CMAKE_ARGS+=(-DFACTORY_CLEAR_CONFIG_ON_BOOT=ON)
fi

west build -p "${PURGE}" -d "${BUILD_DIR}" -b "${BOARD}" "${APP_DIR}" -- "${CMAKE_ARGS[@]}"

HEX="${BUILD_DIR}/merged.hex"
if [[ ! -f "${HEX}" ]]; then
  HEX="${BUILD_DIR}/zephyr/zephyr/zephyr.hex"
fi
if [[ ! -f "${HEX}" ]]; then
  HEX="${BUILD_DIR}/opendisplay_nrf54/zephyr/zephyr.hex"
fi

UPDATE_BIN=""
for candidate in \
  "${BUILD_DIR}/zephyr/zephyr/zephyr.signed.bin" \
  "${BUILD_DIR}/zephyr/app_update.bin" \
  "${BUILD_DIR}/app_update.bin"; do
  if [[ -f "${candidate}" ]]; then
    UPDATE_BIN="${candidate}"
    break
  fi
done
DFU_ZIP=""
for candidate in \
  "${BUILD_DIR}/dfu_application.zip" \
  "${BUILD_DIR}/zephyr/dfu_application.zip"; do
  if [[ -f "${candidate}" ]]; then
    DFU_ZIP="${candidate}"
    break
  fi
done

CONF="${BUILD_DIR}/zephyr/zephyr/.config"
echo
echo "Built: ${HEX}"
if [[ -n "${UPDATE_BIN}" ]]; then
  echo "OTA:   ${UPDATE_BIN}"
fi
if [[ -n "${DFU_ZIP}" ]]; then
  echo "DFU:   ${DFU_ZIP}"
fi
echo "Flash: ./flash.sh"
if [[ -f "${CONF}" ]]; then
  echo "Profile: ${PROFILE}  serial=$(grep -E '^CONFIG_SERIAL=|^# CONFIG_SERIAL is not set' "${CONF}" | head -1)"
  if [[ "${PROFILE}" == "uart" ]]; then
    echo "Serial monitor: 115200 baud on the board USB port"
  elif [[ "${PROFILE}" == "quiet" ]]; then
    echo "Quiet build: LOG off, no heartbeat prints (for advertising-current tests)"
  else
    echo "Logs: SEGGER RTT (battery build has no USB UART)"
  fi
fi
