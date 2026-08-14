#!/usr/bin/env bash
# OpenOCD flashing engine for the nRF54 boards. NOT a front door: call
# ../flash-nrf54l15.sh, ../flash-nrf54lm20.sh or their -debug counterparts, which name the
# board so a wrong-image flash is a typo you can see rather than a positional argument.
#
#   flash-nrf54-openocd.sh <l15|lm20> [factory|update]
#
# Env:
#   OD_FLASH_PROFILE=debug   resolve the -debug build directory and require a debug build
#   BUILD_DIR                use this directory instead of the resolved one
#   OPENOCD                  use this openocd binary
#
# Modes:
#   factory (default) — CTRL-AP mass erase + merged.hex (MCUboot + primary app).
#                       Wipes settings_storage (display config, bonds).
#   update            — signed primary-slot image written in place. Preserves MCUboot,
#                       the secondary slot, and settings_storage.
#
# nRF54L stores code in RRAM, and no OpenOCD flash driver programs it -- the board's
# support/openocd.cfg instead puts RRAMC into write mode and uses load_image, so writes land
# without an erase cycle. That is why `update` needs no sector erase, and why the .cfg's
# trailing `flash bank ... nrf5` line is never exercised here (it would not probe on this part).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
# shellcheck source=target-registry.sh
source "${SCRIPT_DIR}/target-registry.sh"

