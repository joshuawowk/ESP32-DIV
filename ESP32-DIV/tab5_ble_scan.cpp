// -----------------------------------------------------------------------------
// tab5_ble_scan.cpp — BleScan for the M5Stack Tab5 (ESP32-P4) via the IDF NimBLE
// C API (NimBLE-Arduino won't build on P4 — it bundles a NimBLE that conflicts
// with the framework's IDF NimBLE). BLE runs as host on the P4 with the
// controller on the C6 over esp-hosted.
//
// Bring-up (verified against this framework): hostedInitBLE() (Arduino core,
// esp32-hal-hosted) starts the SDIO link + C6 BT controller; then
// nimble_port_init() + a host task; scanning starts from the sync callback.
// Discovery runs asynchronously and streams advertising reports into a
// MAC-deduplicated table that the UI renders. Replaces the BleScan stub; the
// other bluetooth.cpp features remain stubbed until bluetooth.cpp is ported.
// -----------------------------------------------------------------------------
#if defined(BOARD_TAB5)

#include "config.h"
#include "shared.h"
#include "SettingsStore.h"
#include "Touchscreen.h"
#include "utils.h"
#include "esp32-hal-hosted.h"   // hostedInitBLE()

extern "C" {
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"
}

extern TFT_eSPI tft;

namespace BleScan {

struct Dev {
  uint8_t addr[6];
  char    name[24];
  int8_t  rssi;
};

static const int MAXDEV = 96;
static Dev s_dev[MAXDEV];
static volatile int s_count = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static bool s_hostStarted = false;
static volatile bool s_synced = false;
static uint8_t s_ownAddrType = 0;
static volatile bool s_scanning = false;
static bool s_bringupFailed = false;   // C6 hosted BLE bring-up failed this session

// UI state
static int s_sel = 0;
static int s_top = 0;
static volatile bool s_needRedraw = true;
static uint32_t s_lastUiMs = 0;

// ---- device table (written from the NimBLE host task, read from the UI) ----
static void addOrUpdate(const uint8_t* addr, int8_t rssi, const char* name) {
  portENTER_CRITICAL(&s_mux);
  int idx = -1;
  for (int i = 0; i < s_count; i++) {
    if (memcmp(s_dev[i].addr, addr, 6) == 0) { idx = i; break; }
  }
  if (idx < 0 && s_count < MAXDEV) {
    idx = s_count++;
    memcpy(s_dev[idx].addr, addr, 6);
    s_dev[idx].name[0] = 0;
  }
  if (idx >= 0) {
    s_dev[idx].rssi = rssi;
    if (name && name[0]) {
      strncpy(s_dev[idx].name, name, sizeof(s_dev[idx].name) - 1);
      s_dev[idx].name[sizeof(s_dev[idx].name) - 1] = 0;
    }
  }
  portEXIT_CRITICAL(&s_mux);
}

static void clearDevices() {
  portENTER_CRITICAL(&s_mux);
  s_count = 0;
  portEXIT_CRITICAL(&s_mux);
  s_sel = 0;
  s_top = 0;
}

// ---- NimBLE callbacks (run on the host task) ----
static int gap_event(struct ble_gap_event* ev, void* /*arg*/) {
  if (ev->type == BLE_GAP_EVENT_DISC) {
    const struct ble_gap_disc_desc* d = &ev->disc;
    char nm[24] = {0};
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0 &&
        f.name != NULL && f.name_len > 0) {
      uint8_t n = f.name_len < sizeof(nm) - 1 ? f.name_len : sizeof(nm) - 1;
      memcpy(nm, f.name, n);
      nm[n] = 0;
    }
    addOrUpdate(d->addr.val, d->rssi, nm);   // addr.val is little-endian
    s_needRedraw = true;
  } else if (ev->type == BLE_GAP_EVENT_DISC_COMPLETE) {
    s_scanning = false;
    s_needRedraw = true;
  }
  return 0;
}

static void startScan() {
  struct ble_gap_disc_params p;
  memset(&p, 0, sizeof(p));
  p.passive = 0;             // active scan -> solicit scan-response local names
  p.filter_duplicates = 0;   // keep RSSI fresh; we dedup by MAC ourselves
  p.itvl = 0x0060;
  p.window = 0x0030;
  int rc = ble_gap_disc(s_ownAddrType, BLE_HS_FOREVER, &p, gap_event, NULL);
  s_scanning = (rc == 0 || rc == BLE_HS_EALREADY);
}

static void on_sync() {
  ble_hs_util_ensure_addr(0);
  if (ble_hs_id_infer_auto(0, &s_ownAddrType) != 0) return;
  s_synced = true;
  startScan();
}
static void on_reset(int /*reason*/) { s_synced = false; s_scanning = false; }
static void host_task(void* /*param*/) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

static bool ensureHost() {
  if (s_hostStarted) return true;
  if (!hostedInitBLE()) return false;          // SDIO link + C6 BT controller
  if (nimble_port_init() != ESP_OK) return false;
  ble_hs_cfg.sync_cb = on_sync;
  ble_hs_cfg.reset_cb = on_reset;
  nimble_port_freertos_init(host_task);
  s_hostStarted = true;
  return true;
}

