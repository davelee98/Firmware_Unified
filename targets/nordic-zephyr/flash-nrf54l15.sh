#!/usr/bin/env bash
# Flash the XIAO nRF54L15 with OpenOCD over the board's CMSIS-DAP debug USB.
#
#   ./flash-nrf54l15.sh           # factory (see below)
#   ./flash-nrf54l15.sh update    # primary slot only, keeps settings_storage
#
# This board's support/openocd.cfg defines no mass-erase procedure, so 'factory' is refused
# rather than silently degraded to an image-only write. Use ./flash.sh factory (pyocd) when a
# real chip erase is needed.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "${SCRIPT_DIR}/scripts/flash-nrf54-openocd.sh" l15 "$@"
