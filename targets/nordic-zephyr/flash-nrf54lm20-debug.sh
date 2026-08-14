#!/usr/bin/env bash
# Flash the PROFILE=debug XIAO nRF54LM20A build with OpenOCD.
# Pairs with ./build-nrf54-debug.sh lm20.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export OD_FLASH_PROFILE=debug
exec "${SCRIPT_DIR}/scripts/flash-nrf54-openocd.sh" lm20 "$@"
