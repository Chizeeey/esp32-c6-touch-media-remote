#!/usr/bin/env bash
# Preflight and install the media-remote daemon on a machine that has never
# run it. The firmware already lives on the board, so nothing here touches the
# ESP32 -- this is only about the PC side.
#
# Run it with no arguments; it checks first and only installs once everything
# it needs is in place.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
DAEMON="$HERE/media_remote_linux.py"

ok=0
fail=0
note() { printf '  %s\n' "$*"; }
pass() { printf '  \033[32mOK\033[0m    %s\n' "$*"; ok=$((ok + 1)); }
bad()  { printf '  \033[31mMISSING\033[0m %s\n' "$*"; fail=$((fail + 1)); }

# Package names differ per distro; name the right one rather than guessing.
pkg_hint() {
  if   command -v apt    >/dev/null 2>&1; then echo "sudo apt install $1"
  elif command -v dnf    >/dev/null 2>&1; then echo "sudo dnf install $2"
  elif command -v pacman >/dev/null 2>&1; then echo "sudo pacman -S $3"
  else echo "install the $1 package with your package manager"
  fi
}

echo
echo "== Checking prerequisites =="

if command -v python3 >/dev/null 2>&1; then
  pass "python3 ($(python3 -V 2>&1 | cut -d' ' -f2))"
else
  bad "python3"
  note "$(pkg_hint python3 python3 python)"
fi

if python3 -c 'import gi; gi.require_version("Gio","2.0")' 2>/dev/null; then
  pass "python3-gi (D-Bus/MPRIS)"
else
  bad "python3-gi -- without it the daemon cannot find the media players"
  note "$(pkg_hint python3-gi python3-gobject python-gobject)"
fi

if command -v pactl >/dev/null 2>&1; then
  pass "pactl (volume and mute)"
else
  bad "pactl -- the volume controls do nothing, everything else still works"
  note "$(pkg_hint pulseaudio-utils pulseaudio-utils libpulse)"
fi

# --- the board itself ---
PORT=""
for p in /dev/serial/by-id/*Espressif* /dev/ttyACM*; do
  [ -e "$p" ] && { PORT="$p"; break; }
done

if [ -z "$PORT" ]; then
  bad "no ESP32 found on USB -- plug it in and run this script again"
else
  pass "the board is connected ($PORT)"
  # -L follows the by-id symlink: without it this reads the link's own group
  # (root) and would tell the user to add themselves to the root group.
  GRP="$(stat -Lc '%G' "$PORT")"         # dialout on Debian, uucp on Arch
  case "$GRP" in
    root | "") GRP="dialout" ;;          # no sane distro ships it root-owned
  esac
  if [ -r "$PORT" ] && [ -w "$PORT" ]; then
    pass "access to the serial port"
  else
    bad "no access to $PORT"
    note "sudo usermod -aG $GRP \$USER"
    note "then log out and back in (or run: newgrp $GRP)"
  fi
fi

echo
if [ "$fail" -gt 0 ]; then
  echo "$fail thing(s) missing. Fix them above, then run this script again."
  exit 1
fi

# --- install ---
echo "== Installing the service =="
if ! command -v systemctl >/dev/null 2>&1; then
  echo "No systemd here. Start the daemon by hand instead:"
  echo "  $DAEMON"
  exit 0
fi

UNIT_DIR="$HOME/.config/systemd/user"
mkdir -p "$UNIT_DIR"
cat > "$UNIT_DIR/media-remote.service" <<UNIT
[Unit]
Description=ESP32-C6 touch media remote bridge
After=graphical-session.target
PartOf=graphical-session.target

[Service]
Type=simple
ExecStart=/usr/bin/env python3 "$DAEMON"
Restart=always
RestartSec=3

[Install]
WantedBy=default.target
UNIT

systemctl --user daemon-reload
systemctl --user enable --now media-remote.service
sleep 1
systemctl --user --no-pager --lines=0 status media-remote.service | head -4

echo
echo "Done. The board keeps its own settings -- accent colour and coin choice"
echo "live in its flash and travel with the device, not the machine."
