#!/usr/bin/env bash
# Compile the firmware.
set -euo pipefail
source "$(dirname "$0")/common.sh"
exec arduino-cli compile -b "$FQBN" "$SKETCH" "$@"
