// ESP32-C6 Touch Media Remote
//
// A portrait touch remote for whatever is playing on the host PC. The transport
// buttons own the screen; the track details and the volume slider live in two
// panels that stay out of the way until you swipe them in:
//
//   swipe down from the top    -> track details, hides again after 5 s
//   swipe up from the bottom   -> volume slider, hides again after 10 s
//
// The buttons grow into whatever space the panels are not using, and a ring
// around the play button carries the playback position while the detail panel
// is closed, so the common case needs no text at all.
//
//   device -> host   CMD PREV | CMD PLAYPAUSE | CMD NEXT | CMD MUTE | CMD VOL n
//   host   -> device NP|status|muted|vol|pos|len|title|artist
//
// Every frame is composed in an off-screen canvas and pushed in one go; drawing
// straight to the panel briefly showed the old text under the new one.
//
// Board: Waveshare ESP32-C6-Touch-LCD-1.9 (ST7789 170x320 + CST816 touch)

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <Wire.h>

#include "config.h"
#include "cst816.h"
#include "layout.h"
#include "model.h"

// ---------------------------------------------------------------- display --

Arduino_DataBus *bus = new Arduino_HWSPI(PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCK,
                                         PIN_LCD_MOSI);
Arduino_GFX *panel = new Arduino_ST7789(
    bus, PIN_LCD_RST, LCD_ROTATION, LCD_IPS, LCD_PANEL_W, LCD_PANEL_H,
    LCD_COL_OFFSET, 0, LCD_COL_OFFSET, 0);

Arduino_Canvas *canvas = nullptr;
Arduino_GFX *gfx = nullptr;  // canvas when it fits in RAM, else the panel
bool buffered = false;

CST816 touch;
bool touchOk = false;

// ----------------------------------------------------------------- layout --

static const int16_t SCR_W = 170;
static const int16_t SCR_H = 320;
static const int16_t PAD = 13;

static const int16_t NP_H = 118;   // track-detail panel, fully open
static const int16_t VOL_H = 80;   // volume panel, fully open
static const int16_t EDGE = 46;    // top/bottom strip reserved for swipes
static const int16_t SWIPE_MIN = 26;
static const int16_t TAP_SLOP = 10;
static const int16_t DRAG_SLOP = 6;

static const uint32_t NP_LINGER = 5000;
static const uint32_t VOL_LINGER = 10000;
static const uint32_t CX_LINGER = 15000;
static const uint32_t SET_LINGER = 25000;

// Settings button, top left -- mirrors the markets button on the right.
static const int16_t SET_BTN_W = 32, SET_BTN_H = 27;
static const int16_t SET_BTN_X = 8, SET_BTN_Y = 7;

// Settings sheet rows.
static const int16_t SW_Y = 44, SW_H = 24;      // colour swatches
static const int16_t ROW_Y = 94, ROW_H = 26;    // coin rows
static const uint8_t ROWS_VISIBLE = 6;
static const int16_t VIEW_H = ROWS_VISIBLE * ROW_H;
static const int16_t ROW_W = SCR_W - 2 * PAD - 8;  // leaves room for the bar
static const int16_t CLOSE_Y = 258, CLOSE_H = 32;

// Markets button, top right. It sits above the transport band, inside the
// top swipe strip: a tap here opens prices, a swipe down still opens details.
static const int16_t CX_BTN_W = 32, CX_BTN_H = 27;
static const int16_t CX_BTN_X = SCR_W - CX_BTN_W - 8, CX_BTN_Y = 7;

static const int16_t TR_SPLIT_L = 52, TR_SPLIT_R = 118;

static const uint8_t NBTN = 4;
enum { B_PREV = 0, B_PLAY = 1, B_NEXT = 2, B_MUTE = 3 };

// ----------------------------------------------------------------- colours --
// The whole palette is derived at runtime from one accent RGB triple, so the
// settings panel can restyle the UI without a rebuild.
//
// The background stays pure black rather than "almost black": at RGB565's
// darkest values green keeps one more bit than red and blue, so near-black
// renders with a visible green cast.

#define C_BG    RGB565(0, 0, 0)
// The two swipe handles stay white in every theme: they are the only clue that
// the volume and track panels exist, so they should never recede into an accent.
#define C_HINT  RGB565(236, 236, 240)
#define C_UP    RGB565(74, 222, 128)
#define C_UP_D  RGB565(16, 52, 32)
#define C_UP_F  RGB565(11, 34, 21)
#define C_RED   RGB565(248, 113, 113)
#define C_RED_D RGB565(68, 20, 26)
#define C_RED_F RGB565(38, 14, 18)

// Six accents. The names are ASCII-only: the built-in font has no æ, ø or å.
static const Theme THEMES[] = {
    {"Lilla", 167, 139, 250}, {"Cyan", 34, 211, 238},
    {"Rav", 245, 166, 35},    {"Gronn", 52, 211, 153},
    {"Rosa", 244, 114, 182},  {"Bla", 96, 165, 250},
};
static const uint8_t N_THEMES = sizeof(THEMES) / sizeof(THEMES[0]);

uint16_t C_ACCENT, C_ACCENT_L, C_ACCENT_D;
uint16_t C_PANEL, C_CARD, C_RING, C_TEXT, C_DIM;

// scale toward black, then lift by `add` toward white.
static inline uint8_t mixc(uint8_t c, float k, int add) {
  return (uint8_t)constrain((int)(c * k) + add, 0, 255);
}

static uint16_t tint(const Theme &t, float k, int add) {
  return RGB565(mixc(t.r, k, add), mixc(t.g, k, add), mixc(t.b, k, add));
}

static void applyTheme(uint8_t idx) {
  const Theme &t = THEMES[idx % N_THEMES];
  C_ACCENT = RGB565(t.r, t.g, t.b);
  C_ACCENT_L = RGB565(mixc(t.r, 1.0f, (255 - t.r) * 0.45f),
                      mixc(t.g, 1.0f, (255 - t.g) * 0.45f),
                      mixc(t.b, 1.0f, (255 - t.b) * 0.45f));
  C_ACCENT_D = tint(t, 0.31f, 0);
  C_PANEL = tint(t, 0.08f, 0);
  C_CARD = tint(t, 0.13f, 0);
  C_RING = tint(t, 0.35f, 0);
  C_DIM = tint(t, 0.35f, 72);
  C_TEXT = tint(t, 0.12f, 220);
}

// ------------------------------------------------------------- host state --

struct State {
  uint8_t status = 0;  // 0 none, 1 playing, 2 paused
  bool muted = false;
  int vol = -1;
  int pos = 0, len = 0;
  String title = "";
  String artist = "";
} st;

