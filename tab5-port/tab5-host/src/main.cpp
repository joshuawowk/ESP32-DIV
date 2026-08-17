// Tab5 host firmware — main. Boots M5Unified, renders the ESP32-DIV menu on
// M5GFX, routes A164/touch input, and drives the S3 companion over the link.
// Connectivity-class Wi-Fi/BLE will run natively on the C6 (esp-hosted); RF/
// offense features are proxied to the companion. See PORTING_PLAN.md §6.
#include <M5Unified.h>
#include "gfx.h"
#include "input_router.h"
#include "link_client.h"
#include "divproto.h"

using namespace divproto;

// Link UART: M5-Bus "PC" UART — host RX=G7, host TX=G6 (§7.1/§16.1).
// Under esp-emu these are bridged via --uart1-tcp regardless of pin numbers.
static const int LINK_RX = 7, LINK_TX = 6;

static InputRouter input;
static LinkClient  g_link;   // NB: not 'link' — collides with POSIX link() in <unistd.h>
static Layout      L;

// A representative slice of the DIV feature menu (full registry in divproto.h).
// `home` decides routing (plan §5): P4-native connectivity features run on the
// C6 (esp-hosted); everything else is proxied to the S3 companion over the link.
enum Home : uint8_t { HOME_COMPANION = 0, HOME_P4 = 1 };
struct MenuItem { const char* name; Feature feat; Home home; };
static const MenuItem MENU[] = {
  {"Wi-Fi Scan",        Feature::WIFI_SCAN,           HOME_P4},        // C6-native (§5)
  {"Packet Monitor",    Feature::WIFI_PACKET_MONITOR, HOME_COMPANION},
  {"Deauther",          Feature::WIFI_DEAUTH,         HOME_COMPANION}, // Evil-Twin/raw-TX (§17 HR-4)
  {"BLE Scan",          Feature::BLE_SCAN,            HOME_P4},        // C6-native (§5)
  {"Sour Apple",        Feature::BLE_SOUR_APPLE,      HOME_COMPANION},
  {"nRF24 Scanner",     Feature::NRF_SCANNER,         HOME_COMPANION},
  {"Sub-GHz Replay",    Feature::SUBGHZ_REPLAY,       HOME_COMPANION},
  {"RFID / NFC",        Feature::RFID,                HOME_COMPANION},
  {"IR Record",         Feature::IR,                  HOME_COMPANION},
  {"GPS",               Feature::GPS,                 HOME_COMPANION},
};
static const int MENU_N = sizeof(MENU) / sizeof(MENU[0]);
static int sel = 0;

static void startFeature(int i) {
  if (MENU[i].home == HOME_P4) {
    // TODO(P4): run natively on the C6 (esp_wifi/NimBLE over esp-hosted), not
    // proxied to the companion. Stubbed until the connectivity track lands.
    Serial0.printf("NATIVE(C6) %s (0x%02X) — TODO esp-hosted\n", MENU[i].name, (unsigned)MENU[i].feat);
    return;
  }
  uint8_t p[2] = { (uint8_t)Cmd::FEATURE_START, (uint8_t)MENU[i].feat };
  g_link.sendCmd((uint8_t)MENU[i].feat, p, 2, true);
  Serial0.printf("FEATURE_START(companion) %s (0x%02X)\n", MENU[i].name, (unsigned)MENU[i].feat);
}

static void drawMenu() {
  auto& d = gfx();
  d.fillScreen(uic::BLACK);
  d.setTextColor(uic::ORANGE, uic::BLACK);
  d.setTextSize(2);
  d.setCursor(L.pad, L.pad);
  d.print("ESP32-DIV / Tab5");
  // status bar
  d.setTextSize(1);
  d.setTextColor(g_link.linked() ? uic::GREEN : uic::GRAY, uic::BLACK);
  d.setCursor(L.pad, L.titleH);
  d.printf("link:%s rxOk:%u rxErr:%u acks:%u",
           g_link.linked() ? "UP" : "..", (unsigned)g_link.rxOk(),
           (unsigned)g_link.rxErr(), (unsigned)g_link.acks());
  // list
  int y = L.titleH + L.rowH;
  d.setTextSize(2);
  for (int i = 0; i < MENU_N; ++i) {
    bool on = (i == sel);
    d.setTextColor(on ? uic::BLACK : uic::WHITE, on ? uic::ORANGE : uic::BLACK);
    d.fillRect(0, y, L.w, L.rowH, on ? uic::ORANGE : uic::BLACK);
    d.setCursor(L.pad * 2, y + (L.rowH - d.fontHeight()) / 2);
    d.print(MENU[i].name);
    y += L.rowH;
  }
}

static void onEvt(uint8_t chan, divlink::Type t, const uint8_t* p, uint16_t n, void*) {
  if (t == divlink::Type::LOG) { Serial0.write(p, n); Serial0.println(); }
  // EVT/STREAM -> feature screens (Stage-B native rendering) will dispatch here.
  (void)chan; (void)p; (void)n;
}

void setup() {
  Serial0.begin(115200);
  Serial0.println("\n=== TAB5-HOST boot ===");

  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(160);
  L.compute();

  input.begin();
  g_link.begin(&Serial1, LINK_RX, LINK_TX, DIVLINK_BAUD);
  g_link.setEvtHandler(onEvt, nullptr);
  g_link.sendHello();

  drawMenu();
  Serial0.printf("panel %dx%d  PSRAM %u\n", M5.Display.width(), M5.Display.height(),
                 (unsigned)ESP.getPsramSize());
  Serial0.println("HOST_BOOT_OK");   // esp-emu --exit-on sentinel
}

void loop() {
  M5.update();
  g_link.poll();

  InputEventOut ev = input.poll();
  if (ev.has && ev.kind == InputKind::TOUCH) {
    int row = (ev.y - (L.titleH + L.rowH)) / L.rowH;
    if (row >= 0 && row < MENU_N) { sel = row; startFeature(sel); drawMenu(); }
  }

  static uint32_t last = 0;
  if (millis() - last > 500) { last = millis(); drawMenu(); }  // refresh status
  delay(5);
}
