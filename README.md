# ESP32-C6 Touch Media Remote

A physical media remote for your PC, built on a Waveshare
ESP32-C6-Touch-LCD-1.9. Previous, play/pause and next control whatever is
playing on the machine — Spotify, a YouTube tab, VLC, mpv. Works on **Linux**
and **Windows**.

The transport buttons own the screen. Track details and volume live in two
panels that slide in when you pull them out and leave again on their own, so you
never have to read text to hit pause.

```
   at rest              swipe down from top     swipe up from bottom
 ┌─────────┐            ┌─────────┐            ┌─────────┐
 │   ▁▁▁   │  ← handle  │NOW PLAY.│            │         │
 │         │            │ Title   │            │   ◀◀    │
 │         │            │ Artist  │            │  ( ▶ )  │
 │   ◀◀    │            │ ━━━──── │            │   ▶▶    │
 │  (( ▶ ))│ ← the ring │ 1:14 3:32│           ├─────────┤
 │   ▶▶    │   is the   ├─────────┤            │ 🔊  75% │
 │         │   position │  ◀◀ ▶ ▶▶│            │ ━━━━─── │
 │   ▔▔▔   │            └─────────┘            └─────────┘
 └─────────┘             gone after 5 s        gone after 10 s
```

## How it fits together

The board does nothing over the network. It talks to the PC over the same USB-C
cable that powers it.

```
  ESP32-C6                USB CDC serial              host daemon
  ┌──────────┐                                    ┌──────────────────┐
  │ touch UI │ ──── CMD PLAYPAUSE ──────────────► │ Linux: MPRIS     │
  │ ST7789   │ ◄─── NP|1|0|75|12|214|title|... ─  │ Windows: SMTC    │
  └──────────┘                                    └──────────────────┘
```

Both halves speak the same wire protocol, so the firmware cannot tell which
kind of machine it is plugged into. What differs is underneath:

| | Linux | Windows |
|---|---|---|
| Transport | MPRIS over D-Bus | System Media Transport Controls |
| Metadata | MPRIS | the same SMTC session |
| Volume | `pactl` (PulseAudio/PipeWire) | Core Audio (`IAudioEndpointVolume`) |

Going through the desktop's own media layer is the point: no Spotify API key, no
browser extension. If Chromium is playing a YouTube video, it shows up as a
media session by itself.

## Quick start

