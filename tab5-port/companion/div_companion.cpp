// div_companion.cpp — companion glue. Arduino/S3 build (behind -DDIV_COMPANION).
// Bridges divlink CMD/INPUT/telemetry to the DIV firmware's existing features.
#ifdef DIV_COMPANION
#include <Arduino.h>
#include "div_companion.h"
#include "link_server.h"

using namespace divlink;
using namespace divproto;

// Link UART on the S3 side — pick verified-free pins (plan §16.1 / §17.3 MR-6).
// NOT 3/6 (CC1101 GDO/GPS). 8/9 are free on the DIV V2 S3 map.
#ifndef DIVCOMP_RX
#define DIVCOMP_RX 8
#endif
#ifndef DIVCOMP_TX
#define DIVCOMP_TX 9
#endif
#ifndef DIVCOMP_BAUD
#define DIVCOMP_BAUD 3000000
#endif

static LinkServer     s_srv;
static const FeatureEntry* s_tab = nullptr;
static int            s_tabN = 0;
static Feature        s_active = Feature::NONE;
static volatile bool  s_wantRun = false;
// Pending start/stop are LATCHED in the link callback and APPLIED only at the
// top level (never from inside a running feature's pump). (Review H3 / HR-3.)
static volatile Feature s_pendingStart = Feature::NONE;
static volatile bool    s_pendingStop  = false;
static uint32_t         s_txDropped    = 0;

// Host INPUT -> DIV globals (defined in div_companion_globals.cpp).
extern volatile int  g_linkBtn;    // last Btn pressed (0 = none)
extern volatile bool g_linkBtnDown;
extern volatile int  g_linkTouchX, g_linkTouchY; extern volatile bool g_linkTouch;

// Best-effort sink: never block the radio/feature loop on a full TX FIFO — drop
// and count instead (STREAM/CANVAS are lossy by design; ACKs are re-driven by
// the host's CMD retries). (Review H1 — minimal backpressure.)
static void sink(const uint8_t* d, size_t n, void*) {
  if ((size_t)Serial1.availableForWrite() >= n) Serial1.write(d, n);
  else s_txDropped++;
}

static const FeatureEntry* find(Feature f) {
  for (int i = 0; i < s_tabN; ++i) if (s_tab[i].id == f) return &s_tab[i];
  return nullptr;
}

static bool onCmd(uint8_t chan, const uint8_t* p, uint16_t n, Nak* nak, void*) {
  (void)chan;
  if (n < 1) { *nak = Nak::BAD_PARAM; return false; }
  switch ((Cmd)p[0]) {
    case Cmd::FEATURE_START: {
      if (n < 2) { *nak = Nak::BAD_PARAM; return false; }
      if (!find((Feature)p[1])) { *nak = Nak::UNSUPPORTED; return false; }
      s_pendingStart = (Feature)p[1]; s_pendingStop = false;  // latch; applied at top level
      return true;
    }
    case Cmd::FEATURE_STOP: {
      s_pendingStop = true; s_pendingStart = Feature::NONE;   // latch
      return true;
    }
    case Cmd::PING: return true;
    default: *nak = Nak::UNKNOWN_CMD; return false;
  }
}

static void onInput(const InputEvent& ev, void*) {
  if (ev.kind == (uint8_t)InputKind::TOUCH) { g_linkTouchX = ev.x; g_linkTouchY = ev.y; g_linkTouch = ev.pressed; }
  else { g_linkBtn = ev.btn; g_linkBtnDown = (ev.kind == (uint8_t)InputKind::BTN_DOWN); }
}

void divcompRegister(const FeatureEntry* table, int count) { s_tab = table; s_tabN = count; }

void divcompSetup() {
  Serial1.begin(DIVCOMP_BAUD, SERIAL_8N1, DIVCOMP_RX, DIVCOMP_TX);
  Caps caps{};
  caps.proto_ver = PROTO_VER;
  caps.fw_major_minor = 0x0107;                 // DIV v1.7.x
  caps.has_psram = (ESP.getPsramSize() > 0) ? 1 : 0;
  caps.feature_bitmap0 = 0xFFFFFFFF;            // TODO: reflect wired radios
  s_srv.begin(sink, nullptr, caps);
  s_srv.setCmdHandler(onCmd, nullptr);
  s_srv.setInputHandler(onInput, nullptr);
}

// Pump the link RX only (safe to call from within a running feature loop).
static void divcompPump() {
  uint8_t tmp[256];
  int a;
  while ((a = Serial1.available()) > 0) {
    int m = Serial1.readBytes(tmp, a > (int)sizeof(tmp) ? (int)sizeof(tmp) : a);
    if (m > 0) s_srv.feed(tmp, m);
    if (m < (int)sizeof(tmp)) break;
  }
}

// Apply latched start/stop — ONLY from the top level (no feature loop on the
// stack), so a feature is never torn down inside its own call chain. (H3.)
static void divcompApplyPending() {
  if (s_pendingStop) {
    if (s_active != Feature::NONE) { const FeatureEntry* a = find(s_active); if (a && a->stop) a->stop(); }
    s_active = Feature::NONE; s_wantRun = false; s_pendingStop = false;
  }
  if (s_pendingStart != Feature::NONE) {
    if (s_active != Feature::NONE) { const FeatureEntry* a = find(s_active); if (a && a->stop) a->stop(); }
    const FeatureEntry* e = find(s_pendingStart);
    s_active = s_pendingStart; s_wantRun = true; s_pendingStart = Feature::NONE;
    if (e && e->start) e->start();
  }
}

// Top-level service: pump + apply pending. Call from loop() when NOT inside a
// feature's blocking loop.
void divcompLoop() { divcompPump(); divcompApplyPending(); }

// Called from inside a running feature's loop: pump the link and report whether
// the feature should keep running. Returns false when a stop/switch is pending
// so the feature exits; the actual teardown happens back at divcompLoop(). (H3.)
bool divcompFeatureActive() {
  divcompPump();
  return s_wantRun && !s_pendingStop && s_pendingStart == Feature::NONE;
}

uint32_t divcompTxDropped() { return s_txDropped; }
void divcompSendEvt(Feature f, const void* p, uint16_t n)    { s_srv.sendEvt((uint8_t)f, (const uint8_t*)p, n); }
void divcompSendStream(Feature f, const void* p, uint16_t n) { s_srv.sendStream((uint8_t)f, (const uint8_t*)p, n); }
void divcompLog(const char* s) { s_srv.sendLog(s); }

#endif // DIV_COMPANION
