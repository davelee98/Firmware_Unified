#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
# shellcheck source=target-registry.sh
source "${SCRIPT_DIR}/target-registry.sh"
od_target_load "${1:-}"

BUILD_PROFILE="${2:-production}"
case "${BUILD_PROFILE}" in
  production) BUILD_SUFFIX="" ;;
  debug) BUILD_SUFFIX="-debug" ;;
  *)
    echo "Unknown build profile '${BUILD_PROFILE}'. Use production or debug." >&2
    exit 2
    ;;
esac

BUILD_DIR="${BUILD_DIR:-${TARGET_ROOT}/${OD_TARGET_BUILD_DIR}${BUILD_SUFFIX}}"
CONFIG="${BUILD_DIR}/zephyr/zephyr/.config"
COMMANDS="${BUILD_DIR}/zephyr/compile_commands.json"
DTS="${BUILD_DIR}/zephyr/zephyr/zephyr.dts"

for required in "${CONFIG}" "${COMMANDS}" "${DTS}"; do
  if [[ ! -f "${required}" ]]; then
    echo "Missing build output: ${required}" >&2
    exit 1
  fi
done

if [[ "${BUILD_PROFILE}" == debug ]]; then
  grep -q -- '-DOD_DEBUG_BUILD=1' "${COMMANDS}"
  grep -q -- '-DOD_LOG_LEVEL=OD_LOG_DEBUG' "${COMMANDS}"
  grep -q '^CONFIG_LOG_DEFAULT_LEVEL=3$' "${CONFIG}"
else
  if grep -q -- '-DOD_DEBUG_BUILD=1' "${COMMANDS}"; then
    echo "Production build contains the debug-build definition" >&2
    exit 1
  fi
  grep -q -- '-DOD_LOG_LEVEL=OD_LOG_INFO' "${COMMANDS}"
  grep -q '^CONFIG_LOG_DEFAULT_LEVEL=2$' "${CONFIG}"
fi

if [[ "${OD_TARGET_NAME}" == nrf52840 ]]; then
  grep -q '^CONFIG_SOC_NRF52840=y$' "${CONFIG}"
  grep -q '^CONFIG_OD_PLATFORM_NRF52840=y$' "${CONFIG}"
  grep -q '^CONFIG_LOG_BACKEND_UART=y$' "${CONFIG}"
  if grep -q '^CONFIG_LOG_BACKEND_RTT=y$' "${CONFIG}"; then
    echo "nRF52840 build has both UART and RTT logging backends enabled" >&2
    exit 1
  fi
  if grep -q -- '-DTARGET_NRF54\|-DOD_PLATFORM_NRF54' "${COMMANDS}"; then
    echo "nRF52840 build contains an nRF54 platform definition" >&2
    exit 1
  fi
  grep -Fq 'OPENDISPLAY_BUILD_ID=\\\"nrf52840\\\"' "${COMMANDS}"
  if ! sed -n '/uart0:.*uart@40002000/,/};/p' "${DTS}" | grep -q 'status = "disabled"'; then
    echo "nRF52840 uart0 is not disabled; P1.12 would overlap display CS" >&2
    exit 1
  fi
  if ! sed -n '/i2c0:.*i2c@40003000/,/};/p' "${DTS}" | grep -q 'status = "disabled"'; then
    echo "nRF52840 i2c0 is not disabled; runtime GPIO I2C requires unclaimed pins" >&2
    exit 1
  fi
  if ! sed -n '/i2c1:.*i2c@40004000/,/};/p' "${DTS}" | grep -q 'status = "disabled"'; then
    echo "nRF52840 i2c1 is not disabled; P0.05 overlaps runtime board functions" >&2
    exit 1
  fi
  test -f "${BUILD_DIR}/zephyr/zephyr/zephyr.uf2"
else
  grep -q '^CONFIG_SOC_SERIES_NRF54L=y$' "${CONFIG}"
  grep -q '^CONFIG_OD_PLATFORM_NRF54=y$' "${CONFIG}"
  if grep -q -- '-DOD_PLATFORM_NRF52840' "${COMMANDS}"; then
    echo "nRF54 build contains the nRF52840 platform definition" >&2
    exit 1
  fi
  grep -Fq 'OPENDISPLAY_BUILD_ID=\\\"nrf54\\\"' "${COMMANDS}"
  test -f "${BUILD_DIR}/dfu_application.zip"
fi

echo "Validated ${OD_TARGET_NAME} (${BUILD_PROFILE}): ${BUILD_DIR}"