bool linked = false;
uint32_t lastHostMs = 0;

// Panels: `open` is the intent, `a` is the 0..1 slide animation that follows it.
bool npOpen = false, volOpen = false, cxOpen = false, setOpen = false;
float npA = 0.0f, volA = 0.0f, cxA = 0.0f, setA = 0.0f;
uint32_t npHideAt = 0, volHideAt = 0, cxHideAt = 0, setHideAt = 0;

Preferences prefs;
uint8_t themeIdx = 0;
int16_t coinScroll = 0;  // pixels the coin list is scrolled down by
bool coinsDirty = false;  // selection changed; tell the host on the next loop

// Spot prices pushed by the host: the board has no network of its own, and the
// daemon already has one.
static const CoinInfo CATALOG[] = {
    {"BTC", "Bitcoin", "bitcoin"},        {"ETH", "Ethereum", "ethereum"},
    {"ADA", "Cardano", "cardano"},        {"SOL", "Solana", "solana"},
    {"XRP", "XRP", "ripple"},             {"DOGE", "Dogecoin", "dogecoin"},
    {"DOT", "Polkadot", "polkadot"},      {"LINK", "Chainlink", "chainlink"},
    {"AVAX", "Avalanche", "avalanche-2"}, {"MATIC", "Polygon", "matic-network"},
    {"LTC", "Litecoin", "litecoin"},      {"BCH", "Bitcoin Cash", "bitcoin-cash"},
    {"TRX", "Tron", "tron"},              {"ATOM", "Cosmos", "cosmos"},
    {"UNI", "Uniswap", "uniswap"},        {"XLM", "Stellar", "stellar"},
    {"ALGO", "Algorand", "algorand"},     {"NEAR", "Near", "near"},
    {"APT", "Aptos", "aptos"},            {"ARB", "Arbitrum", "arbitrum"},
    {"OP", "Optimism", "optimism"},       {"SHIB", "Shiba Inu", "shiba-inu"},
    {"TON", "Toncoin", "the-open-network"}, {"SUI", "Sui", "sui"},
};
static const uint8_t N_CATALOG = sizeof(CATALOG) / sizeof(CATALOG[0]);
static const uint8_t N_SLOTS = 3;  // the price sheet has room for three cards

uint8_t sel[N_SLOTS] = {2, 0, 1};  // ADA, BTC, ETH
uint8_t selN = N_SLOTS;            // between 1 and N_SLOTS are shown
CoinData cdata[N_SLOTS] = {};
bool cxValid = false;

int8_t pressedBtn = -1;
uint32_t pressFlashUntil[NBTN] = {0, 0, 0, 0};

// One gesture at a time. Once a swipe or a volume drag claims the touch, the
// button under the finger no longer fires on release.
bool gDown = false, gClaimed = false, gVolDrag = false;
int16_t gx0 = 0, gy0 = 0;
int8_t gBtn = -1;
int gStartVol = 0;
uint32_t lastVolSendMs = 0;

uint32_t lastTouchMs = 0;
uint8_t blLevel = BL_ACTIVE;
bool dirty = true;

// -------------------------------------------------------------- utilities --

static int16_t textW(const String &s, uint8_t size) {
  return (int16_t)s.length() * 6 * size;
}

static String mmss(int secs) {
  if (secs < 0) secs = 0;
  char buf[12];
  snprintf(buf, sizeof(buf), "%d:%02d", secs / 60, secs % 60);
  return String(buf);
}

// Greedy word wrap. Text that still overflows the last line is truncated with
// a tilde -- the built-in font has no ellipsis glyph.
static uint8_t wrapText(const String &s, uint8_t size, int16_t maxW,
                        String *out, uint8_t maxLines) {
  const uint8_t perLine = maxW / (6 * size);
  uint8_t n = 0;
  int start = 0;
  while (start < (int)s.length() && n < maxLines) {
    if ((int)s.length() - start <= perLine) {
      out[n++] = s.substring(start);
      return n;
    }
    int brk = -1;
    for (int i = start; i < start + perLine + 1 && i < (int)s.length(); i++) {
      if (s[i] == ' ') brk = i;
    }
    if (brk <= start) brk = start + perLine;  // a single very long word
    out[n++] = s.substring(start, brk);
    start = brk;
    while (start < (int)s.length() && s[start] == ' ') start++;
  }
  if (start < (int)s.length() && n > 0) {
    String &last = out[n - 1];
    if ((int)last.length() > perLine - 1) last = last.substring(0, perLine - 1);
    last += "~";
  }
  return n;
}

static void textAt(const String &s, uint8_t size, int16_t x, int16_t y,
                   uint16_t col) {
  gfx->setTextSize(size);
  gfx->setTextColor(col);
  gfx->setCursor(x, y);
  gfx->print(s);
}

static void textCentered(const String &s, uint8_t size, int16_t y, uint16_t col) {
  textAt(s, size, (SCR_W - textW(s, size)) / 2, y, col);
}

static Layout layout() {
  Layout L;
  L.npH = (int16_t)(npA * NP_H);
  L.volH = (int16_t)(volA * VOL_H);
  L.freeTop = L.npH;
  L.freeBot = SCR_H - L.volH;
  const int16_t freeH = L.freeBot - L.freeTop;
  L.cy = L.freeTop + freeH / 2;

  const float t = constrain((freeH - (SCR_H - NP_H - VOL_H)) /
                                (float)(SCR_H - (SCR_H - NP_H - VOL_H)),
                            0.0f, 1.0f);
  L.grow = t;
  L.playR = 29 + (int16_t)(t * 8);
  L.sideR = 18 + (int16_t)(t * 2);
  L.prevCX = 28 - (int16_t)(t * 4);
  L.nextCX = 142 + (int16_t)(t * 4);
  return L;
}

// ---------------------------------------------------------------- drawing --

