// -----------------------------------------------------------------------------
// TFT_eSPI -> M5GFX compatibility shim for the M5Stack Tab5 (ESP32-P4) port.
//
// ESP32-DIV is written against the TFT_eSPI API (~2800 `tft.*` calls) for a
// 240x320 portrait TFT. The Tab5 has a 720x1280 MIPI-DSI panel driven by M5GFX.
// This header is placed FIRST on the Tab5 include path so every existing
// `#include <TFT_eSPI.h>` resolves here, mapping the whole drawing API onto M5GFX.
//
// UI SCALING (TAB5_SCALED_UI, default on): ESP32-DIV's layout is hard-coded for
// 240x320. Rather than retarget hundreds of coordinates, we render the whole UI
// into an offscreen 240x320 canvas (a LovyanGFX sprite in PSRAM) and blit it
// integer-scaled and centered onto the 720x1280 panel (720/240 = 3x -> a crisp
// 720x960 image, letterboxed top/bottom). Touch is read from the panel and
// mapped back to canvas coordinates. Set -DTAB5_SCALED_UI=0 to draw directly to
// the panel at native coordinates (the UI then sits in the top-left corner) --
// a fallback if the scaled path misbehaves on hardware.
//
// LovyanGFX (M5GFX's base) mirrors the TFT_eSPI API: fillRect, drawString,
// setTextDatum, drawCentreString, drawNumber, drawBitmap, fillRoundRect,
// textWidth, the TFT_* colours, setTextFont(int), etc. all exist. We add only
// the handful of TFT_eSPI-specific spellings the codebase uses.
// -----------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <M5GFX.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#ifndef TAB5_SCALED_UI
#define TAB5_SCALED_UI 1
#endif

// Logical canvas the ESP32-DIV UI is authored for.
#ifndef TAB5_UI_W
#define TAB5_UI_W 240
#endif
#ifndef TAB5_UI_H
// 240x427 is ~9:16, matching the 720x1280 panel, so a uniform 3x scale fills the
// whole panel (no letterbox bars) instead of 240x320's 3:4 (which left 160px black
// bars top+bottom). MUST stay in sync with TFT_HEIGHT in shared.h — tft.height()
// returns THIS (the sprite height), and app layout reflows off tft.height().
#define TAB5_UI_H 427
#endif

// TFT_eSPI panel command constants used by the terminal hardware-scroll helpers.
// The DSI panel does not hardware-scroll, so these only need to exist to compile.
#ifndef ILI9341_VSCRDEF
#define ILI9341_VSCRDEF 0x33
#endif
#ifndef ILI9341_VSCRSADD
#define ILI9341_VSCRSADD 0x37
#endif

// LovyanGFX sprite exposed under the TFT_eSPI name (existing code constructs
// `TFT_eSprite gScanPanel(&tft);`). All sprite methods come from LGFX_Sprite.
class TFT_eSprite : public lgfx::LGFX_Sprite {
 public:
  TFT_eSprite(LovyanGFX* parent = nullptr) : lgfx::LGFX_Sprite(parent) {}
};

// ---- software touch sensitivity (board-agnostic) ----
// This Tab5 uses a Touch_ST7123 controller (not a GT911), whose sensitivity is
// not runtime-configurable here — and it reports contact even for a finger
// hovering just above the glass. Fortunately the ST7123 driver fills
// touch_point_t.size with the reported contact AREA (0..255): a hover / feather
// touch has a small area, a deliberate press a larger one. So "sensitivity" is a
// software filter in tab5GetTouch: reject any contact whose area is below
// kMinArea[level], and additionally require it to persist for kSettleMs[level].
// level 0 = accept anything (most sensitive) .. 4 = firm, held press only.
//
// Thresholds set from on-device ST7123 measurement (peak contact area per tap):
// light graze/hover = 7-9, normal tap = 12-13, firm press = 17-23. So a threshold
// in the 10-12 gap rejects grazes while keeping normal taps; 16 requires a firm
// press. level 0 keeps the "accept anything" escape hatch. tab5GetTouch reports a
// contact only while a >=threshold sample was seen within the last kTouchReleaseMs
// (see below), so a hover/graze that never reaches the threshold is rejected.
static inline uint16_t tab5TouchMinArea(uint8_t level) {
  static const uint16_t kMinArea[5] = {0, 10, 11, 12, 16};
  return kMinArea[level > 4 ? 4 : level];
}
// After the area qualifies, the contact must persist this long before registering
// — debounces micro-bounces. Small, since the area threshold now does the heavy
// lifting against grazes/phantoms; kept snappy at the higher-sensitivity levels.
static inline uint16_t tab5TouchSettleMs(uint8_t level) {
  static const uint16_t kSettleMs[5] = {0, 15, 25, 40, 60};
  return kSettleMs[level > 4 ? 4 : level];
}
// A qualified contact is treated as released once no >=threshold sample has been
// seen for this long. Bridges momentary area dips mid-press (a held press never
// flickers) yet still ends the touch when the finger lifts to a hover — which this
// panel keeps reporting as a low-area contact (raw stays true), so we cannot rely
// on raw==false alone to detect release.
static constexpr uint16_t kTouchReleaseMs = 80;

