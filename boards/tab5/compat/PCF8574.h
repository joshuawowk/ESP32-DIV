// -----------------------------------------------------------------------------
// PCF8574 stub for the M5Stack Tab5 (ESP32-P4) port.
//
// The classic ESP32-DIV reads its 5-way D-pad through a PCF8574 I2C expander.
// The Tab5 has no such expander (HAS_PCF8574_BUTTONS == 0), so all input is
// routed through the on-screen touch nav bar and getPcf8574Address() returns 0,
// meaning the `pcf` object is constructed but never actually read. This stub
// satisfies the existing `<PCF8574.h>` include and the `pcf` API surface
// (Mischianti-style pinMode/digitalRead/digitalWrite/begin) without pulling in
// the real library. digitalRead() returns HIGH == "not pressed" (buttons are
// active-LOW) so any stray read is harmless.
// -----------------------------------------------------------------------------
#pragma once

#include <Arduino.h>

class PCF8574 {
 public:
  explicit PCF8574(uint8_t address = 0x20) : _address(address) {}
  bool begin() { return false; }
  bool begin(uint8_t) { return false; }
  void pinMode(uint8_t, uint8_t) {}
  uint8_t digitalRead(uint8_t) { return HIGH; }
  void digitalWrite(uint8_t, uint8_t) {}
  uint8_t read8() { return 0xFF; }

 private:
  uint8_t _address;
};