// ---- UI ----
static void drawList() {
  const int bottom = featureHasTouchNavBar() ? (int)touchNavContentBottomY() : TFT_HEIGHT;
  tft.fillRect(0, 38, TFT_WIDTH, bottom - 38, TFT_BLACK);

  if (s_bringupFailed) {
    tft.setTextFont(1);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(RED, TFT_BLACK);
    tft.setCursor(6, 44);
    tft.print("BLE bring-up failed (C6 hosted).");
    tft.setTextColor(WHITE, TFT_BLACK);
    tft.setCursor(6, 64);
    tft.print("Tap Rescan to retry.");
    return;
  }

  int cnt;
  portENTER_CRITICAL(&s_mux);
  cnt = s_count;
  portEXIT_CRITICAL(&s_mux);

  tft.setTextFont(1);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(GREEN, TFT_BLACK);
  tft.setCursor(6, 40);
  tft.printf("BLE Devices: %d%s", cnt, s_scanning ? "  scanning.." : "");

  if (cnt == 0) {
    tft.setTextColor(WHITE, TFT_BLACK);
    tft.setCursor(6, 62);
    tft.print(s_scanning ? "Listening for adverts..." : "No devices. Tap Rescan.");
    return;
  }

  const int rowH = 18, y0 = 60;
  int rows = (bottom - y0) / rowH;
  if (rows < 1) rows = 1;
  if (s_sel < 0) s_sel = 0;
  if (s_sel >= cnt) s_sel = cnt - 1;
  if (s_sel < s_top) s_top = s_sel;
  if (s_sel >= s_top + rows) s_top = s_sel - rows + 1;

  for (int i = 0; i < rows; i++) {
    const int di = s_top + i;
    if (di >= cnt) break;
    Dev d;
    portENTER_CRITICAL(&s_mux);
    d = s_dev[di];
    portEXIT_CRITICAL(&s_mux);
    const int y = y0 + i * rowH;
    const bool seld = (di == s_sel);
    tft.setTextColor(seld ? ORANGE : WHITE, TFT_BLACK);
    tft.setCursor(6, y);
    const char* nm = d.name[0] ? d.name : "(unknown)";
    tft.printf("%s%.20s", seld ? ">" : " ", nm);
    tft.setCursor(TFT_WIDTH - 42, y);
    tft.printf("%4d", (int)d.rssi);
    // second line: MAC (dimmer)
    tft.setTextColor(seld ? ORANGE : GRAY, TFT_BLACK);
    tft.setCursor(14, y + 9);
    tft.printf("%02X:%02X:%02X:%02X:%02X:%02X",
               d.addr[5], d.addr[4], d.addr[3], d.addr[2], d.addr[1], d.addr[0]);
  }
}

// ---- public entrypoints (match config.h namespace BleScan) ----
void bleScanSetup() {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  // nav slots: left, down, center, up, right
  setTouchNavLabels("Rescan", "Down", "Exit", "Up", nullptr);
  tft.fillScreen(TFT_BLACK);
  drawStatusBar(currentBatteryVoltage, true);
  redrawTouchButtonBar();

  s_needRedraw = true;
  s_lastUiMs = 0;

  s_bringupFailed = !ensureHost();   // drawList() renders the failure state (persistent, not overwritten)
  if (s_bringupFailed) return;
  clearDevices();
  if (s_synced) startScan();   // else on_sync() starts it once the C6 is ready
}

void bleScanLoop() {
  if (feature_active && isButtonPressed(BTN_SELECT)) {
    feature_exit_requested = true;
    return;
  }
  if (isButtonPressedEdge(BTN_DOWN)) { s_sel++; s_needRedraw = true; }
  if (isButtonPressedEdge(BTN_UP))   { s_sel--; s_needRedraw = true; }
  if (isButtonPressedEdge(BTN_LEFT)) {          // Rescan (also retries C6 bring-up)
    if (!s_hostStarted) {
      s_bringupFailed = !ensureHost();
    }
    if (!s_bringupFailed) { clearDevices(); if (s_synced) startScan(); }
    s_needRedraw = true;
  }

  const uint32_t now = millis();
  if (s_needRedraw && (now - s_lastUiMs) > 250) {
    drawList();
    s_lastUiMs = now;
    s_needRedraw = false;
  }
  updateStatusBar();
  delay(10);
}

void exit() {
  if (s_scanning) {
    ble_gap_disc_cancel();
    s_scanning = false;
  }
  // Leave the NimBLE host running (shared, brought up once).
}

int getLastCount() {
  return settings().autoBleScan ? s_count : 0;
}

void startBackgroundScanner() {
  // Deferred on the Tab5: the C6 hosted BT link may not be ready at boot, so the
  // NimBLE host is brought up lazily on first entry to BLE Scan. Symbol must
  // exist (called once from setup()); safe no-op here.
}

}  // namespace BleScan

#endif  // BOARD_TAB5