A board fresh from Waveshare is empty, so flash it first — one command, no
toolchain needed, see [Flashing the firmware](#flashing-the-firmware). Then set
up the host side for your machine.

### Windows

No Python, no dependencies, no administrator rights. Grab the bundle from the
[Releases](https://github.com/Chizeeey/esp32-c6-touch-media-remote/releases)
page, or build it yourself in one command (see
[Building the Windows bundle](#building-the-windows-bundle)). Then:

1. Plug the remote into a USB port **with data lines**. A charge-only cable
   gives it power but no connection.
2. Double-click `Start.bat` to try it in a visible window.
3. Happy with it? Double-click `Install.bat`. It copies the program to your
   local disk and starts it at every login, without a window.

`Uninstall.bat` undoes that. `Troubleshoot.bat` lists the serial ports and then
runs with full logging.

### Linux

Everything needed is already on a normal desktop: `python3` with `python3-gi`
(GLib/Gio, for D-Bus) and `pactl`. No `pip install`.

```bash
sudo usermod -aG dialout $USER     # once; log out and in again afterwards
./host/setup.sh
```

`setup.sh` checks the prerequisites first, finds the board, verifies you can
open the serial port, and installs a systemd **user** service only once all of
that is in place. If something is missing it prints the exact command for the
distro you are on — `apt`, `dnf` and `pacman` name these packages differently.

To run it by hand instead:

```bash
./host/media_remote_linux.py -v
```

## Gestures

| Gesture | Does |
|---|---|
| Tap the left / middle / right third | Previous, play/pause, next |
| Swipe down from the top | Track details — title, artist, position. Gone after 5 s |
| Swipe up from the bottom | Volume. Gone after 10 s |
| Drag sideways in the volume panel | Sets the volume, and unmutes. The panel stays |
| Tap inside the volume panel | Toggles mute **and** closes the panel |
| Tap inside the track panel | Closes it |
| Swipe the other way in an open panel | Closes it without changing anything |
| Tap the top-right button | Crypto prices. Gone after 15 s, or on a tap |
| Tap the top-left button | Opens settings |

When the track changes, the details panel appears by itself and leaves again.

The tap targets cover the whole free area, not just the circles, so you hit them
without aiming. Position shows as a ring around the play button while the panel
is closed — ordinary use needs no text at all.

## What previous and next actually do

Not every player has a next track, and a remote that silently does the wrong
thing is worse than one that does nothing. So the daemon asks the session what
it is capable of, and picks from three tiers:

```
player has another track      → change track     (Spotify, a playlist)
no track, but can seek        → move ±10 seconds (a single video in a tab)
neither                       → do nothing, and say so in the log
no media session at all       → fall back to a media key (Windows only)
```

This matters more than it sounds. A browser tab playing one video registers no
track handlers, and if you send it a bare "next track" the browser decides what
that means — Chrome turns it into a ten-second seek of its own, and the daemon
never learns the track did not change. Asking first is what keeps the button
honest, and it is why the seek is deliberate here rather than accidental.

`SEEK_SECONDS` in [host/remote_common.py](host/remote_common.py) is the step.
Both hosts read it from there.

## Crypto prices

The top-right button opens spot prices for the coins you picked, in the
currency you picked. Each coin gets a card with its price, a coloured pill for
the last 24 hours, and a graph over those same 24 hours.

Each graph is scaled to its own coin's range rather than a shared one: it shows
the shape of the day, while the number beside it carries the level. Sharing a
scale would flatten Cardano into a line next to Bitcoin.

The board has no network connection. The daemon fetches the prices from
[CoinGecko](https://www.coingecko.com/)'s open API and pushes them down the same
serial line as everything else. No API key, no WiFi setup on the board. Prices
refresh every five minutes; if a call fails, the previous values stand.

Prices from 1000 up show as whole units with the thousands separated — "802 234"
in NOK, "802,234" in USD. Below 1000 the decimals come along, "2,41", or ADA
would read as just "2". Below one unit, four significant digits stay,
"0,000142". Which separators are used follows the currency rather than the
host's locale, because the person reading the screen picked the currency. The
rules live in `format_price` in
[host/remote_common.py](host/remote_common.py).

Formatting happens on the PC and arrives ready-made. The catalogue spans Bitcoin
at ~800 000 kr and Shiba Inu at ~0.0001 kr, and no fixed-point integer carries
both ends — micro-kroner overflow an int32 at the top, fractions of a krone
round to zero at the bottom. The host has the float, so the host places the
decimal point.

Graphs go over as 32 points per coin, one coin per update in rotation, so they
refill themselves if the board restarts without the host noticing.

## Settings

The top-left button opens a panel with three things.

**Colour.** Six accents. The whole palette — cards, rings, hint text, pressed
surfaces — is derived from the one colour, so nothing is left behind in the
previous theme. The background is always true black.

**Currency.** NOK, USD, EUR, GBP or SEK. NOK is the default. Changing it clears
the prices on screen rather than relabelling them: everything cached is
denominated in the currency you just left, and showing dollar figures under a
NOK heading would be worse than showing "--" for the second it takes to
refetch.

**Coins.** 24 of them in a scrollable list; drag up or down inside it. Between
one and three can be shown at a time, and the cards share the height according
to how many you picked. With three already chosen you have to remove one before
adding another — the panel will not swap something out behind your back.

The handles at the top and bottom of the screen stay white in every theme. They
are the only hint that the volume and track panels exist, so they should never
recede into an accent colour.

All three live in the board's flash and survive a power cut. The board sends
your choice to the daemon when they connect, so the host needs no configuration
of its own — it serves what it is asked for, and only from a list it already
knows.

## Taking it to another machine

The firmware is in the board's flash, and so are your settings — accent colour
and coin choice travel with the device, not the machine. A new PC needs only the
host side.

On Windows that is the bundle. On Linux, copy [host/](host/) — two files is
enough — and run `./host/setup.sh`.

Two things that differ between Linux machines, both handled for you:

**The serial port group is not called the same everywhere.** Debian, Ubuntu and
Mint use `dialout`; Arch uses `uucp`. The script reads the group off the device
itself instead of guessing.

**Without `pactl`, everything except volume works.** It is reported as missing
but does not stop the install — the transport goes over D-Bus and does not care
about the mixer.

## Flashing the firmware

Grab `media_remote-esp32c6-v1.0.0.bin` from the
[Releases](https://github.com/Chizeeey/esp32-c6-touch-media-remote/releases)
page. All you need to write it is `esptool`, which is one pip install — not the
gigabyte of Arduino toolchain:

```bash
pip install esptool
esptool --chip esp32c6 --port COM3 --baud 921600 write-flash 0x0 media_remote-esp32c6-v1.0.0.bin
```

On Linux the port is usually `/dev/ttyACM0`, and you need to be in the group
that owns it — see the Linux quick start above.

That is the whole 8 MB image, so the board comes up on its defaults: NOK,
purple, and BTC/ETH/ADA. On a fresh board that is exactly what you want; on a
board you have already set up, expect to pick your colour and coins again.

The firmware does not report a version over serial yet, so the file name is the
only thing that tells you which build is on a board.

## Building the firmware

You only need this to change what runs on the board.

`arduino-cli` and the ESP32 core are about a gigabyte of download, and install
into your home directory without root. On Windows the shell scripts below will
not run, but `arduino-cli` itself works the same — compile with the FQBN below
and pass `-p COM3` to `upload`.

```bash
./scripts/build.sh          # compile
./scripts/flash.sh          # compile and upload
./scripts/monitor.sh        # watch the board's serial output
```

The board settings are pinned in [scripts/common.sh](scripts/common.sh):

```
esp32:esp32:esp32c6  CDCOnBoot=cdc  FlashSize=8M  PartitionScheme=default_8MB
```

`CDCOnBoot=cdc` is not optional — it is what puts the USB Serial/JTAG peripheral
on the port the daemon opens.

## Building the Windows bundle

```powershell
.\packaging\windows\build.ps1 -Zip
```

That writes `dist\esp32-media-remote-windows\` and a matching `.zip`: the
embeddable CPython from python.org, the dependencies pip-installed beside it,
and the `.bat` files. Around 70 MB, and none of it is committed — pip brings
each package's own licence metadata along, which a hand-copied `lib/` folder
would lose.

If the repository sits on a network share, note that the bundle will not *run*
from there: shares commonly grant read but not execute, and Windows refuses to
start `python.exe` with a bare "Access denied". The build says so when it
finishes. Copy the folder to a local disk, or use its `Install.bat`, which does
that for you.

## Troubleshooting

**See what the board is saying.** `./scripts/monitor.sh` on Linux, or
`Troubleshoot.bat` on Windows, shows the boot log including an I2C scan and
whether the touch chip was found. Stop the daemon first — only one process can
hold the port:

```bash
systemctl --user stop media-remote
```

**Windows cannot find the board.** It is usually the cable. Plenty of USB-C
cables are charge-only, with no data lines. `Troubleshoot.bat` lists every
serial port Windows can see and marks the remote if it is there.

**Touch hits the wrong button.** Set `TOUCH_DEBUG` to `1` in
[firmware/media_remote/config.h](firmware/media_remote/config.h), reflash, and
watch the `RAW x= y=` lines in the monitor. Then adjust `LCD_ROTATION`, which
drives both the image and the touch mapping, so the two cannot drift apart.

**The colours look inverted.** Flip `LCD_IPS` in the same file. This panel is
IPS and needs `1`; Waveshare's own example sets `0`, which turns black white.

**Nothing happens when you tap.** On Linux, check that the daemon is running
(`systemctl --user status media-remote`) and that the player actually appears on
D-Bus:

```bash
gdbus call --session --dest org.freedesktop.DBus \
  --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.ListNames
```

Firefox needs `media.hardwaremediakeys.enabled` set to `true` in `about:config`
before it registers with MPRIS. Chromium does it by itself.

**Buttons work but there is no title.** The media session is unavailable. That
degrades on its own: the buttons keep working, the display just shows no track.

**The volume field shows `--`.** The mixer could not be read. The slider falls
back to nudging the volume keys, so it still works, but the level cannot be
shown.

## Orientation

The board is meant to stand with the USB-C connector up. `LCD_ROTATION` in
[firmware/media_remote/config.h](firmware/media_remote/config.h) controls it:
`0` and `2` are the two portrait variants, `1` and `3` landscape. The touch
mapping follows the same setting, so image and finger stay in step whichever you
pick.

## Files

| Path | What |
|---|---|
| [firmware/media_remote/](firmware/media_remote/) | Arduino sketch for the ESP32-C6 |
| [firmware/media_remote/config.h](firmware/media_remote/config.h) | Pinout and display settings |
| [firmware/media_remote/cst816.h](firmware/media_remote/cst816.h) | Touch driver |
| [firmware/media_remote/layout.h](firmware/media_remote/layout.h) | The geometry of one frame |
| [firmware/media_remote/model.h](firmware/media_remote/model.h) | Theme and coin types |
| [host/remote_common.py](host/remote_common.py) | Wire protocol, prices, text — what both hosts share |
| [host/media_remote_linux.py](host/media_remote_linux.py) | The MPRIS / PulseAudio bridge |
| [host/media_remote_win.py](host/media_remote_win.py) | The SMTC / Core Audio bridge |
| [host/setup.sh](host/setup.sh) | Checks prerequisites and installs the Linux service |
| [packaging/windows/](packaging/windows/) | Bundle builder and the `.bat` files |
| [scripts/](scripts/) | Build, flash, serial monitor |
| [docs/hardware.md](docs/hardware.md) | Pinout and sources |

## The serial protocol

Line-based ASCII, 115200 baud.

Board → PC:

```
HELLO media-remote esp32c6
CMD PREV | CMD PLAYPAUSE | CMD NEXT | CMD MUTE
CMD VOL <0-100>
CMD COINS <coingecko-id>,<id>,<id>
CMD CUR <NOK|USD|EUR|GBP|SEK>
```

PC → board, about twice a second. This doubles as a host heartbeat: miss it for
five seconds and the display falls back to its idle state.

```
NP|<status>|<muted>|<vol>|<pos>|<len>|<title>|<artist>
      │        │      │     │     │
      │        │      │     │     └─ length in seconds (0 = unknown)
      │        │      │     └─────── position in seconds
      │        │      └───────────── volume 0-100, -1 = unknown
      │        └──────────────────── 0 or 1
      └───────────────────────────── 0 nothing, 1 playing, 2 paused
```

Prices go in their own frame every five seconds, where the change is in tenths
of a percent and `ok` is 0 if the daemon has never had an answer:

```
CX|<ok>|<count>|<price>|<change>|...   (one pair per selected coin)
CS|<index>|<32 points, 0-63, comma separated>
```

Title and artist are transliterated to ASCII on the PC side, because the
built-in font in Arduino_GFX has no æ, ø or å.

## Hardware

Waveshare [ESP32-C6-Touch-LCD-1.9](https://www.waveshare.com/esp32-c6-touch-lcd-1.9.htm).
The variant without touch works too — the boot log says `touch: NOT FOUND` and
the BOOT button becomes play/pause. Pinout and sources are in
[docs/hardware.md](docs/hardware.md).

## Licence

MIT — see [LICENSE](LICENSE). Redistributed third-party components are listed in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