static void drawIcon(uint8_t i, int16_t cx, int16_t cy, int16_t r, uint16_t col) {
  const float s = r / 29.0f;  // glyphs were drawn for a 29 px radius
  const int16_t a = (int16_t)(10 * s), b = (int16_t)(13 * s);
  const int16_t w = (int16_t)(3 * s), h = (int16_t)(9 * s);
  switch (i) {
    case B_PREV:
      gfx->fillRect(cx - (int16_t)(11 * s), cy - h, max<int16_t>(w, 2), 2 * h, col);
      gfx->fillTriangle(cx + a, cy - h, cx + a, cy + h, cx - 1, cy, col);
      gfx->fillTriangle(cx + 1, cy - h, cx + 1, cy + h, cx - (int16_t)(7 * s), cy, col);
      break;
    case B_PLAY:
      if (st.status == 1) {
        gfx->fillRoundRect(cx - a, cy - b, (int16_t)(7 * s), 2 * b, 2, col);
        gfx->fillRoundRect(cx + (int16_t)(3 * s), cy - b, (int16_t)(7 * s), 2 * b, 2, col);
      } else {
        // Nudged right so the triangle's mass looks centred in the circle.
        const int16_t t = (int16_t)(14 * s);
        gfx->fillTriangle(cx - (int16_t)(8 * s), cy - t, cx - (int16_t)(8 * s),
                          cy + t, cx + b, cy, col);
      }
      break;
    case B_NEXT:
      gfx->fillRect(cx + (int16_t)(8 * s), cy - h, max<int16_t>(w, 2), 2 * h, col);
      gfx->fillTriangle(cx - a, cy - h, cx - a, cy + h, cx + 1, cy, col);
      gfx->fillTriangle(cx - 1, cy - h, cx - 1, cy + h, cx + (int16_t)(7 * s), cy, col);
      break;
  }
}

// Position ring hugging the inside of the play button's rim. It replaces the
// progress bar whenever the detail panel is closed.
static void drawRing(int16_t cx, int16_t cy, int16_t r, float frac) {
  const int16_t rOut = r - 1, rIn = r - 4;
  for (int deg = 0; deg < 360; deg += 3) {
    const float rad = (deg - 90) * 0.017453f;
    const float c = cosf(rad), s = sinf(rad);
    const bool on = (deg / 360.0f) <= frac;
    gfx->drawLine(cx + (int16_t)(c * rIn), cy + (int16_t)(s * rIn),
                  cx + (int16_t)(c * rOut), cy + (int16_t)(s * rOut),
                  on ? C_ACCENT : C_RING);
  }
}

static void drawTransport(const Layout &L) {
  struct { uint8_t id; int16_t cx, r; } b[3] = {
      {B_PREV, L.prevCX, L.sideR},
      {B_PLAY, 85, L.playR},
      {B_NEXT, L.nextCX, L.sideR},
  };

  for (auto &e : b) {
    const bool down =
        (pressedBtn == (int8_t)e.id) || (millis() < pressFlashUntil[e.id]);
    const bool hero = (e.id == B_PLAY);

    gfx->fillCircle(e.cx, L.cy, e.r, down ? C_ACCENT_D : C_CARD);
    gfx->drawCircle(e.cx, L.cy, e.r, down ? C_ACCENT : C_RING);

    if (hero && npA < 0.5f && st.len > 0) {
      drawRing(e.cx, L.cy, e.r, constrain(st.pos / (float)st.len, 0.0f, 1.0f));
    } else if (hero) {
      gfx->drawCircle(e.cx, L.cy, e.r - 1,
                      st.status == 1 ? C_ACCENT : (down ? C_ACCENT : C_RING));
    }

    uint16_t icon = C_TEXT;
    if (hero) icon = down ? C_ACCENT_L : C_ACCENT;
    drawIcon(e.id, e.cx, L.cy, e.r, icon);
  }
}

static void drawNowPlaying(const Layout &L) {
  if (L.npH <= 0) return;
  const int16_t top = L.npH - NP_H;  // slides down from off-screen

  gfx->fillRect(0, 0, SCR_W, L.npH, C_PANEL);
  gfx->drawFastHLine(0, L.npH - 1, SCR_W, C_RING);

  String label = "NOW PLAYING";
  String title = st.title, artist = st.artist;
  if (!linked) {
    label = "VENTER";
    title = "Kobler til";
    artist = "starting host...";
  } else if (st.status == 0 || title.length() == 0) {
    label = "STILLE";
    title = "Ingenting spiller";
    artist = "Spotify, YouTube, VLC";
  } else if (st.status == 2) {
    label = "PAUSE";
  }

  textAt(label, 1, PAD, top + 12, st.status == 1 ? C_ACCENT : C_DIM);

  // Level meter, right-aligned against the label.
  for (int i = 0; i < 4; i++) {
    int16_t h = 2;
    if (st.status == 1) {
      const float p = millis() / 150.0f + i * 1.9f;
      h = 3 + (int16_t)(5.5f * (1.0f + sinf(p)));
    }
    gfx->fillRect(SCR_W - PAD - 26 + i * 7, top + 21 - h, 4, h,
                  st.status == 1 ? C_ACCENT_L : C_RING);
  }

  // Title over at most two lines -- wrapped, never scrolled.
  const int16_t avail = SCR_W - 2 * PAD;
  String lines[2];
  const uint8_t n = wrapText(title, 2, avail, lines, 2);
  for (uint8_t i = 0; i < n; i++) {
    textCentered(lines[i], 2, top + 32 + i * 18, C_TEXT);
  }

  String one[1];
  wrapText(artist, 1, avail, one, 1);
  textCentered(one[0], 1, top + 72, C_DIM);

  const int16_t barY = top + 88;
  gfx->fillRoundRect(PAD, barY, avail, 4, 2, C_RING);
  if (st.len > 0) {
    int32_t fw = (int32_t)avail * st.pos / st.len;
    fw = constrain(fw, 0, avail);
    if (fw > 4) {
      gfx->fillRoundRect(PAD, barY, fw, 4, 2,
                         st.status == 1 ? C_ACCENT : C_DIM);
    }
  }
  textAt(st.len > 0 ? mmss(st.pos) : String("--:--"), 1, PAD, barY + 10, C_DIM);
  const String total = st.len > 0 ? mmss(st.len) : String("--:--");
  textAt(total, 1, SCR_W - PAD - textW(total, 1), barY + 10, C_DIM);
}

