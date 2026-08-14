#!/usr/bin/env bash
# Flash the XIAO nRF54LM20A with OpenOCD over the board's CMSIS-DAP debug USB.
#
#   ./flash-nrf54lm20.sh          # factory: CTRL-AP mass erase + merged.hex
#   ./flash-nrf54lm20.sh update   # primary slot only, keeps settings_storage
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "${SCRIPT_DIR}/scripts/flash-nrf54-openocd.sh" lm20 "$@"
