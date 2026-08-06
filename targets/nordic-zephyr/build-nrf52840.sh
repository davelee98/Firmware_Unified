#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/target-registry.sh
source "${SCRIPT_DIR}/scripts/target-registry.sh"
od_target_load nrf52840

export BOARD="${OD_TARGET_BOARD}"
if [[ "${PROFILE:-battery}" == debug ]]; then
  export BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/${OD_TARGET_BUILD_DIR}-debug}"
else
  export BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/${OD_TARGET_BUILD_DIR}}"
fi
exec "${SCRIPT_DIR}/build.sh" "$@"
