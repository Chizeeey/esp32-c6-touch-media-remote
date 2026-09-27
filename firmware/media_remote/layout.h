// Geometry for one frame, derived from how far each panel has slid in.
//
// This lives in a header rather than the sketch because the Arduino build
// injects generated function prototypes above everything defined in the .ino,
// so a struct used in a signature there is not yet declared when they appear.
#pragma once

#include <Arduino.h>

struct Layout {
  int16_t npH;      // height of the track-detail panel, 0 when closed
  int16_t volH;     // height of the volume panel, 0 when closed
  int16_t freeTop;  // first row the transport row may use
  int16_t freeBot;  // last row the transport row may use
  int16_t cy;       // centre of the transport row
  int16_t playR;    // radius of the play button
  int16_t sideR;    // radius of the previous/next buttons
  int16_t prevCX;
  int16_t nextCX;
  float grow;       // 0 = both panels open, 1 = the screen to itself
};
