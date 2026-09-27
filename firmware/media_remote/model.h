// Data types shared by the sketch's drawing and parsing code.
//
// These live in a header for the same reason as Layout: the Arduino build
// injects generated function prototypes above everything defined in the .ino,
// so a struct used in a signature there is not yet declared when they appear.
#pragma once

#include <Arduino.h>

// ---- theme ----

struct Theme {
  const char *name;
  uint8_t r, g, b;
};

// ---- crypto ----

static const uint8_t SPARK_N = 32;   // samples in a 24 h sparkline
static const uint8_t SPARK_MAX = 63; // each sample is scaled to 0..SPARK_MAX

// One entry in the catalogue the settings panel picks from. `id` is the
// CoinGecko identifier the host fetches with, so the wire only carries ids.
struct CoinInfo {
  const char *ticker;
  const char *name;
  const char *id;
};

// What the host most recently reported for one selected slot.
//
// The price arrives pre-formatted. The catalogue runs from Bitcoin at ~800 000
// kr to Shiba Inu at ~0.0001 kr, and no single fixed-point integer carries both
// ends, so the host -- which holds the float -- places the decimal point. A
// plain char array rather than String keeps the struct safe to memset.
struct CoinData {
  char price[16];
  int16_t chg;  // tenths of a percent over 24 h
  uint8_t spark[SPARK_N];
  bool hasSpark;
};
