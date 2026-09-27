#!/usr/bin/env bash
# Watch the raw serial output from the board (Ctrl-C to quit).
#
# This goes through the daemon's --monitor mode rather than stty/cat on
# purpose: the board's USB Serial/JTAG stays silent unless DTR is asserted,
# and the daemon is the one place that knows how to open the port correctly.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
exec python3 "$HERE/../host/media_remote_linux.py" --monitor "$@"
