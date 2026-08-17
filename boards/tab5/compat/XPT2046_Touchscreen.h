// -----------------------------------------------------------------------------
// XPT2046_Touchscreen stub for the M5Stack Tab5 (ESP32-P4) port.
//
// The classic ESP32-DIV uses an XPT2046 resistive controller on a dedicated SPI
// bus. The Tab5 uses a GT911 capacitive panel that M5GFX brings up during
// autodetect and exposes through tft.getTouch(). The Tab5 branch of
// Touchscreen.cpp reads touch that way; this stub only exists so the unchanged
// `<XPT2046_Touchscreen.h>` include and the `ts` declaration still compile.
// -----------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <SPI.h>

class TS_Point {
 public:
  TS_Point() : x(0), y(0), z(0) {}
  TS_Point(int16_t _x, int16_t _y, int16_t _z) : x(_x), y(_y), z(_z) {}
  int16_t x, y, z;
};

class XPT2046_Touchscreen {
 public:
  explicit XPT2046_Touchscreen(uint8_t /*cs*/, uint8_t /*irq*/ = 255) {}
  bool begin() { return false; }
  bool begin(SPIClass&) { return false; }
  void setRotation(uint8_t) {}
  bool touched() { return false; }
  bool tirqTouched() { return false; }
  TS_Point getPoint() { return TS_Point(); }
};
