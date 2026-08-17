// div_companion.h — the DIV_COMPANION seam. Turns the ESP32-DIV firmware into a
// headless RF backend driven by the Tab5 host over divlink. Compiles into the
// existing Arduino/S3 build behind -DDIV_COMPANION. See PORTING_PLAN.md §7.4/§10
// and companion/INTEGRATION.md for the exact ESP32-DIV.ino edits.
#ifndef DIV_COMPANION_H
#define DIV_COMPANION_H
#include <stdint.h>
#include "divproto.h"

// A feature is exposed to the host as a start/stop pair mapping onto the DIV's
// existing namespace setup()/exit() functions (e.g. Scanner::scannerSetup).
typedef void (*FeatureFn)();
struct FeatureEntry {
  divproto::Feature id;
  FeatureFn start;   // e.g. []{ Scanner::scannerSetup(); }
  FeatureFn stop;    // e.g. []{ Scanner::exit(); }
};

// Register the feature table (static array owned by caller) once at boot.
void divcompRegister(const FeatureEntry* table, int count);

// Call from setup() (after Serial/radios init) and loop() respectively.
void divcompSetup();
void divcompLoop();

// The active feature's loop must call this so the host's INPUT events reach the
// DIV button/touch globals, and telemetry can be pushed. Returns true while the
// host wants the feature to keep running (FEATURE_STOP clears it).
bool divcompFeatureActive();

// Telemetry helpers a ported feature calls instead of drawing locally (Stage-B).
void divcompSendEvt(divproto::Feature f, const void* p, uint16_t n);
void divcompSendStream(divproto::Feature f, const void* p, uint16_t n);
void divcompLog(const char* s);

// Count of frames dropped because the link TX FIFO was full (backpressure).
uint32_t divcompTxDropped();

// NOTE: the link uses the Serial1 UART peripheral. On the stock DIV S3 build,
// Serial1 may already be claimed by the Neo-6M GPS. If so, move the link to
// Serial2 (define DIVCOMP_UART) — the GPIOs (8/9) are separate from that. (M3.)

#endif // DIV_COMPANION_H
