#!/usr/bin/env bash
# Build the PROFILE=debug nRF54 image (l15 | lm20).
#
# Pairs with ./flash-nrf54-debug.sh. build-nrf54.sh already appends -debug to the registry
# build directory when PROFILE is debug, so this only has to set the profile; CMakeLists.txt
# reads PROFILE from the environment to pick up prj_debug.conf.
#
# Usage:
#   ./build-nrf54-debug.sh lm20
#   ./build-nrf54-debug.sh l15
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PROFILE=debug
exec "${SCRIPT_DIR}/build-nrf54.sh" "$@"
