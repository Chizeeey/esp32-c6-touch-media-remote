#!/usr/bin/env python3
"""Bridge between the ESP32-C6 touch remote and this desktop's media players.

The firmware sends button presses over USB CDC; this daemon turns them into
MPRIS calls (Spotify, Firefox/Chrome YouTube tabs, VLC, ...) and PulseAudio
mute toggles, and streams the current track back for the display.

Dependencies are whatever a normal desktop already has: python3, python3-gi
(GLib/Gio, for D-Bus) and pactl. No pip install required.
"""

from __future__ import annotations

import argparse
import fcntl
import glob
import logging
import os
import select
import struct
import subprocess
import sys
import termios
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    import gi  # python3-gi

    gi.require_version("Gio", "2.0")
    from gi.repository import Gio, GLib
except (ImportError, ValueError) as exc:  # a fresh machine may not have it
    sys.exit(
        f"The Python GObject bindings are missing ({exc}).\n"
        "  Debian/Ubuntu/Mint:  sudo apt install python3-gi\n"
        "  Fedora:              sudo dnf install python3-gobject\n"
        "  Arch:                sudo pacman -S python-gobject"
    )

from remote_common import (  # noqa: E402  (same directory)
    ALLOWED_COINS, CRYPTO_FRAME_INTERVAL, LOG, MAX_ARTIST, MAX_TITLE,
    POLL_INTERVAL, Prices, SEEK_SECONDS, to_ascii,
)

MPRIS_PREFIX = "org.mpris.MediaPlayer2."
MPRIS_PATH = "/org/mpris/MediaPlayer2"
PLAYER_IFACE = "org.mpris.MediaPlayer2.Player"

# --------------------------------------------------------------------- MPRIS


class Players:
    """Tracks the MPRIS players on the session bus and picks the active one."""

    def __init__(self) -> None:
        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        self.preferred: str | None = None

    def _names(self) -> list[str]:
        reply = self.bus.call_sync(
            "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "ListNames", None,
            GLib.VariantType("(as)"), Gio.DBusCallFlags.NONE, 1000, None,
        )
        return sorted(n for n in reply.unpack()[0] if n.startswith(MPRIS_PREFIX))

    def _get(self, name: str, prop: str):
        try:
            reply = self.bus.call_sync(
                name, MPRIS_PATH, "org.freedesktop.DBus.Properties", "Get",
                GLib.Variant("(ss)", (PLAYER_IFACE, prop)),
                GLib.VariantType("(v)"), Gio.DBusCallFlags.NONE, 800, None,
            )
            return reply.unpack()[0]
        except GLib.Error:
            return None

    def active(self) -> str | None:
        """Prefer a player that is actually playing, else the last one used."""
        names = self._names()
        if not names:
            self.preferred = None
            return None

        playing = [n for n in names if self._get(n, "PlaybackStatus") == "Playing"]
        if playing:
            # Keep the current choice if it is among them, to avoid flapping.
            if self.preferred in playing:
                return self.preferred
            self.preferred = playing[0]
            return self.preferred

        if self.preferred in names:
            return self.preferred
        self.preferred = names[0]
        return self.preferred

    def call(self, method: str, params: GLib.Variant | None = None) -> bool:
        name = self.active()
        if not name:
            LOG.info("no MPRIS player for %s", method)
            return False
        try:
            self.bus.call_sync(
                name, MPRIS_PATH, PLAYER_IFACE, method, params, None,
                Gio.DBusCallFlags.NONE, 1500, None,
            )
            LOG.info("%s -> %s", method, name.removeprefix(MPRIS_PREFIX))
            # A transport change makes the next poll worth doing immediately.
            self.preferred = name
            return True
        except GLib.Error as exc:
            LOG.warning("%s on %s failed: %s", method, name, exc.message)
            return False

    def skip(self, forward: bool) -> str:
        """Move the active player one step in the given direction.

        The same three tiers as the Windows daemon, so the remote behaves the
        same whichever machine it is plugged into:

          "skipped"     the player has another track, and took it
          "seeked"      no track to move to, so the position moved SEEK_SECONDS
                        instead -- a browser tab playing a single video
          "refused"     the player can do neither
          "unavailable" there is no player to ask

        Asking CanGoNext first is what keeps the button honest. A single video
        in a browser tab reports CanGoNext false and CanSeek true, and calling
        Next regardless leaves it to the browser to decide what that means --
        which turns out to be a seek of its own choosing, with the daemon none
        the wiser that the track never changed.

        Note that many players treat Previous as "restart this track" on the
        first press. That is still what people expect from a remote, so it is
        left alone.
        """
        name = self.active()
        if not name:
            return "unavailable"

        if self._get(name, "CanGoNext" if forward else "CanGoPrevious"):
            if self.call("Next" if forward else "Previous"):
                return "skipped"
            return "refused"

        if self._get(name, "CanSeek"):
            offset = SEEK_SECONDS * 1_000_000  # MPRIS counts microseconds
            # Seek is relative, and the spec puts the clamping at both ends of
            # the track on the player, so there is no arithmetic to get wrong.
            if self.call("Seek",
                         GLib.Variant("(x)", (offset if forward else -offset,))):
                return "seeked"
            return "refused"

        return "refused"

    def snapshot(self) -> dict:
        name = self.active()
        if not name:
            return {"status": 0, "title": "", "artist": "", "pos": 0, "len": 0}

        status_text = self._get(name, "PlaybackStatus") or "Stopped"
        status = {"Playing": 1, "Paused": 2}.get(status_text, 0)

        meta = self._get(name, "Metadata") or {}
        title = meta.get("xesam:title") or ""
        artist = meta.get("xesam:artist") or meta.get("xesam:albumArtist") or ""
        if isinstance(artist, (list, tuple)):
            artist = ", ".join(a for a in artist if a)

        # Browsers often leave the title empty and use the tab name instead.
        if not title:
            title = meta.get("xesam:url", "") or name.removeprefix(MPRIS_PREFIX)
        if not artist:
            artist = name.removeprefix(MPRIS_PREFIX).split(".")[0].capitalize()

        length_us = meta.get("mpris:length") or 0
        pos_us = self._get(name, "Position") or 0

        return {
            "status": status,
            "title": to_ascii(str(title))[:MAX_TITLE],
            "artist": to_ascii(str(artist))[:MAX_ARTIST],
            "pos": int(pos_us) // 1_000_000,
            "len": int(length_us) // 1_000_000,
        }