#if TAB5_SCALED_UI
// ------------------------- scaled (offscreen canvas) -------------------------
class TFT_eSPI : public lgfx::LGFX_Sprite {
  M5GFX _panel;
  float _scale = 1.0f;
  float _offx = 0.0f, _offy = 0.0f;
  SemaphoreHandle_t _panelMux = nullptr;   // serializes _panel access (flush task vs touch reads)
  uint8_t  _touchLevel = 3;                 // sensitivity 0..4 (see tab5TouchMinArea)
  uint32_t _downSince  = 0;                 // millis() when the contact became active
  uint32_t _lastStrong = 0;                 // millis() of the last sample with area >= threshold
  bool     _active     = false;             // currently reporting a qualified press
#ifdef TAB5_TOUCH_DEBUG
  uint16_t _dbgPeak = 0;                    // peak contact area of the in-progress tap
  uint16_t _dbgLast = 0;                    // peak area of the last completed tap (on-screen readout)
#endif

  // Blit the offscreen UI canvas to the panel on a dedicated task (core 0) so
  // the app task's touch polling (core 1) is never stalled by the ~700K-pixel
  // scale-blit. Tying the flush to touch reads made quick taps need a long
  // press, and left blocking/animated sections (boot logo, loading spinners,
  // BLE bring-up) invisible until the next poll. The task fixes both.
  static void flushTaskThunk(void* arg) {
    TFT_eSPI* self = static_cast<TFT_eSPI*>(arg);
    for (;;) {
      self->flush();
      vTaskDelay(pdMS_TO_TICKS(33));   // ~30 fps
    }
  }

 public:
  TFT_eSPI() : lgfx::LGFX_Sprite() {}

  void init() {
    _panelMux = xSemaphoreCreateMutex();
    _panel.init();
    _panel.setRotation(0);            // 720x1280 portrait
    _panel.setBrightness(200);
    _panel.startWrite();
    _panel.fillScreen(0);
    _panel.endWrite();

    setPsram(true);
    setColorDepth(16);
    createSprite(TAB5_UI_W, TAB5_UI_H);
    fillScreen(0);

    const float sx = (float)_panel.width()  / (float)TAB5_UI_W;   // 720/240 = 3.0
    const float sy = (float)_panel.height() / (float)TAB5_UI_H;   // 1280/427 = 2.998
    // "cover" (max), not "contain" (min): with the 9:16 canvas this is the clean
    // integer 3.0x on both axes and fills the panel; the ~1px of vertical overflow
    // is centered off the top/bottom edges (invisible) instead of leaving bars.
    _scale = sx > sy ? sx : sy;
    _offx = (_panel.width()  - TAB5_UI_W * _scale) * 0.5f;        // 0
    _offy = (_panel.height() - TAB5_UI_H * _scale) * 0.5f;        // ~-0.5 (overflow, not a bar)
    flush();
    xTaskCreatePinnedToCore(flushTaskThunk, "tab5flush", 4096, this, 1, nullptr, 0);
  }

  // Blit the canvas to the panel, scaled and centered (serialized vs touch reads).
  // (pushRotateZoom = nearest-neighbor; the WithAA variant mis-placed the image
  // to the left half on this panel, so we use the plain integer upscale.)
  void flush() {
    if (_panelMux) xSemaphoreTake(_panelMux, portMAX_DELAY);
    pushRotateZoom(&_panel, _panel.width() * 0.5f, _panel.height() * 0.5f,
                   0.0f, _scale, _scale);
#ifdef TAB5_TOUCH_DEBUG
    // On-screen area readout (redrawn every frame on top of the blitted UI). Shows
    // the peak contact area of the last tap so it can be measured without serial.
    _panel.fillRect(_panel.width() - 250, 0, 250, 48, 0x001F);   // blue box
    _panel.setTextColor(0xFFFF, 0x001F);                          // white on blue
    _panel.setTextSize(3);
    _panel.setCursor(_panel.width() - 242, 10);
    _panel.printf("area=%u", (unsigned)_dbgLast);
    _panel.setTextSize(1);
#endif
    if (_panelMux) xSemaphoreGive(_panelMux);
  }