static void drawVolume(const Layout &L) {
  if (L.volH <= 0) return;
  const int16_t top = SCR_H - L.volH;  // slides up from off-screen

  gfx->fillRect(0, top, SCR_W, L.volH, C_PANEL);
  gfx->drawFastHLine(0, top, SCR_W, C_RING);

  const bool down = (pressedBtn == B_MUTE) || (millis() < pressFlashUntil[B_MUTE]);
  const uint16_t fg = st.muted ? C_RED : (down ? C_ACCENT_L : C_ACCENT);

  // Speaker glyph.
  const int16_t sx = PAD + 20, sy = top + 26;
  gfx->fillRect(sx - 10, sy - 4, 5, 8, fg);
  for (int i = 0; i < 9; i++) {
    const int16_t h = 4 + i;
    gfx->drawFastVLine(sx - 5 + i, sy - h, 2 * h, fg);
  }
  if (st.muted) {
    for (int t = 0; t < 2; t++) {
      gfx->drawLine(sx + 7, sy - 6 + t, sx + 16, sy + 3 + t, fg);
      gfx->drawLine(sx + 16, sy - 6 + t, sx + 7, sy + 3 + t, fg);
    }
  } else {
    for (int16_t r = 8; r <= 14; r += 4) {
      for (int16_t dy = -r; dy <= r; dy++) {
        const float dx = sqrtf((float)(r * r - dy * dy));
        if (dx < r * 0.6f) continue;
        gfx->drawPixel(sx + 3 + (int16_t)dx, sy + dy, fg);
      }
    }
  }

  const String txt = st.muted ? String("MUTE")
                              : (st.vol >= 0 ? String(st.vol) + "%" : String("--"));
  textAt(txt, 2, SCR_W - PAD - textW(txt, 2), top + 18, st.muted ? C_RED : C_TEXT);

  // Slider. Thickens and grows a knob while the finger is scrubbing.
  const int16_t tx = PAD, tw = SCR_W - 2 * PAD;
  const int16_t th = gVolDrag ? 6 : 4;
  const int16_t ty = top + 52;
  gfx->fillRoundRect(tx, ty, tw, th, th / 2, C_RING);
  if (!st.muted && st.vol > 0) {
    const int16_t fw = (int32_t)tw * min(st.vol, 100) / 100;
    gfx->fillRoundRect(tx, ty, fw, th, th / 2, C_ACCENT);
    if (gVolDrag) gfx->fillCircle(tx + fw, ty + th / 2, 6, C_ACCENT_L);
  }
}

// Markets button: a tiny ascending bar chart in a rounded square.
static void drawMarketsButton() {
  gfx->fillRoundRect(CX_BTN_X, CX_BTN_Y, CX_BTN_W, CX_BTN_H, 8, C_CARD);
  gfx->drawRoundRect(CX_BTN_X, CX_BTN_Y, CX_BTN_W, CX_BTN_H, 8, C_RING);
  const int16_t bx = CX_BTN_X + 9, by = CX_BTN_Y + CX_BTN_H - 8;
  for (int i = 0; i < 3; i++) {
    const int16_t h = 5 + i * 4;
    gfx->fillRect(bx + i * 5, by - h, 3, h, C_ACCENT);
  }
}

static void drawTrend(int16_t x, int16_t y, int16_t chg, uint16_t col) {
  if (chg >= 0) {
    gfx->fillTriangle(x, y + 6, x + 8, y + 6, x + 4, y, col);
  } else {
    gfx->fillTriangle(x, y, x + 8, y, x + 4, y + 6, col);
  }
}

static void drawSpark(const CoinData &c, int16_t x, int16_t y, int16_t w,
                      int16_t h, uint16_t line, uint16_t fill) {
  if (!c.hasSpark) return;
  const float dx = (float)w / (SPARK_N - 1);
  int16_t px = x, py = y + h - 1 - (int16_t)((float)c.spark[0] / SPARK_MAX * (h - 1));
  for (uint8_t i = 1; i < SPARK_N; i++) {
    const int16_t nx = x + (int16_t)(i * dx);
    const int16_t ny =
        y + h - 1 - (int16_t)((float)c.spark[i] / SPARK_MAX * (h - 1));
    // Shade below the curve first, then stroke it, so the line stays crisp.
    for (int16_t sx = px; sx <= nx; sx++) {
      const float t = (nx == px) ? 0.0f : (float)(sx - px) / (nx - px);
      const int16_t sy = py + (int16_t)((ny - py) * t);
      gfx->drawFastVLine(sx, sy, y + h - sy, fill);
    }
    gfx->drawLine(px, py, nx, ny, line);
    gfx->drawLine(px, py - 1, nx, ny - 1, line);
    px = nx;
    py = ny;
  }
}

static void drawCrypto() {
  if (cxA <= 0.0f) return;
  const int16_t top = (int16_t)(-(1.0f - cxA) * SCR_H);  // slides down as a sheet

  gfx->fillRect(0, top, SCR_W, SCR_H, C_BG);

  textAt("KRYPTO", 1, PAD, top + 12, C_ACCENT);
  const String unit = "NOK  24T";
  textAt(unit, 1, SCR_W - PAD - textW(unit, 1), top + 12, C_DIM);

  // Cards share out whatever height is left, capped so one lonely coin does
  // not become a full-screen slab.
  const int16_t cx = PAD, cw = SCR_W - 2 * PAD;
  const int16_t gap = 6, avail = SCR_H - 32 - 24;
  const int16_t ch = min<int16_t>(110, (avail - (selN - 1) * gap) / selN);
  const int16_t y0 = top + 32 + (avail - (ch * selN + gap * (selN - 1))) / 2;

  for (int i = 0; i < selN; i++) {
    const int16_t y = y0 + i * (ch + gap);
    const CoinInfo &info = CATALOG[sel[i]];
    const CoinData &c = cdata[i];
    const bool up = c.chg >= 0;
    const uint16_t trend = up ? C_UP : C_RED;

    gfx->fillRoundRect(cx, y, cw, ch, 12, C_CARD);
    gfx->drawRoundRect(cx, y, cw, ch, 12, C_RING);

    textAt(info.ticker, 2, cx + 11, y + 10, C_ACCENT);
    textAt(info.name, 1, cx + 11, y + 30, C_DIM);

    if (!cxValid) {
      textAt("--", 2, cx + cw - 11 - textW("--", 2), y + 10, C_DIM);
      continue;
    }

    // Drop to the small face when a long price would run into the ticker.
    const String price(c.price);
    const int16_t room = cw - 22 - textW(info.ticker, 2) - 6;
    const uint8_t psize = textW(price, 2) <= room ? 2 : 1;
    textAt(price, psize, cx + cw - 11 - textW(price, psize),
           y + (psize == 2 ? 10 : 14), C_TEXT);

    // Change as a tinted pill, so the colour reads even at a glance.
    char buf[12];
    snprintf(buf, sizeof(buf), "%d.%d%%", abs(c.chg) / 10, abs(c.chg) % 10);
    const String pct(buf);
    const int16_t pw = textW(pct, 1) + 20, ph = 13;
    const int16_t px2 = cx + cw - 11 - pw, py2 = y + 27;
    gfx->fillRoundRect(px2, py2, pw, ph, 6, up ? C_UP_D : C_RED_D);
    if (up) {
      gfx->fillTriangle(px2 + 6, py2 + 9, px2 + 12, py2 + 9, px2 + 9, py2 + 4, trend);
    } else {
      gfx->fillTriangle(px2 + 6, py2 + 4, px2 + 12, py2 + 4, px2 + 9, py2 + 9, trend);
    }
    textAt(pct, 1, px2 + 16, py2 + 3, trend);

    drawSpark(c, cx + 11, y + 46, cw - 22, ch - 56, trend,
              up ? C_UP_F : C_RED_F);
  }

  textCentered("tap anywhere", 1, top + SCR_H - 16, C_DIM);
}

