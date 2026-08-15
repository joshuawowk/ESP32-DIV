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

// ---- GT911 touch sensitivity (raise thresholds = less sensitive) ----
// Reduces the Tab5's over-eager capacitive touch by raising the GT911
// Screen_Touch_Level (0x8053) / Screen_Leave_Level (0x8054) config registers.
// Uses LovyanGFX's i2c on the same already-initialized port M5GFX owns (I2C_NUM_1,
// SDA31/SCL32) — never Arduino Wire1 (it maps to the same port on P4 and would
// collide). Config is written to RAM only (0x8047 left untouched, so the GT911
// does not burn its NVM); must be re-applied after each tft.init().
// level 0 = most sensitive (factory-ish) .. 4 = least sensitive.
static inline bool tab5Gt911SetSensitivityLevel(uint8_t level) {
  static const uint8_t kTouch[5] = {0x38, 0x50, 0x68, 0x80, 0x98};
  if (level > 4) level = 4;
  const uint8_t touch = kTouch[level];
  const uint8_t leave = (touch > 0x18) ? (uint8_t)(touch - 0x18) : 0x10;

  constexpr int      PORT = 1;
  constexpr uint32_t FREQ = 400000;
  constexpr size_t   CFGLEN = 184;          // 0x8047..0x80FE
  const uint8_t addrs[2] = {0x14, 0x5D};
  uint8_t addr = 0;
  for (uint8_t a : addrs) {
    const uint8_t probe[2] = {0x80, 0x40};
    uint8_t t = 0;
    if (lgfx::i2c::transactionWriteRead(PORT, a, probe, 2, &t, 1, FREQ).has_value()) { addr = a; break; }
  }
  if (!addr) return false;

  uint8_t buf[2 + CFGLEN + 2];
  buf[0] = 0x80; buf[1] = 0x47;
  const uint8_t rd[2] = {buf[0], buf[1]};
  if (!lgfx::i2c::transactionWriteRead(PORT, addr, rd, 2, &buf[2], CFGLEN, FREQ).has_value()) return false;

  buf[2 + (0x8053 - 0x8047)] = touch;       // -> buf[14]
  buf[2 + (0x8054 - 0x8047)] = leave;       // -> buf[15]

  uint8_t sum = 0;
  for (size_t i = 0; i < CFGLEN; ++i) sum += buf[2 + i];
  buf[2 + CFGLEN]     = (uint8_t)((~sum) + 1);   // checksum @ 0x80FF
  buf[2 + CFGLEN + 1] = 0x01;                    // config-fresh @ 0x8100
  return lgfx::i2c::transactionWrite(PORT, addr, buf, sizeof(buf), FREQ).has_value();
}

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
  // Anti-aliased zoom smooths the 3x upscale (less blocky text) vs nearest-neighbor.
  void flush() {
    if (_panelMux) xSemaphoreTake(_panelMux, portMAX_DELAY);
    pushRotateZoomWithAA(&_panel, _panel.width() * 0.5f, _panel.height() * 0.5f,
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

  void setTouchSensitivity(uint8_t level) {
    if (_panelMux) xSemaphoreTake(_panelMux, portMAX_DELAY);
    tab5Gt911SetSensitivityLevel(level);   // serialize GT911 i2c vs touch reads
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
  void setTouchSensitivity(uint8_t level) { tab5Gt911SetSensitivityLevel(level); }
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
