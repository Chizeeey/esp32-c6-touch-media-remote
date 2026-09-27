# Third-party notices

The project's own code is MIT licensed; see [LICENSE](LICENSE). This file covers
the third-party software that is **redistributed** with it.

Nothing listed here is committed to this repository.
`packaging/windows/build.ps1` downloads it at build time, which is deliberate:
pip places each package's own `.dist-info` alongside it, licence text included,
so the bundle carries these notices without anyone having to maintain them by
hand. A hand-copied `lib/` folder loses exactly that.

## The Windows bundle

Everything below ships inside `dist/esp32-media-remote-windows/`.

| Component | Version | Licence | Full text in the bundle |
|---|---|---|---|
| [CPython](https://www.python.org/) (embeddable) | 3.12.8 | PSF License Agreement | `python/LICENSE.txt` |
| [pyserial](https://github.com/pyserial/pyserial) | 3.5 | BSD-3-Clause | see the note below |
| [pycaw](https://github.com/AndreMiras/pycaw) | 20260927 | MIT | `lib/pycaw-*.dist-info/licenses/LICENSE` |
| [comtypes](https://github.com/enthought/comtypes) | 1.4.17 | MIT | `lib/comtypes-*.dist-info/licenses/LICENSE.txt` |
| [psutil](https://github.com/giampaolo/psutil) | 7.2.2 | BSD-3-Clause | `lib/psutil-*.dist-info/LICENSE` |
| [winsdk](https://github.com/pywinrt/pywinrt) | 1.0.0b10 | MIT | `lib/winsdk-*.dist-info/LICENSE` |

Only pyserial, pycaw and winsdk are imported directly. comtypes and psutil
arrive as pycaw's dependencies, and are listed because they are redistributed
all the same.

The versions are the ones a build resolved and was tested with, not pins —
`requirements.txt` leaves them open. Re-run the build to see what you get.

### Note on pyserial

The pyserial 3.5 wheel does not carry its licence text in `.dist-info`, so
unlike the others it is not reproduced inside the bundle. The terms are
BSD-3-Clause, copyright Chris Liechti \<cliechti@gmx.net\>, and the canonical
text is `LICENSE.txt` in
[the pyserial repository](https://github.com/pyserial/pyserial/blob/master/LICENSE.txt).

## The firmware

The firmware is **not** redistributed here as a binary — this repository holds
source only, and `scripts/build.sh` compiles it against libraries that
`arduino-cli` fetches onto your own machine. Nothing below is bundled.

It starts to matter if you publish a compiled `.bin`, because the result links:

- the [ESP32 Arduino core](https://github.com/espressif/arduino-esp32), which
  carries ESP-IDF and its mix of Apache-2.0 and LGPL-licensed components, and
- [Arduino_GFX](https://github.com/moononournation/Arduino_GFX), the display
  driver.

Take the terms from each project rather than from this table, and ship their
notices alongside any binary release.

## Hardware documentation

The pinout in [docs/hardware.md](docs/hardware.md) was read off Waveshare's
schematic and their Arduino examples for the board. That is documentation rather
than redistributed code, and it is credited in that file.