VARIANT="${1:-}"
if [[ $# -gt 0 ]]; then
  shift
fi
od_target_load "${VARIANT}"
if [[ "${OD_TARGET_BOOTLOADER}" != mcuboot ]]; then
  echo "flash-nrf54-openocd.sh accepts only l15 or lm20" >&2
  exit 2
fi

PROFILE="${OD_FLASH_PROFILE:-battery}"

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

if [[ -z "${BUILD_DIR:-}" ]]; then
  if [[ "${PROFILE}" == debug ]]; then
    # Two directory naming schemes reach the same board: build-nrf54.sh writes the registry
    # name plus -debug, build.sh --all writes build-<board-tag>-debug. Both occur in this
    # tree, so try each rather than failing on a build spelled the other way.
    REGISTRY_DIR="${TARGET_DIR}/${OD_TARGET_BUILD_DIR}-debug"
    ALL_DIR="${TARGET_DIR}/build-${OD_TARGET_BOARD%%/*}-debug"
    if [[ -f "${REGISTRY_DIR}/zephyr/zephyr/.config" ]]; then
      BUILD_DIR="${REGISTRY_DIR}"
    elif [[ -f "${ALL_DIR}/zephyr/zephyr/.config" ]]; then
      BUILD_DIR="${ALL_DIR}"
    else
      echo "No debug build for ${OD_TARGET_NAME}. Looked in:" >&2
      echo "  ${REGISTRY_DIR}" >&2
      echo "  ${ALL_DIR}" >&2
      echo "Build one with:  ./build-nrf54-debug.sh ${VARIANT}" >&2
      exit 1
    fi
  else
    BUILD_DIR="${TARGET_DIR}/${OD_TARGET_BUILD_DIR}"
  fi
fi

CONFIG="${BUILD_DIR}/zephyr/zephyr/.config"
if [[ ! -f "${CONFIG}" ]]; then
  echo "Refusing to flash: no application .config under ${BUILD_DIR}." >&2
  echo "Build first:  ./build-nrf54.sh ${VARIANT}" >&2
  exit 1
fi

# Checked for every profile: flashing an L15 image to an LM20A is a soft brick that reads as a
# bad cable, and both boards present the same CMSIS-DAP probe. Board field 2 of the registry
# entry is the SoC Kconfig string.
WANT_SOC="$(echo "${OD_TARGET_BOARD}" | cut -d/ -f2)"
if ! grep -q "^CONFIG_SOC=\"${WANT_SOC}\"\$" "${CONFIG}"; then
  echo "Refusing to flash: ${BUILD_DIR} is a $(sed -n 's/^CONFIG_SOC=//p' "${CONFIG}") build," >&2
  echo "expected \"${WANT_SOC}\"." >&2
  exit 1
fi

# The directory name is not evidence of the profile. A -debug directory rebuilt without
# PROFILE=debug in the environment holds a battery image under a debug name; CMakeLists.txt
# reads PROFILE from the environment, so nothing in the path records what was applied.
# prj_debug.conf is what raises the Zephyr subsystem level to 3.
if [[ "${PROFILE}" == debug ]] && ! grep -q '^CONFIG_LOG_DEFAULT_LEVEL=3$' "${CONFIG}"; then
  echo "Refusing to flash: ${BUILD_DIR} is not a PROFILE=debug build." >&2
  echo "  CONFIG_LOG_DEFAULT_LEVEL=$(sed -n 's/^CONFIG_LOG_DEFAULT_LEVEL=//p' "${CONFIG}"), expected 3" >&2
  echo "Rebuild with:  ./build-nrf54-debug.sh ${VARIANT}" >&2
  exit 1
fi

# runners.yaml is the build's own record of board_dir, the load proc and the OpenOCD target
# name. Taking them from there rather than hardcoding keeps l15 (board under NCS) and lm20
# (board in this repo) on one code path and cannot drift from what was built.
RUNNERS="${BUILD_DIR}/zephyr/zephyr/runners.yaml"
if [[ ! -f "${RUNNERS}" ]]; then
  echo "Missing ${RUNNERS}" >&2
  exit 1
fi

runners_config() { sed -n "s|^  $1: ||p" "${RUNNERS}" | head -1; }

BOARD_DIR="$(runners_config board_dir)"
OOCD_LOAD="$(sed -n 's|^    - --cmd-load=||p' "${RUNNERS}" | head -1)"
OOCD_TARGET="$(sed -n 's|^    - targets ||p' "${RUNNERS}" | head -1)"

if [[ -z "${BOARD_DIR}" || -z "${OOCD_LOAD}" || -z "${OOCD_TARGET}" ]]; then
  echo "Could not read the OpenOCD runner config from ${RUNNERS}" >&2
  echo "  board_dir='${BOARD_DIR}' load='${OOCD_LOAD}' target='${OOCD_TARGET}'" >&2
  exit 1
fi

CFG="${BOARD_DIR}/support/openocd.cfg"
if [[ ! -f "${CFG}" ]]; then
  echo "Missing OpenOCD board config: ${CFG}" >&2
  exit 1
fi

# Zephyr leaves the recorded path empty when it did not resolve openocd at configure time.
OPENOCD="${OPENOCD:-$(runners_config openocd)}"
if [[ -z "${OPENOCD}" ]]; then
  OPENOCD="$(command -v openocd || true)"
fi
if [[ -z "${OPENOCD}" || ! -x "${OPENOCD}" ]]; then
  echo "openocd not found (set OPENOCD=/path/to/openocd)" >&2
  exit 1
fi

if [[ "${MODE}" == "factory" ]]; then
  HEX="${BUILD_DIR}/merged.hex"
  if [[ ! -f "${HEX}" ]]; then
    echo "Missing ${HEX} (factory flash needs the merged MCUboot+app image)" >&2
    exit 1
  fi
else
  HEX="${BUILD_DIR}/zephyr/zephyr/zephyr.signed.hex"
  if [[ ! -f "${HEX}" ]]; then
    echo "Missing ${HEX} (update flash needs the signed primary-slot image)" >&2
    exit 1
  fi
fi

# Probed rather than keyed off the variant: only some board configs define the CTRL-AP
# ERASEALL proc, and a config that gains one later should start working without an edit here.
ERASE_PROC=""
if grep -qE '^[[:space:]]*proc[[:space:]]+nrf54l_mass_erase[[:space:]]' "${CFG}"; then
  ERASE_PROC=nrf54l_mass_erase
fi
if [[ "${MODE}" == "factory" && -z "${ERASE_PROC}" ]]; then
  echo "Refusing to flash: ${CFG} defines no mass-erase procedure, so 'factory' cannot" >&2
  echo "wipe settings_storage -- it would only overwrite the image and silently keep the" >&2
  echo "stored display config and BLE bonds." >&2
  echo "Use the same script with 'update' for an image-only write," >&2
  echo "or './flash.sh factory' to chip-erase via pyocd instead." >&2
  exit 1
fi

OOCD_ARGS=(-f "${CFG}" -c init -c "targets ${OOCD_TARGET}" -c halt)
if [[ -n "${ERASE_PROC}" && "${MODE}" == "factory" ]]; then
  OOCD_ARGS+=(-c "${ERASE_PROC}" -c halt)
fi
OOCD_ARGS+=(-c "${OOCD_LOAD} ${HEX}" -c "verify_image ${HEX}" -c "reset run" -c shutdown)

echo "Board:  ${OD_TARGET_BOARD} (${OOCD_TARGET})"
echo "Profile: ${PROFILE}"
echo "Mode:   ${MODE}"
echo "Image:  ${HEX}"
echo "Config: ${CFG}"
if [[ "${MODE}" == "factory" ]]; then
  echo "Note:   mass erase — display config / NVS settings will be wiped"
else
  echo "Note:   primary slot only — display config in settings_storage is kept"
fi

exec "${OPENOCD}" "${OOCD_ARGS[@]}"
