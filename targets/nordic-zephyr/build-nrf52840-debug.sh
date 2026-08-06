#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PROFILE=debug
export BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/build-nrf52840-debug}"
exec "${SCRIPT_DIR}/build-nrf52840.sh" "$@"
