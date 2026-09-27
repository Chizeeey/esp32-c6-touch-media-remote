# Shared settings for the build/flash/monitor scripts.
export PATH="$HOME/.local/bin:$PATH"

FQBN="esp32:esp32:esp32c6:CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB"
SKETCH="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/firmware/media_remote"

find_port() {
  local p
  for p in /dev/serial/by-id/*Espressif* /dev/ttyACM*; do
    [ -e "$p" ] && { echo "$p"; return 0; }
  done
  return 1
}