// Settings button: three mixer sliders.
static void drawSettingsButton() {
  gfx->fillRoundRect(SET_BTN_X, SET_BTN_Y, SET_BTN_W, SET_BTN_H, 8, C_CARD);
  gfx->drawRoundRect(SET_BTN_X, SET_BTN_Y, SET_BTN_W, SET_BTN_H, 8, C_RING);
  const int16_t lx = SET_BTN_X + 8, lw = SET_BTN_W - 16;
  const int16_t knob[3] = {3, 8, 5};  // staggered, so it reads as sliders
  for (int i = 0; i < 3; i++) {
    const int16_t ly = SET_BTN_Y + 8 + i * 6;
    gfx->drawFastHLine(lx, ly, lw, C_RING);
    gfx->fillCircle(lx + knob[i], ly, 2, C_ACCENT);
  }
}

static void drawCheck(int16_t x, int16_t y, bool on) {
  if (on) {
    gfx->fillRoundRect(x, y, 16, 16, 5, C_ACCENT);
    gfx->drawLine(x + 4, y + 8, x + 7, y + 11, C_BG);
    gfx->drawLine(x + 4, y + 9, x + 7, y + 12, C_BG);
    gfx->drawLine(x + 7, y + 11, x + 12, y + 5, C_BG);
    gfx->drawLine(x + 7, y + 12, x + 12, y + 6, C_BG);
  } else {
    gfx->drawRoundRect(x, y, 16, 16, 5, C_RING);
  }
}

static bool isSelected(uint8_t catIdx, uint8_t *slot = nullptr) {
  for (uint8_t i = 0; i < N_SLOTS; i++) {
    if (sel[i] == catIdx) {
      if (slot) *slot = i;
      return true;
    }
  }
  return false;
}

static int16_t maxCoinScroll() {
  const int16_t total = N_CATALOG * ROW_H;
  return total > VIEW_H ? total - VIEW_H : 0;
}

static void drawSettings() {
  if (setA <= 0.0f) return;
  const int16_t top = (int16_t)(-(1.0f - setA) * SCR_H);

  gfx->fillRect(0, top, SCR_W, SCR_H, C_BG);

  // The list is drawn first and then masked top and bottom, so a row scrolled
  // half out of the viewport is clipped instead of bleeding into the chrome.
  for (uint8_t i = 0; i < N_CATALOG; i++) {
    const int16_t y = top + ROW_Y + i * ROW_H - coinScroll;
    if (y + ROW_H < top + ROW_Y - ROW_H || y > top + ROW_Y + VIEW_H + ROW_H) {
      continue;
    }
    const bool on = isSelected(i);
    if (on) gfx->fillRoundRect(PAD, y, ROW_W, ROW_H - 3, 7, C_CARD);
    drawCheck(PAD + 6, y + 4, on);
    textAt(CATALOG[i].ticker, 1, PAD + 30, y + 8, on ? C_ACCENT : C_DIM);
    textAt(CATALOG[i].name, 1, PAD + 66, y + 8, on ? C_TEXT : C_DIM);
  }
  gfx->fillRect(0, top, SCR_W, ROW_Y, C_BG);
  gfx->fillRect(0, top + ROW_Y + VIEW_H, SCR_W, SCR_H - ROW_Y - VIEW_H, C_BG);

  // Scroll bar.
  const int16_t barX = SCR_W - PAD + 2, barY = top + ROW_Y;
  gfx->fillRoundRect(barX, barY, 3, VIEW_H, 1, C_CARD);
  const int16_t total = N_CATALOG * ROW_H;
  const int16_t thumbH = max<int16_t>(20, (int32_t)VIEW_H * VIEW_H / total);
  const int16_t span = maxCoinScroll();
  const int16_t thumbY =
      barY + (span ? (int32_t)(VIEW_H - thumbH) * coinScroll / span : 0);
  gfx->fillRoundRect(barX, thumbY, 3, thumbH, 1, C_RING);

  textAt("INNSTILLINGER", 1, PAD, top + 12, C_ACCENT);

  textAt("FARGE", 1, PAD, top + 32, C_DIM);
  const int16_t sw = 22, gap = 2;
  for (uint8_t i = 0; i < N_THEMES; i++) {
    const int16_t x = PAD + i * (sw + gap);
    const Theme &t = THEMES[i];
    gfx->fillRoundRect(x, top + SW_Y, sw, SW_H, 6, RGB565(t.r, t.g, t.b));
    if (i == themeIdx) {
      gfx->drawRoundRect(x - 2, top + SW_Y - 2, sw + 4, SW_H + 4, 8, C_TEXT);
    }
  }

  char hdr[28];
  snprintf(hdr, sizeof(hdr), "KRYPTO  %d/%d  (%d)", selN, N_SLOTS, N_CATALOG);
  textAt(hdr, 1, PAD, top + 80, C_DIM);

  gfx->fillRoundRect(PAD, top + CLOSE_Y, SCR_W - 2 * PAD, CLOSE_H, 10, C_CARD);
  gfx->drawRoundRect(PAD, top + CLOSE_Y, SCR_W - 2 * PAD, CLOSE_H, 10, C_RING);
  textCentered("LUKK", 1, top + CLOSE_Y + 13, C_ACCENT);
}

// Small handles at the screen edges, so the hidden panels are discoverable.
static void drawHints(const Layout &L) {
  if (L.npH == 0) gfx->fillRoundRect(SCR_W / 2 - 17, 5, 34, 3, 2, C_HINT);
  if (L.volH == 0) gfx->fillRoundRect(SCR_W / 2 - 17, SCR_H - 8, 34, 3, 2, C_HINT);
}

static void render() {
  const Layout L = layout();
  gfx->fillScreen(C_BG);
  drawTransport(L);
  drawNowPlaying(L);
  drawVolume(L);
  drawHints(L);
  if (L.npH == 0 && cxA < 1.0f && setA < 1.0f) {
    drawMarketsButton();
    drawSettingsButton();
  }
  drawCrypto();
  drawSettings();
  if (buffered) canvas->flush();
  dirty = false;
}

