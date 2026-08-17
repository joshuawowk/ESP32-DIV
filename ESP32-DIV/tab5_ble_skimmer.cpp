// -----------------------------------------------------------------------------
// tab5_ble_skimmer.cpp — BleSkimmer for the M5Stack Tab5 (ESP32-P4).
//
// Bluetooth card-skimmer detector: continuously BLE-scans and flags devices
// whose advertised name matches known cheap BT/BLE serial-module signatures
// (HC-05, HM-10, JDY-xx, BT-05, ...) used in real skimmers. A match is a WARNING,
// not proof. The detection layer (signature table + name normalize/match + hit
// table) is copied verbatim from bluetooth.cpp; only the scan glue is rewritten
// against the IDF NimBLE C API, reusing the shared host (tab5_ble.h) like the
// scanner and advertising features.
// -----------------------------------------------------------------------------
#if defined(BOARD_TAB5)

#include "config.h"
#include "shared.h"
#include "SettingsStore.h"
#include "Touchscreen.h"
#include "utils.h"
#include "tab5_ble.h"

#include <string.h>

extern "C" {
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"
}

extern TFT_eSPI tft;

namespace BleSkimmer {

// ---- detection layer (verbatim behavior from bluetooth.cpp) ----
struct Signature { const char* needle; const char* label; uint8_t threat; };
static const Signature s_sigs[] = {
  {"FREE2MOVE","FREE2MOVE",5},{"BTHC05","BT-HC05",5},{"BTHC06","BT-HC06",5},
  {"MLTBT05","MLT-BT05",4},{"BT04A","BT04-A",4},{"BTSPP","BT-SPP",4},{"CC41A","CC41-A",4},
  {"SPPCA","SPP-CA",4},{"LINVOR","LINVOR",4},{"HC03","HC-03",5},{"HC04","HC-04",5},
  {"HC05","HC-05",5},{"HC06","HC-06",5},{"HC08","HC-08",5},{"BT04","BT-04",4},
  {"BT05","BT-05",4},{"BT06","BT-06",4},{"BT08","BT-08",4},{"CC41","CC41",4},
  {"HM10","HM-10",3},{"HM11","HM-11",3},{"HM19","HM-19",3},{"AT09","AT-09",3},
  {"JDY08","JDY-08",4},{"JDY10","JDY-10",4},{"JDY16","JDY-16",4},{"JDY23","JDY-23",4},
  {"JDY31","JDY-31",4},
};

static void normalizeName(const char* in, char* out, size_t outSz) {
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 1 < outSz; ++i) {
    char c = in[i];
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out[o++] = c;
  }
  out[o] = '\0';
}

// Returns true + fills label/threat if the (raw) name matches a skimmer signature.
static bool matchSignature(const char* name, const char** label, uint8_t* threat) {
  char norm[32];
  normalizeName(name, norm, sizeof(norm));
  if (norm[0] == '\0') return false;
  if (strncmp(norm, "JDY", 3) == 0) { *label = "JDY-*"; *threat = 4; return true; }
  for (const auto& s : s_sigs) {
    if (strstr(norm, s.needle)) { *label = s.label; *threat = s.threat; return true; }
  }
  return false;
}

// ---- hit table (MAC-deduped) ----
struct Hit { uint8_t addr[6]; char name[20]; char label[14]; int8_t rssi; uint8_t threat; uint16_t hits; };
static const int MAXHITS = 32;
static Hit s_hit[MAXHITS];
static volatile int s_count = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_dirty = true;

static int findHit(const uint8_t* a) {
  for (int i = 0; i < s_count; i++) if (memcmp(s_hit[i].addr, a, 6) == 0) return i;
  return -1;
}