# ---------------------------------------------------------------- PulseAudio


def pactl(*args: str) -> str:
    try:
        return subprocess.run(
            ["pactl", *args], capture_output=True, text=True, timeout=2,
        ).stdout.strip()
    except (OSError, subprocess.SubprocessError) as exc:
        LOG.warning("pactl %s failed: %s", " ".join(args), exc)
        return ""


def mixer_state() -> tuple[bool, int]:
    muted = pactl("get-sink-mute", "@DEFAULT_SINK@").endswith("yes")
    vol = -1
    out = pactl("get-sink-volume", "@DEFAULT_SINK@")
    for token in out.replace("/", " ").split():
        if token.endswith("%"):
            try:
                vol = int(token[:-1])
            except ValueError:
                pass
            break
    return muted, vol


def toggle_mute() -> None:
    pactl("set-sink-mute", "@DEFAULT_SINK@", "toggle")
    LOG.info("MUTE toggled")


def set_volume(percent: int) -> None:
    percent = max(0, min(100, percent))
    pactl("set-sink-volume", "@DEFAULT_SINK@", f"{percent}%")
    # Scrubbing the slider means "I want to hear this much", so lift any mute.
    pactl("set-sink-mute", "@DEFAULT_SINK@", "0")
    LOG.info("VOL -> %d%%", percent)


# -------------------------------------------------------------------- crypto


# -------------------------------------------------------------------- serial


def find_port(explicit: str | None) -> str | None:
    if explicit:
        return explicit if os.path.exists(explicit) else None
    # The stable by-id symlink survives re-enumeration; fall back to ttyACM*.
    for pattern in ("/dev/serial/by-id/*Espressif*", "/dev/ttyACM*"):
        hits = sorted(glob.glob(pattern))
        if hits:
            return hits[0]
    return None


TIOCM_DTR = 0x002
TIOCM_RTS = 0x004


def open_port(path: str):
    """Open the CDC port raw at 115200 with DTR/RTS asserted.

    The DTR part is not optional: the ESP32-C6's USB Serial/JTAG peripheral
    withholds everything it writes until the host looks "connected", so a port
    opened without it stays silent forever.
    """
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
    try:
        attrs = termios.tcgetattr(fd)
        attrs[0] = 0  # iflag: no translation, no flow control
        attrs[1] = 0  # oflag
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[3] = 0  # lflag: non-canonical, no echo
        attrs[4] = attrs[5] = termios.B115200
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        fcntl.ioctl(fd, termios.TIOCMBIS, struct.pack("I", TIOCM_DTR | TIOCM_RTS))
        termios.tcflush(fd, termios.TCIOFLUSH)
    except Exception:
        os.close(fd)
        raise
    return os.fdopen(fd, "r+b", buffering=0)


# ---------------------------------------------------------------------- main