// ----------------------------------------------------------------- input --

// Map panel-native coordinates (170 x 320 portrait) onto the rotated screen.
static void mapTouch(uint16_t xn, uint16_t yn, int16_t &sx, int16_t &sy) {
#if LCD_ROTATION == 3
  sx = LCD_PANEL_H - 1 - (int16_t)yn;
  sy = (int16_t)xn;
#elif LCD_ROTATION == 1
  sx = (int16_t)yn;
  sy = LCD_PANEL_W - 1 - (int16_t)xn;
#elif LCD_ROTATION == 2
  sx = LCD_PANEL_W - 1 - (int16_t)xn;
  sy = LCD_PANEL_H - 1 - (int16_t)yn;
#else
  sx = (int16_t)xn;
  sy = (int16_t)yn;
#endif
}

// Transport targets are disjoint bands covering the free area minus the swipe
// strips: padded circles would overlap, and the first one tested silently wins.
static int8_t hitTest(const Layout &L, int16_t sx, int16_t sy) {
  if (L.volH > 0 && sy >= SCR_H - L.volH) return B_MUTE;
  const int16_t top = L.freeTop + (L.npH > 0 ? 0 : EDGE);
  const int16_t bot = L.freeBot - (L.volH > 0 ? 0 : EDGE);
  if (sy >= top && sy <= bot) {
    if (sx < TR_SPLIT_L) return B_PREV;
    if (sx < TR_SPLIT_R) return B_PLAY;
    return B_NEXT;
  }
  return -1;
}

static void sendCommand(uint8_t i) {
  switch (i) {
    case B_PREV: Serial.println("CMD PREV"); break;
    case B_PLAY: Serial.println("CMD PLAYPAUSE"); break;
    case B_NEXT: Serial.println("CMD NEXT"); break;
    case B_MUTE: Serial.println("CMD MUTE"); break;
  }
}

static void showNP() {
  npOpen = true;
  npHideAt = millis() + NP_LINGER;
  dirty = true;
}

static void showVol() {
  volOpen = true;
  volHideAt = millis() + VOL_LINGER;
  dirty = true;
}

static void showCrypto() {
  cxOpen = true;
  cxHideAt = millis() + CX_LINGER;
  dirty = true;
}

static bool inMarketsButton(int16_t sx, int16_t sy) {
  return sx >= CX_BTN_X - 6 && sy >= 0 && sy <= CX_BTN_Y + CX_BTN_H + 6;
}

static bool inSettingsButton(int16_t sx, int16_t sy) {
  return sx <= SET_BTN_X + SET_BTN_W + 6 && sy >= 0 &&
         sy <= SET_BTN_Y + SET_BTN_H + 6;
}

static void showSettings() {
  setOpen = true;
  setHideAt = millis() + SET_LINGER;
  dirty = true;
}

static void saveSettings() {
  prefs.putUChar("theme", themeIdx);
  prefs.putUChar("selN", selN);
  prefs.putBytes("sel", sel, N_SLOTS);
}

// Toggle a coin in or out of the sheet. At least one stays on, and the sheet
// holds at most N_SLOTS -- a tap on a fourth is ignored rather than silently
// evicting something the user picked.
static void toggleCoin(uint8_t catIdx) {
  uint8_t slot;
  if (isSelected(catIdx, &slot)) {
    if (selN <= 1) return;
    for (uint8_t i = slot; i + 1 < selN; i++) sel[i] = sel[i + 1];
    selN--;
  } else {
    if (selN >= N_SLOTS) return;
    sel[selN++] = catIdx;
  }
  memset(cdata, 0, sizeof(cdata));  // stale prices belong to the old picks
  cxValid = false;
  coinsDirty = true;
  saveSettings();
}

static void handleSettingsTap(int16_t x, int16_t y) {
  if (y >= SW_Y - 4 && y <= SW_Y + SW_H + 4) {
    const int16_t sw = 22, gap = 2;
    for (uint8_t i = 0; i < N_THEMES; i++) {
      const int16_t x0 = PAD + i * (sw + gap);
      if (x >= x0 - 1 && x < x0 + sw + 1) {
        themeIdx = i;
        applyTheme(themeIdx);
        saveSettings();
        return;
      }
    }
    return;
  }
  if (y >= ROW_Y && y < ROW_Y + VIEW_H) {
    const int16_t idx = (y - ROW_Y + coinScroll) / ROW_H;
    if (idx >= 0 && idx < N_CATALOG) toggleCoin((uint8_t)idx);
    return;
  }
  if (y >= CLOSE_Y && y <= CLOSE_Y + CLOSE_H) setOpen = false;
}

static void sendCoins() {
  Serial.print("CMD COINS ");
  for (uint8_t i = 0; i < selN; i++) {
    if (i) Serial.print(',');
    Serial.print(CATALOG[sel[i]].id);
  }
  Serial.println();
}

