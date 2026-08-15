#include "SettingsStore.h"
#include "Touchscreen.h"
#include <TFT_eSPI.h>

extern TFT_eSPI tft;

#if defined(BOARD_TAB5)
// -----------------------------------------------------------------------------
// M5Stack Tab5 touch backend: the ST7123 capacitive panel is initialized by
// M5GFX autodetect and read through tft.tab5GetTouch(), which returns calibrated
// screen-pixel coordinates (no XPT2046 SPI bus, no map()/calibration) and applies
// the software sensitivity filter (settle-debounce + optional area rejection).
// The XPT2046 `ts` / `touchscreenSPI` globals are defined only to satisfy the
// declarations in Touchscreen.h; they are never used on this board.
// -----------------------------------------------------------------------------
SPIClass touchscreenSPI;
XPT2046_Touchscreen ts(0, 255);
bool feature_active = false;

static bool tab5ReadTouch(int& x, int& y) {
  // tab5GetTouch reads the ST7123 panel and maps to logical canvas coordinates.
  return tft.tab5GetTouch(x, y);
}

void setupTouchscreen() { /* handled by M5GFX autodetect in tft.init() */ }

// isTouchDown* and readTouchRawXY report the raw (level) contact state — used by
// the hold-to-confirm feedback loop and release-waits, which must see the finger
// stay down for the whole press.
bool isTouchDown(uint16_t /*zThresh*/) {
  int x = 0, y = 0;
  return tab5ReadTouch(x, y);
}

bool isTouchDownDismiss(uint16_t zThresh) { return isTouchDown(zThresh); }

bool readTouchRawXY(int16_t& x, int16_t& y, uint16_t /*zThresh*/) {
  int sx = 0, sy = 0;
  if (!tab5ReadTouch(sx, sy)) return false;
  x = (int16_t)sx;
  y = (int16_t)sy;
  return true;
}

// ---- edge-triggered tap-select (fixes touch carry-over across screens) --------
// The UI's ~73 `if (readTouchXY(x,y)) { act }` sites are tap-to-select. With a
// resistive panel the user naturally lifts between taps; the ST7123 is sensitive
// enough that a select-and-hold contact is still down when the next screen draws,
// so that screen read the same contact as a fresh tap ("selected a menu item and
// it also selected something in the next menu"). Fix: report a hit only on the
// RISING edge of a press (first contact after a release). A single uninterrupted
// contact thus yields exactly one hit — consumed by whichever screen is active at
// the rising edge; later screens see nothing until the finger lifts and taps
// again. `s_selPrevDown` is static so it persists across screen transitions (the
// whole point). Level-based consumers above are unaffected.
static bool s_selPrevDown = false;

static bool tab5ReadTouchEdge(int& x, int& y) {
  int sx = 0, sy = 0;
  const bool down = tab5ReadTouch(sx, sy);
  const bool rising = down && !s_selPrevDown;
  s_selPrevDown = down;
  if (!rising) return false;
  x = sx;
  y = sy;
  return true;
}

bool readTouchXY(int& x, int& y) { return tab5ReadTouchEdge(x, y); }
bool readTouchXYDismiss(int& x, int& y) { return tab5ReadTouchEdge(x, y); }

#else  // ---- original XPT2046 backend (classic ESP32-DIV / CYD) ----

#if defined(BOARD_CYD) || defined(BOARD_ESP32_DIV_V1)
// Dedicated VSPI bus for XPT2046 — must not share HSPI with TFT_eSPI on classic ESP32.
SPIClass touchscreenSPI = SPIClass(VSPI);
#else
SPIClass touchscreenSPI = SPIClass(HSPI);
#endif

#ifndef XPT2046_IRQ
#define XPT2046_IRQ 255
#endif

XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
bool feature_active = false;

static bool s_touchInitialized = false;

#ifndef TOUCH_ROTATION
#if defined(BOARD_ESP32_DIV_V2)
#define TOUCH_ROTATION 0
#else
#define TOUCH_ROTATION TFT_ROTATION
#endif
#endif

