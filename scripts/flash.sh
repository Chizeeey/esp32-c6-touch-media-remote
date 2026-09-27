#!/usr/bin/env bash
# Compile and upload the firmware to the attached board.
set -euo pipefail
source "$(dirname "$0")/common.sh"

PORT="${1:-$(find_port || true)}"
if [ -z "$PORT" ]; then
  echo "No ESP32 found on USB. Is the board plugged in?" >&2
  exit 1
fi
if [ ! -r "$PORT" ] || [ ! -w "$PORT" ]; then
  echo "No access to $PORT. Run:  sudo usermod -aG dialout \$USER" >&2
  echo "then log out and back in (or 'newgrp dialout' in this shell)." >&2
  exit 1
fi

echo "==> Building"
arduino-cli compile -b "$FQBN" "$SKETCH"
echo "==> Flashing to $PORT"
arduino-cli upload -b "$FQBN" -p "$PORT" "$SKETCH"
echo "==> Done"