static void upsertHit(const uint8_t* a, const char* name, const char* label, uint8_t threat, int8_t rssi) {
  portENTER_CRITICAL(&s_mux);
  int idx = findHit(a);
  if (idx < 0) {
    if (s_count < MAXHITS) {
      idx = s_count++;
    } else {
      int weak = 0;
      for (int i = 1; i < s_count; i++) if (s_hit[i].rssi < s_hit[weak].rssi) weak = i;
      if (rssi > s_hit[weak].rssi) idx = weak; else { portEXIT_CRITICAL(&s_mux); return; }
    }
    memcpy(s_hit[idx].addr, a, 6);
    s_hit[idx].name[0] = 0; s_hit[idx].hits = 0;
  }
  s_hit[idx].rssi = rssi;
  s_hit[idx].hits++;
  if (threat > s_hit[idx].threat) s_hit[idx].threat = threat;
  if (name && name[0]) { strncpy(s_hit[idx].name, name, sizeof(s_hit[idx].name)-1); s_hit[idx].name[sizeof(s_hit[idx].name)-1]=0; }
  if (label && label[0]) { strncpy(s_hit[idx].label, label, sizeof(s_hit[idx].label)-1); s_hit[idx].label[sizeof(s_hit[idx].label)-1]=0; }
  portEXIT_CRITICAL(&s_mux);
  s_dirty = true;
}

static void clearHits() { portENTER_CRITICAL(&s_mux); s_count = 0; portEXIT_CRITICAL(&s_mux); s_dirty = true; }

// ---- NimBLE scan glue ----
static volatile bool s_scanning = false;
static int s_sel = 0, s_top = 0;
static uint32_t s_lastUi = 0;
static bool s_started = false;

static int gap_event(struct ble_gap_event* ev, void* /*arg*/) {
  if (ev->type == BLE_GAP_EVENT_DISC) {
    const struct ble_gap_disc_desc* d = &ev->disc;
    char nm[24] = {0};
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0 && f.name && f.name_len) {
      uint8_t n = f.name_len < sizeof(nm)-1 ? f.name_len : sizeof(nm)-1;
      memcpy(nm, f.name, n); nm[n] = 0;
    }
    const char* label = nullptr; uint8_t threat = 0;
    if (nm[0] && matchSignature(nm, &label, &threat)) {
      upsertHit(d->addr.val, nm, label, threat, d->rssi);
    }
  } else if (ev->type == BLE_GAP_EVENT_DISC_COMPLETE) {
    s_scanning = false;
  }
  return 0;
}

static void startScan() {
  struct ble_gap_disc_params p; memset(&p, 0, sizeof(p));
  p.passive = 0; p.filter_duplicates = 0; p.itvl = 0x0060; p.window = 0x0030;
  int rc = ble_gap_disc(tab5BleOwnAddrType(), BLE_HS_FOREVER, &p, gap_event, NULL);
  s_scanning = (rc == 0 || rc == BLE_HS_EALREADY);
}
static void stopScan() { if (ble_gap_adv_active()) {} ble_gap_disc_cancel(); s_scanning = false; }

// ---- UI ----
static void navLabels() {
  setTouchNavLabels(s_scanning ? "Stop" : "Start", "Clear", "Exit", "Prev", "Next");
  redrawTouchButtonBar();
}