#if TOUCH_SHARES_TFT_SPI
static void applyTouchRotation(int16_t rawX, int16_t rawY, int16_t& x, int16_t& y) {
  switch (TOUCH_ROTATION) {
    case 0: x = 4095 - rawY; y = rawX; break;
    case 1: x = rawX; y = rawY; break;
    case 2: x = rawY; y = 4095 - rawX; break;
    default: x = 4095 - rawX; y = 4095 - rawY; break;
  }
}

static bool readSharedTouchSample(int16_t& x, int16_t& y, int16_t& z, uint16_t zThresh) {
  if (!s_touchInitialized) {
    return false;
  }

  tft.endWrite();
  z = (int16_t)tft.getTouchRawZ();
  if (z < (int16_t)zThresh) {
    x = 0;
    y = 0;
    return false;
  }

  uint16_t rawX = 0;
  uint16_t rawY = 0;
  tft.getTouchRaw(&rawX, &rawY);
  applyTouchRotation((int16_t)rawX, (int16_t)rawY, x, y);
  return true;
}
#endif

static void ensureTouchSpiReady() {
#if !TOUCH_SHARES_TFT_SPI
  touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
#endif
}

static bool touchSampleOk(uint16_t zThresh, int16_t& rawX, int16_t& rawY) {
#if TOUCH_SHARES_TFT_SPI
  int16_t z = 0;
  return readSharedTouchSample(rawX, rawY, z, zThresh);
#else
  ensureTouchSpiReady();
#if defined(XPT2046_IRQ) && (XPT2046_IRQ < 255)
  if (!ts.tirqTouched()) {
    return false;
  }
#endif
  if (!ts.touched()) {
    return false;
  }
  TS_Point p = ts.getPoint();
  if (p.z < (int16_t)zThresh) {
    return false;
  }
  rawX = p.x;
  rawY = p.y;
  return true;
#endif
}

void setupTouchscreen() {
  if (s_touchInitialized) {
    return;
  }

#if TOUCH_SHARES_TFT_SPI
  pinMode(XPT2046_CS, OUTPUT);
  digitalWrite(XPT2046_CS, HIGH);
#else
  ensureTouchSpiReady();
  ts.begin(touchscreenSPI);
  ts.setRotation(TOUCH_ROTATION);
#endif

  s_touchInitialized = true;
}

extern XPT2046_Touchscreen ts;

bool isTouchDown(uint16_t zThresh) {
  int16_t x = 0;
  int16_t y = 0;
  return touchSampleOk(zThresh, x, y);
}

bool isTouchDownDismiss(uint16_t zThresh) {
  return isTouchDown(zThresh);
}

bool readTouchRawXY(int16_t& x, int16_t& y, uint16_t zThresh) {
  return touchSampleOk(zThresh, x, y);
}

static void mapTouchToScreen(int16_t rawX, int16_t rawY, int& x, int& y) {
  auto& s = settings();
#if defined(BOARD_CYD)
  // Same axis order as the RNT CYD touch test (no inverted Y).
  x = ::map(rawX, s.touchXMin, s.touchXMax, 0, TFT_WIDTH - 1);
  y = ::map(rawY, s.touchYMin, s.touchYMax, 0, TFT_HEIGHT - 1);
#else
  x = ::map(rawX, s.touchXMin, s.touchXMax, 0, TFT_WIDTH - 1);
  y = ::map(rawY, s.touchYMax, s.touchYMin, 0, TFT_HEIGHT - 1);
#endif
}

bool readTouchXY(int& x, int& y) {
  int16_t rawX = 0;
  int16_t rawY = 0;
  if (!touchSampleOk(200, rawX, rawY)) {
    return false;
  }
  mapTouchToScreen(rawX, rawY, x, y);
  return true;
}

bool readTouchXYDismiss(int& x, int& y) {
  int16_t rawX = 0;
  int16_t rawY = 0;
  if (!touchSampleOk(120, rawX, rawY)) {
    return false;
  }
  mapTouchToScreen(rawX, rawY, x, y);
  return true;
}

#endif  // BOARD_TAB5