static void handleTouch() {
  const Layout L = layout();
  uint16_t xn, yn;
  const bool down = touchOk && touch.read(xn, yn);
  int16_t sx = 0, sy = 0;
  if (down) {
    mapTouch(xn, yn, sx, sy);
    lastTouchMs = millis();
    // Any contact counts as activity for the linger timers.
    if (npOpen) npHideAt = millis() + NP_LINGER;
    if (volOpen) volHideAt = millis() + VOL_LINGER;
  }

#if TOUCH_DEBUG
  static uint32_t dbg = 0;
  if (down && millis() - dbg > 120) {
    dbg = millis();
    Serial.printf("RAW x=%u y=%u -> %d,%d\n", xn, yn, sx, sy);
  }
#endif

  // The settings sheet has controls, so it acts on release rather than
  // dismissing on any touch the way the read-only sheets do.
  if (setOpen) {
    static int16_t scrollStart = 0;
    if (down) {
      setHideAt = millis() + SET_LINGER;
      if (!gDown) {
        gDown = true;
        gClaimed = false;
        gx0 = sx;
        gy0 = sy;
        scrollStart = coinScroll;
      } else if (gy0 >= ROW_Y && gy0 < ROW_Y + VIEW_H) {
        // Vertical drag inside the list scrolls it; once it does, the release
        // no longer counts as a tap on whatever row ended up under the finger.
        const int16_t dy = sy - gy0;
        if (!gClaimed && abs(dy) > TAP_SLOP) gClaimed = true;
        if (gClaimed) {
          coinScroll = constrain(scrollStart - dy, 0, maxCoinScroll());
          dirty = true;
        }
      }
    } else if (gDown) {
      gDown = false;
      if (!gClaimed) handleSettingsTap(gx0, gy0);
      gClaimed = false;
      dirty = true;
    }
    return;
  }

  // While the price sheet is up it owns the screen: any tap dismisses it.
  if (cxOpen) {
    if (down) cxHideAt = millis() + CX_LINGER;
    if (!down && gDown) {
      cxOpen = false;
      gDown = false;
      dirty = true;
    } else if (down) {
      gDown = true;
    }
    return;
  }

  if (down && !gDown) {  // press
    gDown = true;
    gClaimed = false;
    gVolDrag = false;
    gx0 = sx;
    gy0 = sy;
    gBtn = hitTest(L, sx, sy);
    gStartVol = st.vol >= 0 ? st.vol : 50;
    pressedBtn = gBtn;  // highlight only; the command waits for release
    dirty = true;
    return;
  }

  if (down && gDown) {  // move
    const int16_t dx = sx - gx0, dy = sy - gy0;

    if (!gClaimed) {
      if (!npOpen && gy0 < EDGE && dy > SWIPE_MIN) {
        showNP();
        gClaimed = true;
      } else if (!volOpen && gy0 > SCR_H - EDGE && dy < -SWIPE_MIN) {
        showVol();
        gClaimed = true;
      } else if (npOpen && gy0 < L.npH && dy < -SWIPE_MIN) {
        npOpen = false;
        gClaimed = true;
      } else if (volOpen && gy0 > SCR_H - L.volH && dy > SWIPE_MIN) {
        volOpen = false;
        gClaimed = true;
      } else if (volOpen && gy0 > SCR_H - L.volH && abs(dx) > DRAG_SLOP) {
        gVolDrag = true;
        gClaimed = true;
      } else if (abs(dx) > TAP_SLOP || abs(dy) > TAP_SLOP) {
        gClaimed = true;  // moved too far to still count as a tap
      }
      if (gClaimed) {
        pressedBtn = -1;
        dirty = true;
      }
    }

    if (gVolDrag) {
      const int16_t travel = SCR_W - 2 * PAD;
      int target = gStartVol + (int)((int32_t)dx * 100 / travel);
      target = constrain(target, 0, 100);
      if (target != st.vol && millis() - lastVolSendMs > 60) {
        lastVolSendMs = millis();
        st.vol = target;
        st.muted = false;  // the host unmutes as a side effect
        Serial.print("CMD VOL ");
        Serial.println(target);
        dirty = true;  // optimistic; the next NP frame confirms it
      }
    }
    return;
  }

  if (!down && gDown) {  // release
    if (!gClaimed) {
      const bool inNP = (L.npH > 0 && gy0 < L.npH);
      const bool inVol = (L.volH > 0 && gy0 >= SCR_H - L.volH);

      if (inNP) {
        // Same dismissal as the price sheet: tap the panel to put it away.
        npOpen = false;
      } else if (inVol) {
        // The volume panel cannot just close on a tap -- a tap there means
        // mute. So it does both: toggle, then get out of the way. Dragging
        // sets the level instead and leaves the panel up.
        sendCommand(B_MUTE);
        pressFlashUntil[B_MUTE] = millis() + 150;
        volOpen = false;
      } else if (npA == 0.0f && inMarketsButton(gx0, gy0)) {
        showCrypto();
      } else if (npA == 0.0f && inSettingsButton(gx0, gy0)) {
        showSettings();
      } else if (gBtn >= 0) {
        sendCommand(gBtn);
        pressFlashUntil[gBtn] = millis() + 150;
      } else if (gy0 < EDGE) {
        showNP();  // a tap on the edge strip opens the panel too
      } else if (gy0 > SCR_H - EDGE) {
        showVol();
      }
    }
    gDown = false;
    gVolDrag = false;
    pressedBtn = -1;
    dirty = true;
  }
}

// ------------------------------------------------------------ host serial --

static void applyLine(const String &line) {
  if (!line.startsWith("NP|")) return;

  // NP|status|muted|vol|pos|len|title|artist  (host escapes '|' in the text)
  String f[8];
  uint8_t n = 0;
  int start = 0;
  while (n < 8) {
    int sep = line.indexOf('|', start);
    if (sep < 0 || n == 7) {
      f[n++] = line.substring(start);
      break;
    }
    f[n++] = line.substring(start, sep);
    start = sep + 1;
  }
  if (n < 8) return;

  const String oldTitle = st.title;

  st.status = (uint8_t)f[1].toInt();
  st.muted = f[2].toInt() != 0;
  st.vol = f[3].toInt();
  st.pos = f[4].toInt();
  st.len = f[5].toInt();
  st.title = f[6];
  st.artist = f[7];

  // A new track is worth showing unprompted; it hides itself again.
  if (linked && st.title != oldTitle && st.title.length()) showNP();

  if (!linked) coinsDirty = true;  // a fresh host needs our coin selection
  linked = true;
  lastHostMs = millis();
  dirty = true;
}

static void applyCrypto(const String &line) {
  // CX|ok|count|price|chg|price|chg|...  -- one pair per selected coin, and the
  // price is a display string the host has already formatted.
  String f[3 + N_SLOTS * 2];
  const uint8_t maxF = sizeof(f) / sizeof(f[0]);
  uint8_t n = 0;
  int start = 0;
  while (n < maxF) {
    const int sep = line.indexOf('|', start);
    if (sep < 0) {
      f[n++] = line.substring(start);
      break;
    }
    f[n++] = line.substring(start, sep);
    start = sep + 1;
  }
  if (n < 3) return;

  const uint8_t count = (uint8_t)constrain(f[2].toInt(), 0, (long)N_SLOTS);
  if (n < 3 + count * 2) return;  // host is still on the previous selection

  cxValid = f[1].toInt() != 0;
  for (uint8_t i = 0; i < count; i++) {
    strncpy(cdata[i].price, f[3 + i * 2].c_str(), sizeof(cdata[i].price) - 1);
    cdata[i].price[sizeof(cdata[i].price) - 1] = '\0';
    cdata[i].chg = (int16_t)f[4 + i * 2].toInt();
  }
  if (cxOpen) dirty = true;
}