def handle_command(cmd: str, players: Players, prices: Prices) -> bool:
    """Returns True if the caller should refresh the device immediately."""
    if cmd == "PLAYPAUSE":
        return players.call("PlayPause")
    if cmd in ("NEXT", "PREV"):
        forward = cmd == "NEXT"
        outcome = players.skip(forward)
        if outcome == "seeked":
            LOG.info("%s -- no %s track, seeked %+d s", cmd,
                     "next" if forward else "previous",
                     SEEK_SECONDS if forward else -SEEK_SECONDS)
        elif outcome == "refused":
            LOG.info("%s -- the player can neither change track nor seek", cmd)
        elif outcome == "unavailable":
            LOG.info("no MPRIS player for %s", cmd)
        return outcome in ("skipped", "seeked")
    if cmd == "MUTE":
        toggle_mute()
        return True
    if cmd.startswith("CUR "):
        return prices.set_currency(cmd[4:])
    if cmd.startswith("COINS "):
        return prices.set_coins(cmd[6:].strip().split(","))
    if cmd.startswith("VOL "):
        try:
            set_volume(int(cmd[4:]))
        except ValueError:
            LOG.debug("bad volume in %r", cmd)
            return False
        return True
    LOG.debug("ignoring %r", cmd)
    return False


def serve(port_path: str, players: Players, prices: Prices) -> None:
    fh = open_port(port_path)
    LOG.info("connected to %s", port_path)
    buf = b""
    last_poll = 0.0
    last_crypto = 0.0
    spark_turn = 0
    last_frame = None

    try:
        while True:
            ready, _, _ = select.select([fh], [], [], 0.2)
            if ready:
                chunk = fh.read(256)
                if chunk:
                    buf += chunk
                    while b"\n" in buf:
                        raw, buf = buf.split(b"\n", 1)
                        line = raw.decode("utf-8", "replace").strip()
                        if not line:
                            continue
                        LOG.debug("dev: %s", line)
                        if line.startswith("CMD "):
                            if handle_command(line[4:].strip(), players, prices):
                                last_poll = 0.0  # force an immediate refresh
                elif chunk == b"":
                    pass

            now = time.monotonic()

            if now - last_crypto >= CRYPTO_FRAME_INTERVAL:
                last_crypto = now
                fh.write(prices.frame().encode("ascii", "ignore"))
                # One sparkline per tick, round-robin: cheap, and it refills
                # itself if the board reboots without the host noticing.
                spark = prices.spark_frame(spark_turn % max(1, len(prices.coins)))
                spark_turn += 1
                if spark:
                    fh.write(spark.encode("ascii", "ignore"))

            if now - last_poll < POLL_INTERVAL:
                continue
            last_poll = now

            snap = players.snapshot()
            muted, vol = mixer_state()
            frame = "NP|{status}|{muted}|{vol}|{pos}|{len}|{title}|{artist}\n".format(
                status=snap["status"], muted=int(muted), vol=vol,
                pos=snap["pos"], len=snap["len"],
                title=snap["title"], artist=snap["artist"],
            )
            # Always resend: the device uses these frames as a host heartbeat.
            fh.write(frame.encode("ascii", "ignore"))
            if frame != last_frame:
                LOG.debug("host: %s", frame.strip())
                last_frame = frame
    finally:
        fh.close()


def monitor(port_path: str) -> int:
    """Echo whatever the board says, without touching the media players."""
    fh = open_port(port_path)
    print(f"-- {port_path} (Ctrl-C to quit) --", flush=True)
    try:
        while True:
            if select.select([fh], [], [], 0.3)[0]:
                chunk = fh.read(512)
                if chunk:
                    sys.stdout.write(chunk.decode("utf-8", "replace"))
                    sys.stdout.flush()
    except KeyboardInterrupt:
        return 0
    finally:
        fh.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-p", "--port", help="serial device (default: auto-detect)")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--monitor", action="store_true",
                    help="just print the board's serial output and exit")
    args = ap.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)-7s %(message)s",
        datefmt="%H:%M:%S",
    )

    if args.monitor:
        path = find_port(args.port)
        if not path:
            LOG.error("no ESP32 found on USB")
            return 1
        return monitor(path)

    players = Players()
    prices = Prices()

    while True:
        path = find_port(args.port)
        if not path:
            LOG.info("waiting for the remote to be plugged in...")
            while not path:
                time.sleep(2)
                path = find_port(args.port)
        try:
            serve(path, players, prices)
        except KeyboardInterrupt:
            LOG.info("bye")
            return 0
        except (OSError, subprocess.CalledProcessError) as exc:
            LOG.warning("%s went away (%s); waiting for it to come back", path, exc)
            time.sleep(2)


if __name__ == "__main__":
    sys.exit(main())
