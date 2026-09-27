# Hardware — Waveshare ESP32-C6-Touch-LCD-1.9

Sources: [ESP32-C6-Touch-LCD-1.9-Schematic.pdf](https://files.waveshare.com/wiki/ESP32-C6-LCD-1.9/ESP32-C6-Touch-LCD-1.9-Schematic.pdf)
and Waveshare's own Arduino examples in
[waveshareteam/ESP32-C6-LCD-1.9](https://github.com/waveshareteam/ESP32-C6-LCD-1.9).

## Pinout

| Function | GPIO |
|---|---|
| LCD MOSI (DIN) | 4 |
| LCD SCK (CLK) | 5 |
| LCD DC | 6 |
| LCD CS | 7 |
| LCD RST | 14 |
| LCD backlight | 15 |
| Touch / IMU I2C SDA | 18 |
| Touch / IMU I2C SCL | 8 |
| BOOT button | 9 |

The touch controller (CST816) and the IMU (QMI8658) share one I2C bus.
`TP_RESET` and `TP_INT` are not populated on this board — the resistors are
marked NC in the schematic — so the driver polls instead of using interrupts.

## Display

ST7789, 170×320 pixels, IPS. The glass sits in the middle of the controller's
240×320 frame buffer, which is where the 35-pixel column offset in `config.h`
comes from. The firmware uses rotation 0: 170×320 portrait, USB-C connector at
the top.

## I2C addresses

| Device | Address |
|---|---|
| CST816 touch | 0x15 |
| QMI8658 IMU | 0x6B |

The firmware scans the bus at boot and writes what it finds to the serial port,
so `scripts/monitor.sh` shows what is actually on your board.

## Two variants

Waveshare sells both `ESP32-C6-LCD-1.9` (no touch) and
`ESP32-C6-Touch-LCD-1.9` (with a CST816). The display and the LCD pins are the
same on both. On the variant without touch, the boot log says
`touch: NOT FOUND` and the BOOT button acts as play/pause instead.
