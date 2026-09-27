#!/usr/bin/env python3
"""Windows half of the bridge between the ESP32-C6 touch remote and the PC.

Windows has no MPRIS and no PulseAudio, so the three jobs are done differently
from the Linux daemon, and each degrades on its own:

  transport  media-key scancodes (VK_MEDIA_*), the same thing a keyboard's
             play button sends. Works with anything that listens to a keyboard,
             which is every player worth naming. Always available.
  metadata   the System Media Transport Controls session (winsdk). Without it
             the display simply shows no track, and the buttons still work.
  volume     Core Audio's IAudioEndpointVolume (pycaw), which can read and set
             an absolute level. Without it the slider falls back to nudging the
             volume-up/down keys and the device shows no percentage.

The wire protocol is identical to the Linux daemon's, so the firmware cannot
tell which machine it is plugged into.
"""

from __future__ import annotations

import argparse
import asyncio
import ctypes
import logging
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from remote_common import (  # noqa: E402  (same directory)
    ALLOWED_COINS, CRYPTO_FRAME_INTERVAL, LOG, MAX_ARTIST, MAX_TITLE,
    POLL_INTERVAL, Prices, SEEK_SECONDS, to_ascii,
)

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit(
        "pyserial is missing.\n"
        "  Run START.bat from the bundled folder, which ships the library,\n"
        "  or install it with:  pip install pyserial"
    )

USB_VID, USB_PID = 0x303A, 0x1001  # Espressif USB JTAG/serial debug unit
BAUD = 115200

# ------------------------------------------------------------- media keys --

VK_MEDIA_NEXT = 0xB0
VK_MEDIA_PREV = 0xB1
VK_MEDIA_PLAY_PAUSE = 0xB3
VK_VOLUME_MUTE = 0xAD
VK_VOLUME_DOWN = 0xAE
VK_VOLUME_UP = 0xAF

KEYEVENTF_EXTENDEDKEY = 0x0001
KEYEVENTF_KEYUP = 0x0002


def tap(vk: int, times: int = 1) -> None:
    user32 = ctypes.windll.user32
    for _ in range(times):
        user32.keybd_event(vk, 0, KEYEVENTF_EXTENDEDKEY, 0)
        user32.keybd_event(vk, 0, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, 0)
        time.sleep(0.005)


# ---------------------------------------------------------- now playing --


class NowPlaying:
    """Polls the Windows media session on a thread and publishes a snapshot."""

    EMPTY = {"status": 0, "title": "", "artist": "", "pos": 0, "len": 0}

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._snap = dict(self.EMPTY)
        self._aloop: asyncio.AbstractEventLoop | None = None
        self._manager = None
        self.available = False
        try:
            import winsdk.windows.media.control  # noqa: F401
        except Exception as exc:
            LOG.warning(
                "No track info (%s) -- the buttons still work, the display shows no title",
                exc,
            )
            return
        self.available = True
        threading.Thread(target=self._run, name="smtc", daemon=True).start()

    def snapshot(self) -> dict:
        with self._lock:
            return dict(self._snap)

    def skip(self, forward: bool) -> str:
        """Move the session one step in the given direction.

        Three tiers, because what "next" can mean depends on what is playing:

          "skipped"     the session has another track, and took it
          "seeked"      no track to move to, so the position moved SEEK_SECONDS
                        instead -- a single video in a browser tab
          "refused"     the session can do neither
          "unavailable" there is no session to ask; only then is a blind media
                        key worth sending

        Asking the controls first is the whole point. A browser tab playing a
        single video registers no track handlers, and Chrome then quietly turns
        both the media key and TrySkipNextAsync into a ten-second seek of its
        own -- which is the right thing by accident, at the cost of the daemon
        not knowing it happened. TrySkipNextAsync even returns True while doing
        it, so its answer cannot be trusted: only the controls know, and only
        before the call.
        """
        loop, manager = self._aloop, self._manager
        if loop is None or manager is None:
            return "unavailable"

        async def ask() -> str:
            session = manager.get_current_session()
            if session is None:
                return "unavailable"
            controls = session.get_playback_info().controls

            if (controls.is_next_enabled if forward
                    else controls.is_previous_enabled):
                if forward:
                    await session.try_skip_next_async()
                else:
                    await session.try_skip_previous_async()
                return "skipped"

            if controls.is_playback_position_enabled:
                tl = session.get_timeline_properties()
                start = tl.start_time.total_seconds()
                here = tl.position.total_seconds() - start
                end = tl.end_time.total_seconds() - start
                target = max(0.0, here + (SEEK_SECONDS if forward else -SEEK_SECONDS))
                if end > 0:
                    target = min(target, end)
                # WinRT counts 100-nanosecond ticks from the timeline's own
                # start, which is not always zero.
                await session.try_change_playback_position_async(
                    int((target + start) * 10_000_000)
                )
                return "seeked"

            return "refused"

        try:
            return asyncio.run_coroutine_threadsafe(ask(), loop).result(timeout=2)
        except Exception as exc:
            LOG.debug("skip: %s", exc)
            return "unavailable"

    def _run(self) -> None:
        try:
            asyncio.run(self._loop())
        except Exception as exc:
            LOG.warning("track info stopped: %s", exc)

    async def _loop(self) -> None:
        from winsdk.windows.media.control import (
            GlobalSystemMediaTransportControlsSessionManager as Manager,
        )
        from winsdk.windows.media.control import (
            GlobalSystemMediaTransportControlsSessionPlaybackStatus as Status,
        )

        manager = await Manager.request_async()
        # Reachable from the serve thread so a skip can be asked of the session
        # itself rather than guessed at with a media key -- see skip().
        self._manager = manager
        self._aloop = asyncio.get_running_loop()
        while True:
            snap = dict(self.EMPTY)
            try:
                session = manager.get_current_session()
                if session is not None:
                    info = session.get_playback_info()
                    status = info.playback_status
                    snap["status"] = (
                        1 if status == Status.PLAYING
                        else 2 if status == Status.PAUSED
                        else 0
                    )

                    props = await session.try_get_media_properties_async()
                    title = props.title or ""
                    artist = props.artist or props.album_artist or ""
                    if not title:
                        title = props.album_title or ""
                    snap["title"] = to_ascii(str(title))[:MAX_TITLE]
                    snap["artist"] = to_ascii(str(artist))[:MAX_ARTIST]

                    tl = session.get_timeline_properties()
                    start = tl.start_time.total_seconds()
                    snap["pos"] = max(0, int(tl.position.total_seconds() - start))
                    snap["len"] = max(0, int(tl.end_time.total_seconds() - start))
            except Exception as exc:
                LOG.debug("track info: %s", exc)

            with self._lock:
                self._snap = snap
            await asyncio.sleep(POLL_INTERVAL)


