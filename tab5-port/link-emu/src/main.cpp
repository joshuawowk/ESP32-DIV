// link-emu — exercises the real host LinkClient against the Python companion
// simulator over UART1 (esp-emu --uart1-tcp bridge). Proves the on-target
// reliable handshake (HELLO->CAPS), CMD/ACK, and telemetry (LOG) end-to-end.
#include <Arduino.h>
#include "link_client.h"
#include "divproto.h"

using namespace divproto;

static LinkClient g_link;
static bool announced = false;

static void onEvt(uint8_t, divlink::Type t, const uint8_t* p, uint16_t n, void*) {
  if (t == divlink::Type::LOG) { Serial.write(p, n); Serial.println(); }
}

void setup() {
  Serial.begin(115200);          // UART0 — captured by esp-emu
  delay(200);
  Serial.println("\n=== TAB5 LINK-EMU (ESP32-P4 r3) ===");
  // UART1 pins are irrelevant under emulation (bridged to TCP); use the real
  // host mapping (RX=G7, TX=G6) for parity.
  g_link.begin(&Serial1, 7, 6, DIVLINK_BAUD);
  g_link.setEvtHandler(onEvt, nullptr);
  g_link.sendHello();
  Serial.println("HELLO sent, waiting for companion CAPS...");
}

void loop() {
  g_link.poll();                 // pumps RX + re-HELLOs every 1s until linked
  if (!announced && g_link.linked()) {
    announced = true;
    Serial.printf("CAPS: fw=0x%04X psram=%u bitmap0=0x%08X\n",
                  g_link.caps().fw_major_minor, g_link.caps().has_psram,
                  (unsigned)g_link.caps().feature_bitmap0);
    // Fire a reliable command to exercise CMD/ACK end-to-end.
    uint8_t p[2] = { (uint8_t)Cmd::FEATURE_START, (uint8_t)Feature::NRF_SCANNER };
    g_link.sendCmd((uint8_t)Feature::NRF_SCANNER, p, 2, true);
  }
  if (announced && g_link.acks() >= 2) {   // HELLO ack + FEATURE_START ack
    Serial.printf("rxOk=%u acks=%u rxErr=%u\n",
                  (unsigned)g_link.rxOk(), (unsigned)g_link.acks(), (unsigned)g_link.rxErr());
    Serial.println("LINK_UP");             // esp-emu --exit-on sentinel
    Serial.flush();
    delay(50);
    while (true) { delay(1000); }          // done
  }
  delay(5);
}
