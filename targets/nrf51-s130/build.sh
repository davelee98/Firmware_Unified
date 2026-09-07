#!/usr/bin/env bash
set -euo pipefail

TARGET_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${TARGET_DIR}/build}"
: "${NRF5_SDK_ROOT:?Set NRF5_SDK_ROOT to Nordic nRF5 SDK 12.3.0}"

cmake -S "${TARGET_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_TOOLCHAIN_FILE="${TARGET_DIR}/cmake/arm-none-eabi.cmake" \
  -DNRF5_SDK_ROOT="${NRF5_SDK_ROOT}"
cmake --build "${BUILD_DIR}" --parallel

CHECK_ARGS=(
  --app "${BUILD_DIR}/opendisplay-nrf51822.hex"
  --elf "${BUILD_DIR}/opendisplay-nrf51822.elf"
  --size-tool "$(command -v arm-none-eabi-size)"
  --nm-tool "$(command -v arm-none-eabi-nm)"
  --manifest "${BUILD_DIR}/manifest.json"
)
if [[ -n "${S130_HEX:-}" ]]; then
  CHECK_ARGS+=(--softdevice "${S130_HEX}" --merged "${BUILD_DIR}/opendisplay-nrf51822-full.hex")
else
  rm -f "${BUILD_DIR}/opendisplay-nrf51822-full.hex"
fi
python3 "${TARGET_DIR}/tools/check_image.py" "${CHECK_ARGS[@]}"

REPO_DIR="$(cd "${TARGET_DIR}/../.." && pwd)"
RELEASE_DIR="${REPO_DIR}/release"
mkdir -p "${RELEASE_DIR}"
cp "${BUILD_DIR}/opendisplay-nrf51822.elf" "${RELEASE_DIR}/"
cp "${BUILD_DIR}/opendisplay-nrf51822.hex" "${RELEASE_DIR}/"
cp "${BUILD_DIR}/opendisplay-nrf51822.bin" "${RELEASE_DIR}/"
cp "${BUILD_DIR}/opendisplay-nrf51822.map" "${RELEASE_DIR}/"
cp "${BUILD_DIR}/manifest.json" "${RELEASE_DIR}/MANIFEST-nrf51-s130.json"
if [[ -f "${BUILD_DIR}/opendisplay-nrf51822-full.hex" ]]; then
  cp "${BUILD_DIR}/opendisplay-nrf51822-full.hex" "${RELEASE_DIR}/"
else
  rm -f "${RELEASE_DIR}/opendisplay-nrf51822-full.hex"
fi
{
  echo "OpenDisplay nRF51822/S130 experimental build"
  echo "application  opendisplay-nrf51822.hex"
  echo "flash        0x1B000..0x1F7FF (18,432 bytes maximum)"
  echo "ram          0x20001F00..0x20003FFF (8,448 bytes)"
  echo "softdevice   S130 2.0.1, supplied separately"
  echo "verification hardware gates remain open"
} > "${RELEASE_DIR}/MANIFEST-nrf51-s130.txt"
