// Tab5 host P4 smoke test.
// Boots on the ESP32-P4 and runs the divlink protocol self-test on real
// riscv32 (in the esp-emu emulator), printing a sentinel line for --exit-on.
//
// Output goes to UART0 (Serial), which the emulator captures on UART TX.
#include <Arduino.h>
#include "divlink.h"

using namespace divlink;

static int g_fail = 0, g_pass = 0;
static void check(bool cond, const char* msg) {
  if (cond) { g_pass++; }
  else { g_fail++; Serial.print("  FAIL: "); Serial.println(msg); }
}

// Minimal on-target mirror of the native tests (no STL, fixed buffers).
static Frame  g_last;
static bool   g_got;
static uint8_t g_lastpay[64];
static void on_frame(const Frame& f, void*) {
  g_got = true; g_last = f;
  for (uint16_t i = 0; i < f.len && i < sizeof(g_lastpay); ++i) g_lastpay[i] = f.payload[i];
}

static void run_selftest() {
  // 1) CRC-16/CCITT-FALSE known vector.
  check(crc16((const uint8_t*)"123456789", 9) == 0x29B1, "crc16 vector 0x29B1");

  // 2) Frame round-trip (payload includes zero bytes).
  uint8_t pay[32];
  for (int i = 0; i < 32; ++i) pay[i] = (uint8_t)(i * 7);
  uint8_t wire[MAX_WIRE];
  size_t w = encode_frame(Type::CMD, 5, 0xBEEF, pay, 32, wire, sizeof(wire));
  check(w > 0 && wire[w - 1] == 0x00, "encode ok + delimiter");
  Parser p; p.on_frame(on_frame, nullptr); g_got = false;
  p.feed(wire, w);
  check(g_got, "frame parsed");
  check(g_last.type == Type::CMD && g_last.chan == 5 && g_last.seq == 0xBEEF, "header preserved");
  bool paymatch = (g_last.len == 32);
  for (int i = 0; i < 32 && paymatch; ++i) paymatch = (g_lastpay[i] == (uint8_t)(i * 7));
  check(paymatch, "payload preserved");
  check(p.frames_ok() == 1 && p.frames_crc_err() == 0, "counters ok");

  // 3) CRC corruption rejected.
  w = encode_frame(Type::EVT, 1, 42, pay, 16, wire, sizeof(wire));
  wire[w / 2] ^= 0x40;
  Parser p2; p2.on_frame(on_frame, nullptr); g_got = false;
  p2.feed(wire, w);
  check(!g_got && p2.frames_ok() == 0 && (p2.frames_crc_err() + p2.frames_cobs_err()) == 1,
        "corruption rejected");

  // 4) Resync after garbage.
  Parser p3; p3.on_frame(on_frame, nullptr); g_got = false;
  uint8_t garbage[] = {0x11, 0x22, 0x33};
  p3.feed(garbage, sizeof(garbage));
  uint8_t z = 0; p3.feed(&z, 1);
  const char* m = "hi";
  w = encode_frame(Type::LOG, 0, 1, (const uint8_t*)m, 2, wire, sizeof(wire));
  p3.feed(wire, w);
  check(g_got && g_last.len == 2 && g_lastpay[0] == 'h' && g_lastpay[1] == 'i', "resync ok");
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("=== TAB5-HOST-SMOKE (ESP32-P4) ===");
  Serial.printf("PSRAM: %u bytes\n", (unsigned)ESP.getPsramSize());
  Serial.printf("free heap: %u\n", (unsigned)ESP.getFreeHeap());
  run_selftest();
  Serial.printf("divlink: %d passed, %d failed\n", g_pass, g_fail);
  // Sentinels for esp-emu --exit-on.
  if (g_fail == 0) Serial.println("SMOKE_RESULT:PASS");
  else             Serial.println("SMOKE_RESULT:FAIL");
  Serial.flush();
}

void loop() {
  delay(1000);
  Serial.println("HEARTBEAT");
}