# ---------------------------------------------------------------- volume --


class Mixer:
    """Core Audio when it is available, media keys when it is not."""

    def __init__(self) -> None:
        self._endpoint = None
        self._last_target: int | None = None
        try:
            import comtypes
            from pycaw.pycaw import AudioUtilities

            comtypes.CoInitialize()  # this thread owns the COM apartment
            # GetSpeakers() hands back pycaw's AudioDevice wrapper, not the raw
            # IMMDevice, so the Activate/cast dance lives behind EndpointVolume.
            self._endpoint = AudioUtilities.GetSpeakers().EndpointVolume
            LOG.info("volume: Core Audio")
        except Exception as exc:
            LOG.warning(
                "No Core Audio (%s) -- volume goes through the media keys, "
                "and the level cannot be read back",
                exc,
            )

    def state(self) -> tuple[bool, int]:
        if self._endpoint is None:
            return False, -1
        try:
            muted = bool(self._endpoint.GetMute())
            level = round(self._endpoint.GetMasterVolumeLevelScalar() * 100)
            return muted, level
        except Exception as exc:
            LOG.debug("reading the volume failed: %s", exc)
            return False, -1

    def set_volume(self, percent: int) -> None:
        percent = max(0, min(100, percent))
        if self._endpoint is not None:
            try:
                self._endpoint.SetMasterVolumeLevelScalar(percent / 100.0, None)
                self._endpoint.SetMute(0, None)  # asking for a level means unmute
                LOG.info("VOL -> %d%%", percent)
                return
            except Exception as exc:
                LOG.warning("could not set the volume: %s", exc)

        # Fallback: the device streams absolute targets while you drag, so the
        # difference between two of them is how far to nudge. Each key press is
        # worth roughly two percent on Windows.
        if self._last_target is not None:
            steps = int(round((percent - self._last_target) / 2.0))
            if steps > 0:
                tap(VK_VOLUME_UP, min(steps, 50))
            elif steps < 0:
                tap(VK_VOLUME_DOWN, min(-steps, 50))
        self._last_target = percent

    def toggle_mute(self) -> None:
        if self._endpoint is not None:
            try:
                self._endpoint.SetMute(0 if self._endpoint.GetMute() else 1, None)
                LOG.info("MUTE toggled")
                return
            except Exception as exc:
                LOG.warning("could not toggle mute: %s", exc)
        tap(VK_VOLUME_MUTE)


# ---------------------------------------------------------------- serial --


def find_port(explicit: str | None) -> str | None:
    if explicit:
        return explicit
    for p in list_ports.comports():
        if p.vid == USB_VID and p.pid == USB_PID:
            return p.device
    return None


def open_port(name: str) -> serial.Serial:
    """Open the CDC port with DTR asserted.

    The DTR part is not optional: the ESP32-C6's USB Serial/JTAG peripheral
    withholds everything it writes until the host looks "connected", so a port
    opened without it stays silent forever.
    """
    ser = serial.Serial()
    ser.port = name
    ser.baudrate = BAUD
    ser.timeout = 0  # non-blocking reads
    ser.write_timeout = 2
    ser.dtr = True
    ser.rts = True
    ser.open()
    ser.reset_input_buffer()
    return ser


# ------------------------------------------------------------------ main --


