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
#define TAB5_UI_H 320
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

#if TAB5_SCALED_UI
// ------------------------- scaled (offscreen canvas) -------------------------
class TFT_eSPI : public lgfx::LGFX_Sprite {
  M5GFX _panel;
  float _scale = 1.0f;
  float _offx = 0.0f, _offy = 0.0f;
  SemaphoreHandle_t _panelMux = nullptr;   // serializes _panel access (flush task vs touch reads)

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

    const float sx = (float)_panel.width()  / (float)TAB5_UI_W;
    const float sy = (float)_panel.height() / (float)TAB5_UI_H;
    _scale = sx < sy ? sx : sy;                       // integer-ish fit (3x)
    _offx = (_panel.width()  - TAB5_UI_W * _scale) * 0.5f;
    _offy = (_panel.height() - TAB5_UI_H * _scale) * 0.5f;
    flush();
    xTaskCreatePinnedToCore(flushTaskThunk, "tab5flush", 4096, this, 1, nullptr, 0);
  }

  // Blit the canvas to the panel, scaled and centered (serialized vs touch reads).
  void flush() {
    if (_panelMux) xSemaphoreTake(_panelMux, portMAX_DELAY);
    pushRotateZoom(&_panel, _panel.width() * 0.5f, _panel.height() * 0.5f,
                   0.0f, _scale, _scale);
    if (_panelMux) xSemaphoreGive(_panelMux);
  }

  // Read panel touch and map to canvas coordinates. Fast (I2C only) — the flush
  // runs on its own task, so touch is polled at the app loop's full rate.
  bool tab5GetTouch(int& x, int& y) {
    int32_t px = 0, py = 0;
    bool ok;
    if (_panelMux) xSemaphoreTake(_panelMux, portMAX_DELAY);
    ok = _panel.getTouch(&px, &py) > 0;
    if (_panelMux) xSemaphoreGive(_panelMux);
    if (!ok) return false;
    x = (int)((px - _offx) / _scale);
    y = (int)((py - _offy) / _scale);
    return true;
  }

  void setBrightness(uint8_t b) {
    if (_panelMux) xSemaphoreTake(_panelMux, portMAX_DELAY);
    _panel.setBrightness(b);
    if (_panelMux) xSemaphoreGive(_panelMux);
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
 public:
  TFT_eSPI() : M5GFX() {}
  bool tab5GetTouch(int& x, int& y) {
    int32_t px = 0, py = 0;
    if (!M5GFX::getTouch(&px, &py)) return false;
    x = (int)px; y = (int)py; return true;
  }
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
