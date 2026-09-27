// Board: Waveshare ESP32-C6-Touch-LCD-1.9
// Pinout verified against ESP32-C6-Touch-LCD-1.9-Schematic.pdf and Waveshare's
// own Arduino examples (02_Example/Arduino/07_Hello_World_GFX).
#pragma once

// ---- LCD (ST7789, 170x320, SPI) ----
#define PIN_LCD_MOSI 4
#define PIN_LCD_SCK  5
#define PIN_LCD_DC   6
#define PIN_LCD_CS   7
#define PIN_LCD_RST  14
#define PIN_LCD_BL   15

#define LCD_PANEL_W  170
#define LCD_PANEL_H  320
// The 170x320 glass sits in the middle of the ST7789's 240x320 memory.
#define LCD_COL_OFFSET 35

// This panel is IPS, so it needs the ST7789's inversion turned on -- without
// it every colour comes out negative and a near-black UI renders near-white.
// Waveshare's own example passes 0 here; that is wrong for this board.
#define LCD_IPS 1

// 0 or 2 = portrait (170x320). Flip between them to move the USB-C port
// between the top and the bottom of the image.
#define LCD_ROTATION 2

// ---- Touch (CST816, I2C) ----
#define PIN_TOUCH_SDA 18
#define PIN_TOUCH_SCL 8
#define TOUCH_ADDR    0x15
#define TOUCH_I2C_HZ  300000

// Set to 1 to stream raw touch coordinates over serial for calibration.
#define TOUCH_DEBUG 0

// ---- Misc ----
#define PIN_BOOT_BTN 9  // fallback control if the touch panel is absent
#define SERIAL_BAUD  115200

// ---- Backlight ----
// An LCD's black is only as dark as its backlight leak, so the panel reads far
// blacker at a moderate level than at full blast. It dims further when idle and
// comes straight back up on the first touch.
#define BL_ACTIVE  165
#define BL_IDLE    45
#define BL_IDLE_MS 30000