  // Read panel touch and map to canvas coordinates. Fast (I2C only) — the flush
  // runs on its own task, so touch is polled at the app loop's full rate.
  bool tab5GetTouch(int& x, int& y) {
    lgfx::touch_point_t tp;
    bool raw;
    if (_panelMux) xSemaphoreTake(_panelMux, portMAX_DELAY);
    raw = _panel.getTouch(&tp, 1) > 0;
    if (_panelMux) xSemaphoreGive(_panelMux);
#ifdef TAB5_TOUCH_DEBUG
    // Measurement build: track the PEAK contact area of each tap and latch it on
    // release, so flush() can show it on-screen (serial can't be used to capture
    // this — opening the USB port resets the P4 and the ST7123 then stops
    // reporting touches). Filter is bypassed so touch stays fully usable.
    if (raw) { if (tp.size > _dbgPeak) _dbgPeak = tp.size; }
    else if (_dbgPeak) { _dbgLast = _dbgPeak; _dbgPeak = 0; }
    if (!raw) return false;
#else
    if (!raw) { _active = false; _lastStrong = 0; return false; }   // finger lifted clear
    const uint32_t now = millis();
    const uint16_t minA = tab5TouchMinArea(_touchLevel);
    if (tp.size >= minA) _lastStrong = now;
    // Reject grazes/hovers: report only while a >=threshold sample was seen within
    // the grace window. A pure hover never qualifies; easing to a hover after a
    // press releases after the grace (this panel keeps reporting a hovering finger).
    if (_lastStrong == 0 || (uint32_t)(now - _lastStrong) > kTouchReleaseMs) { _active = false; return false; }
    // Brief persistence after qualifying (debounce micro-bounces).
    if (!_active) { _active = true; _downSince = now; }
    if ((uint32_t)(now - _downSince) < tab5TouchSettleMs(_touchLevel)) return false;
#endif
    x = (int)((tp.x - _offx) / _scale);
    y = (int)((tp.y - _offy) / _scale);
    return true;
  }

  void setBrightness(uint8_t b) {
    if (_panelMux) xSemaphoreTake(_panelMux, portMAX_DELAY);
    _panel.setBrightness(b);
    if (_panelMux) xSemaphoreGive(_panelMux);
  }

  void setTouchSensitivity(uint8_t level) {
    _touchLevel = level > 4 ? 4 : level;   // software area+settle filter in tab5GetTouch
    _active = false;                       // restart any in-progress press under new threshold
    _lastStrong = 0;
  }
  void setRotation(uint8_t) {}     // logical canvas orientation is fixed
  inline void writecommand(uint8_t) {}
  inline void writedata(uint8_t) {}
  inline uint16_t getTouchRawZ() { int x, y; return tab5GetTouch(x, y) ? 4095 : 0; }

  // TFT_eSPI font-number trailing-arg overloads (see notes in the else-branch).
  using lgfx::LGFX_Sprite::drawString;
  using lgfx::LGFX_Sprite::drawCentreString;
  using lgfx::LGFX_Sprite::drawNumber;
  using lgfx::LGFX_Sprite::textWidth;
  using lgfx::LGFX_Sprite::drawChar;

  inline int32_t drawString(const char* s, int32_t x, int32_t y, uint8_t font) {
    setTextFont(font); return lgfx::LGFX_Sprite::drawString(s, x, y);
  }
  inline int32_t drawString(const String& s, int32_t x, int32_t y, uint8_t font) {
    setTextFont(font); return lgfx::LGFX_Sprite::drawString(s.c_str(), x, y);
  }
  inline int32_t drawCentreString(const char* s, int32_t x, int32_t y, uint8_t font) {
    setTextFont(font);
    auto prev = getTextDatum();
    setTextDatum(lgfx::v1::textdatum_t::top_center);
    int32_t w = lgfx::LGFX_Sprite::drawString(s, x, y);
    setTextDatum(prev);
    return w;
  }
  inline int32_t drawCentreString(const String& s, int32_t x, int32_t y, uint8_t font) {
    return drawCentreString(s.c_str(), x, y, font);
  }
  inline int32_t drawNumber(long n, int32_t x, int32_t y, uint8_t font) {
    setTextFont(font); return lgfx::LGFX_Sprite::drawNumber(n, x, y);
  }
  inline int32_t textWidth(const char* s, uint8_t font) {
    setTextFont(font); return lgfx::LGFX_Sprite::textWidth(s);
  }
  inline int32_t textWidth(const String& s, uint8_t font) {
    setTextFont(font); return lgfx::LGFX_Sprite::textWidth(s.c_str());
  }
  inline int32_t drawChar(uint16_t c, int32_t x, int32_t y, uint8_t font) {
    setTextFont(font); return lgfx::LGFX_Sprite::drawChar(c, x, y);
  }
};