static void applySpark(const String &line) {
  // CS|index|v,v,v,...  -- one coin's 24 h shape, values 0..SPARK_MAX
  const int p1 = line.indexOf('|', 3);
  if (p1 < 0) return;
  const int idx = line.substring(3, p1).toInt();
  if (idx < 0 || idx > 2) return;

  uint8_t n = 0;
  int start = p1 + 1;
  while (n < SPARK_N && start <= (int)line.length()) {
    int sep = line.indexOf(',', start);
    if (sep < 0) sep = line.length();
    cdata[idx].spark[n++] = (uint8_t)constrain(
        line.substring(start, sep).toInt(), 0, (long)SPARK_MAX);
    start = sep + 1;
  }
  if (n == SPARK_N) {
    cdata[idx].hasSpark = true;
    if (cxOpen) dirty = true;
  }
}

static void pumpSerial() {
  static String buf;
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\n') {
      buf.trim();
      if (buf.startsWith("CX|")) {
        applyCrypto(buf);
      } else if (buf.startsWith("CS|")) {
        applySpark(buf);
      } else if (buf.length()) {
        applyLine(buf);
      }
      buf = "";
    } else if (buf.length() < 320) {
      buf += c;
    }
  }
}

// ------------------------------------------------------------------ setup --

String i2cFound;

static void i2cScan() {
  i2cFound = "";
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      char buf[8];
      snprintf(buf, sizeof(buf), "0x%02X ", a);
      i2cFound += buf;
    }
  }
  if (i2cFound.length() == 0) i2cFound = "(none)";
}

// Resetting this board re-enumerates USB, so anything printed during setup() is
// gone before a monitor can attach. Repeat the banner until the host shows up.
static void printBanner() {
  Serial.println("HELLO media-remote esp32c6");
  Serial.print("  i2c: ");
  Serial.println(i2cFound);
  Serial.print("  touch: ");
  Serial.println(touchOk ? "CST816 ready" : "NOT FOUND (BOOT button = play/pause)");
  Serial.print("  frame buffer: ");
  Serial.println(buffered ? "on" : "OFF (low memory, expect flicker)");
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);

  prefs.begin("remote", false);
  themeIdx = prefs.getUChar("theme", 0) % N_THEMES;
  selN = (uint8_t)constrain(prefs.getUChar("selN", N_SLOTS), 1, (int)N_SLOTS);
  if (prefs.getBytesLength("sel") == N_SLOTS) {
    prefs.getBytes("sel", sel, N_SLOTS);
    for (uint8_t i = 0; i < N_SLOTS; i++) sel[i] %= N_CATALOG;
  }
  applyTheme(themeIdx);

  ledcAttach(PIN_LCD_BL, 5000, 8);
  ledcWrite(PIN_LCD_BL, 0);  // stay dark until the first frame is ready

  panel->begin(40000000);
  panel->fillScreen(C_BG);

  // 170 * 320 * 2 = 108.8 kB. Fall back to unbuffered drawing rather than
  // leaving the screen blank if it will not fit.
  canvas = new Arduino_Canvas(SCR_W, SCR_H, panel);
  buffered = canvas->begin(GFX_SKIP_OUTPUT_BEGIN);
  gfx = buffered ? (Arduino_GFX *)canvas : panel;

  // Off, always: the library's auto-wrap silently spills a long title over
  // whatever is drawn below it. All wrapping here is explicit.
  gfx->setTextWrap(false);

  Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL, TOUCH_I2C_HZ);
  i2cScan();
  touchOk = touch.begin(Wire, TOUCH_ADDR);

  render();

  for (int b = 0; b <= BL_ACTIVE; b += 3) {
    ledcWrite(PIN_LCD_BL, b);
    delay(4);
  }
  blLevel = BL_ACTIVE;
  lastTouchMs = millis();

  printBanner();
}

void loop() {
  pumpSerial();
  handleTouch();

  if (coinsDirty && linked) {
    coinsDirty = false;
    sendCoins();
  }

  static uint32_t lastBanner = 0;
  if (!linked && millis() - lastBanner > 2000) {
    lastBanner = millis();
    printBanner();
  }

  // BOOT button is a fallback play/pause when no touch panel is fitted.
  static bool bootPrev = true;
  const bool bootNow = digitalRead(PIN_BOOT_BTN);
  if (bootPrev && !bootNow) {
    sendCommand(B_PLAY);
    pressFlashUntil[B_PLAY] = millis() + 150;
    lastTouchMs = millis();
    dirty = true;
  }
  bootPrev = bootNow;

  // Drop back to the idle state if the host daemon goes away.
  if (linked && millis() - lastHostMs > 5000) {
    linked = false;
    st.status = 0;
    st.vol = -1;
    st.len = 0;
    st.title = "";
    st.artist = "";
    dirty = true;
  }

  if (npOpen && millis() > npHideAt) {
    npOpen = false;
    dirty = true;
  }
  if (volOpen && millis() > volHideAt) {
    volOpen = false;
    dirty = true;
  }
  if (cxOpen && millis() > cxHideAt) {
    cxOpen = false;
    dirty = true;
  }
  if (setOpen && millis() > setHideAt) {
    setOpen = false;
    dirty = true;
  }

  // Ease each panel toward its target; the buttons resize along with it.
  static uint32_t lastAnim = 0;
  if (millis() - lastAnim > 16) {
    lastAnim = millis();
    float *anims[4] = {&npA, &volA, &cxA, &setA};
    const float targets[4] = {npOpen ? 1.0f : 0.0f, volOpen ? 1.0f : 0.0f,
                              cxOpen ? 1.0f : 0.0f, setOpen ? 1.0f : 0.0f};
    for (int i = 0; i < 4; i++) {
      float &a = *anims[i];
      const float target = targets[i];
      if (fabsf(a - target) > 0.005f) {
        a += (target - a) * 0.28f;   // ease out
        if (fabsf(a - target) <= 0.01f) a = target;
        dirty = true;
      }
    }
  }

  // The level meter and the position ring both want a steady repaint.
  static uint32_t lastTick = 0;
  if (st.status == 1 && millis() - lastTick > 120) {
    lastTick = millis();
    dirty = true;
  }

  for (uint8_t i = 0; i < NBTN; i++) {
    if (pressFlashUntil[i] && millis() > pressFlashUntil[i]) {
      pressFlashUntil[i] = 0;
      dirty = true;
    }
  }

  // Dim when untouched; the panel's blacks look far deeper turned down.
  const uint8_t want = (millis() - lastTouchMs > BL_IDLE_MS) ? BL_IDLE : BL_ACTIVE;
  if (want != blLevel) {
    const int8_t step = want > blLevel ? 5 : -2;  // snap up, ease down
    int next = blLevel + step;
    if ((step > 0 && next > want) || (step < 0 && next < want)) next = want;
    blLevel = (uint8_t)next;
    ledcWrite(PIN_LCD_BL, blLevel);
  }

  if (dirty) render();
  delay(6);
}
