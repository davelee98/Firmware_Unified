#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VARIANT="${1:-l15}"
if [[ $# -gt 0 ]]; then
  shift
fi
# shellcheck source=scripts/target-registry.sh
source "${SCRIPT_DIR}/scripts/target-registry.sh"
od_target_load "${VARIANT}"
if [[ "${OD_TARGET_BOOTLOADER}" != mcuboot ]]; then
  echo "build-nrf54.sh accepts only l15 or lm20" >&2
  exit 2
fi

export BOARD="${OD_TARGET_BOARD}"
if [[ "${PROFILE:-battery}" == debug ]]; then
  export BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/${OD_TARGET_BUILD_DIR}-debug}"
else
  export BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/${OD_TARGET_BUILD_DIR}}"
fi
exec "${SCRIPT_DIR}/build.sh" "$@"