def handle_command(cmd: str, np: NowPlaying, mixer: Mixer,
                   prices: Prices) -> bool:
    """Returns True if the caller should refresh the device immediately."""
    if cmd == "PLAYPAUSE":
        tap(VK_MEDIA_PLAY_PAUSE)
        LOG.info("PLAYPAUSE")
        return True
    if cmd in ("NEXT", "PREV"):
        forward = cmd == "NEXT"
        outcome = np.skip(forward)
        if outcome == "unavailable":
            tap(VK_MEDIA_NEXT if forward else VK_MEDIA_PREV)
            LOG.info("%s (media key)", cmd)
        elif outcome == "seeked":
            LOG.info("%s -- no %s track, seeked %+d s", cmd,
                     "next" if forward else "previous",
                     SEEK_SECONDS if forward else -SEEK_SECONDS)
        elif outcome == "refused":
            LOG.info("%s -- the player can neither change track nor seek", cmd)
        else:
            LOG.info("%s", cmd)
        return True
    if cmd == "MUTE":
        mixer.toggle_mute()
        return True
    if cmd.startswith("VOL "):
        try:
            mixer.set_volume(int(cmd[4:]))
        except ValueError:
            return False
        return True
    if cmd.startswith("CUR "):
        return prices.set_currency(cmd[4:])
    if cmd.startswith("COINS "):
        return prices.set_coins(cmd[6:].strip().split(","))
    LOG.debug("unknown command %r", cmd)
    return False


def serve(port_name: str, np: NowPlaying, mixer: Mixer, prices: Prices) -> None:
    ser = open_port(port_name)
    LOG.info("connected to %s", port_name)
    buf = b""
    last_poll = 0.0
    last_crypto = 0.0
    spark_turn = 0

    try:
        while True:
            waiting = ser.in_waiting
            if waiting:
                buf += ser.read(waiting)
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.decode("utf-8", "replace").strip()
                    if not line:
                        continue
                    LOG.debug("dev: %s", line)
                    if line.startswith("CMD "):
                        if handle_command(line[4:].strip(), np, mixer, prices):
                            last_poll = 0.0  # refresh the device at once
            else:
                time.sleep(0.02)

            now = time.monotonic()

            if now - last_crypto >= CRYPTO_FRAME_INTERVAL:
                last_crypto = now
                ser.write(prices.frame().encode("ascii", "ignore"))
                spark = prices.spark_frame(spark_turn % max(1, len(prices.coins)))
                spark_turn += 1
                if spark:
                    ser.write(spark.encode("ascii", "ignore"))

            if now - last_poll < POLL_INTERVAL:
                continue
            last_poll = now

            snap = np.snapshot()
            muted, vol = mixer.state()
            frame = "NP|{status}|{muted}|{vol}|{pos}|{len}|{title}|{artist}\n".format(
                status=snap["status"], muted=int(muted), vol=vol,
                pos=snap["pos"], len=snap["len"],
                title=snap["title"], artist=snap["artist"],
            )
            # Always resend: the device uses these frames as a host heartbeat.
            ser.write(frame.encode("ascii", "ignore"))
    finally:
        try:
            ser.close()
        except Exception:
            pass


def monitor(port_name: str) -> int:
    ser = open_port(port_name)
    print(f"-- {port_name} (Ctrl-C to quit) --", flush=True)
    try:
        while True:
            n = ser.in_waiting
            if n:
                sys.stdout.write(ser.read(n).decode("utf-8", "replace"))
                sys.stdout.flush()
            else:
                time.sleep(0.05)
    except KeyboardInterrupt:
        return 0
    finally:
        ser.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-p", "--port", help="COM port, e.g. COM5 (default: auto-detect)")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--monitor", action="store_true",
                    help="just print the board's serial output and exit")
    ap.add_argument("--list-ports", action="store_true",
                    help="list every serial port Windows can see")
    args = ap.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)-7s %(message)s",
        datefmt="%H:%M:%S",
    )

    if args.list_ports:
        for p in list_ports.comports():
            vid = f"{p.vid:04X}" if p.vid else "----"
            pid = f"{p.pid:04X}" if p.pid else "----"
            mark = " <-- the remote" if (p.vid, p.pid) == (USB_VID, USB_PID) else ""
            print(f"{p.device:8} {vid}:{pid}  {p.description}{mark}")
        return 0

    if args.monitor:
        path = find_port(args.port)
        if not path:
            LOG.error("no ESP32 found on USB")
            return 1
        return monitor(path)

    np = NowPlaying()
    mixer = Mixer()
    prices = Prices()

    while True:
        path = find_port(args.port)
        if not path:
            LOG.info("waiting for the remote to be plugged in...")
            while not path:
                time.sleep(2)
                path = find_port(args.port)
        try:
            serve(path, np, mixer, prices)
        except KeyboardInterrupt:
            LOG.info("shutting down")
            return 0
        except Exception as exc:
            LOG.warning("%s went away (%s); waiting for it to come back",
                        path, exc)
            time.sleep(2)


if __name__ == "__main__":
    sys.exit(main())