#else
// ------------------------- native (direct panel) fallback --------------------
class TFT_eSPI : public M5GFX {
  uint8_t  _touchLevel = 3;
  uint32_t _downSince  = 0;
  uint32_t _lastStrong = 0;
  bool     _active     = false;
 public:
  TFT_eSPI() : M5GFX() {}
  bool tab5GetTouch(int& x, int& y) {
    lgfx::touch_point_t tp;
    if (M5GFX::getTouch(&tp, 1) <= 0) { _active = false; _lastStrong = 0; return false; }
#ifdef TAB5_TOUCH_DEBUG
    Serial.printf("[touch] area=%u x=%d y=%d\n", (unsigned)tp.size, (int)tp.x, (int)tp.y);
#else
    // Area + release-grace filter (see scaled class for the rationale).
    const uint32_t now = millis();
    const uint16_t minA = tab5TouchMinArea(_touchLevel);
    if (tp.size >= minA) _lastStrong = now;
    if (_lastStrong == 0 || (uint32_t)(now - _lastStrong) > kTouchReleaseMs) { _active = false; return false; }
    if (!_active) { _active = true; _downSince = now; }
    if ((uint32_t)(now - _downSince) < tab5TouchSettleMs(_touchLevel)) return false;
#endif
    x = (int)tp.x; y = (int)tp.y; return true;
  }
  void setTouchSensitivity(uint8_t level) { _touchLevel = level > 4 ? 4 : level; _active = false; _lastStrong = 0; }
  inline void writecommand(uint8_t) {}
  inline void writedata(uint8_t) {}
  inline uint16_t getTouchRawZ() { lgfx::touch_point_t tp; return (M5GFX::getTouchRaw(&tp, 1) > 0) ? 4095 : 0; }

  using M5GFX::drawString;
  using M5GFX::drawCentreString;
  using M5GFX::drawNumber;
  using M5GFX::textWidth;
  using M5GFX::drawChar;

  inline int32_t drawString(const char* s, int32_t x, int32_t y, uint8_t font) {
    setTextFont(font); return M5GFX::drawString(s, x, y);
  }
  inline int32_t drawString(const String& s, int32_t x, int32_t y, uint8_t font) {
    setTextFont(font); return M5GFX::drawString(s.c_str(), x, y);
  }
  inline int32_t drawCentreString(const char* s, int32_t x, int32_t y, uint8_t font) {
    setTextFont(font);
    auto prev = getTextDatum();
    setTextDatum(lgfx::v1::textdatum_t::top_center);
    int32_t w = M5GFX::drawString(s, x, y);
    setTextDatum(prev);
    return w;
  }
  inline int32_t drawCentreString(const String& s, int32_t x, int32_t y, uint8_t font) {
    return drawCentreString(s.c_str(), x, y, font);
  }
  inline int32_t drawNumber(long n, int32_t x, int32_t y, uint8_t font) {
    setTextFont(font); return M5GFX::drawNumber(n, x, y);
  }
  inline int32_t textWidth(const char* s, uint8_t font) {
    setTextFont(font); return M5GFX::textWidth(s);
  }
  inline int32_t textWidth(const String& s, uint8_t font) {
    setTextFont(font); return M5GFX::textWidth(s.c_str());
  }
  inline int32_t drawChar(uint16_t c, int32_t x, int32_t y, uint8_t font) {
    setTextFont(font); return M5GFX::drawChar(c, x, y);
  }
};
#endif  // TAB5_SCALED_UI

// The logical canvas height (TAB5_UI_H, above) and the app's TFT_HEIGHT macro
// (shared.h) MUST be equal or layout math desyncs. Enforced in any translation
// unit that has both visible (shared.h included before this header).
#if defined(BOARD_TAB5) && TAB5_SCALED_UI && defined(TFT_HEIGHT)
static_assert(TAB5_UI_H == TFT_HEIGHT,
              "TAB5_UI_H (compat/TFT_eSPI.h) must equal TFT_HEIGHT (shared.h)");
#endif