static void drawList() {
  const int bottom = featureHasTouchNavBar() ? (int)touchNavContentBottomY() : TFT_HEIGHT;
  tft.fillRect(0, 38, TFT_WIDTH, bottom - 38, TFT_BLACK);
  int cnt; portENTER_CRITICAL(&s_mux); cnt = s_count; portEXIT_CRITICAL(&s_mux);

  tft.setTextFont(1); tft.setTextDatum(TL_DATUM);
  tft.setTextColor(cnt ? ORANGE : GREEN, TFT_BLACK);
  tft.setCursor(6, 40);
  if (cnt == 0) tft.printf("No suspects%s", s_scanning ? "  (scanning)" : "");
  else          tft.printf("%d suspect%s nearby%s", cnt, cnt==1?"":"s", s_scanning ? "  scan" : "");

  if (cnt == 0) {
    tft.setTextColor(WHITE, TFT_BLACK); tft.setCursor(6, 62);
    tft.print("Watching for HC-05, HM-10,");
    tft.setCursor(6, 74); tft.print("JDY, BT-05 & similar names.");
    tft.setTextColor(GRAY, TFT_BLACK); tft.setCursor(6, 96);
    tft.print("A match is a warning, not proof.");
    return;
  }
  const int rowH = 22, y0 = 58;
  int rows = (bottom - y0) / rowH; if (rows < 1) rows = 1;
  if (s_sel < 0) s_sel = 0; if (s_sel >= cnt) s_sel = cnt - 1;
  if (s_sel < s_top) s_top = s_sel;
  if (s_sel >= s_top + rows) s_top = s_sel - rows + 1;
  for (int i = 0; i < rows; i++) {
    int di = s_top + i; if (di >= cnt) break;
    Hit h; portENTER_CRITICAL(&s_mux); h = s_hit[di]; portEXIT_CRITICAL(&s_mux);
    int y = y0 + i * rowH; bool sel = (di == s_sel);
    tft.setTextColor(sel ? ORANGE : WHITE, TFT_BLACK);
    tft.setCursor(6, y);
    const char* nm = h.name[0] ? h.name : (h.label[0] ? h.label : "Suspect");
    tft.printf("%s%.16s", sel ? ">" : " ", nm);
    tft.setCursor(TFT_WIDTH - 42, y); tft.printf("%4d", (int)h.rssi);
    tft.setTextColor(sel ? ORANGE : GRAY, TFT_BLACK);
    tft.setCursor(14, y + 9);
    tft.printf("%02X:%02X:%02X:%02X:%02X:%02X  %s",
               h.addr[5], h.addr[4], h.addr[3], h.addr[2], h.addr[1], h.addr[0],
               h.label[0] ? h.label : "");
  }
}

// ---- public entrypoints ----
void bleSkimmerSetup() {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  s_sel = 0; s_top = 0; s_dirty = true; s_lastUi = 0; s_started = false;
  clearHits();
  tft.fillScreen(TFT_BLACK);
  drawStatusBar(currentBatteryVoltage, true);
  navLabels();
  if (!tab5BleEnsureHost()) {
    tft.setTextColor(RED, TFT_BLACK); tft.setTextDatum(TL_DATUM); tft.setCursor(6, 44);
    tft.print("BLE bring-up failed (C6 hosted)");
    return;
  }
  if (tab5BleSynced()) { startScan(); s_started = true; navLabels(); }
}

void bleSkimmerLoop() {
  if (feature_active && isButtonPressed(BTN_SELECT)) { feature_exit_requested = true; return; }
  if (isButtonPressedEdge(BTN_LEFT)) {              // Start/Stop
    if (s_scanning) stopScan(); else if (tab5BleSynced()) { startScan(); s_started = true; }
    navLabels(); s_dirty = true;
  }
  if (isButtonPressedEdge(BTN_DOWN)) { clearHits(); }        // Clear
  if (isButtonPressedEdge(BTN_UP))   { s_sel--; s_dirty = true; }   // Prev
  if (isButtonPressedEdge(BTN_RIGHT)){ s_sel++; s_dirty = true; }   // Next

  if (!s_started && tab5BleSynced()) { startScan(); s_started = true; navLabels(); }   // cold-boot start
  // watchdog: keep continuous scan alive
  if (s_started && !s_scanning && tab5BleSynced()) { startScan(); }

  const uint32_t now = millis();
  if ((s_dirty || (now - s_lastUi) > 1000) && (now - s_lastUi) > 250) {
    drawList(); s_lastUi = now; s_dirty = false;
  }
  updateStatusBar();
  delay(5);
}

void exit() { stopScan(); }

}  // namespace BleSkimmer

#endif  // BOARD_TAB5
