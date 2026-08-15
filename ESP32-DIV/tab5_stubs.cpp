// -----------------------------------------------------------------------------
// tab5_stubs.cpp  (AUTO-GENERATED for the M5Stack Tab5 / ESP32-P4 port)
//
// Provides feature entrypoints for the hardware-heavy modules still excluded
// from the Tab5 build, so the firmware links. Each shows an exitable
// "not available on Tab5" screen. Modules that get re-included (their .cpp
// added back to the build) are removed from here to avoid duplicate symbols.
// -----------------------------------------------------------------------------
#include "config.h"
#include "shared.h"
#include "Touchscreen.h"

extern TFT_eSPI tft;

static void drawUnavailable(const char* name) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(name, TFT_WIDTH / 2, TFT_HEIGHT / 2 - 40, 4);
  tft.drawString("Not available on Tab5 hardware", TFT_WIDTH / 2, TFT_HEIGHT / 2 + 4, 2);
  tft.drawString("Tap to go back", TFT_WIDTH / 2, TFT_HEIGHT / 2 + 44, 2);
  tft.setTextDatum(TL_DATUM);
}

static void unavailableLoop() {
  int x = 0, y = 0;
  if (readTouchXY(x, y)) feature_exit_requested = true;
  delay(30);
}
namespace AirTagSniffer {
  void airTagSnifferLoop() { unavailableLoop(); }
  void airTagSnifferSetup() { drawUnavailable("AirTag Sniffer"); }
  void exit() {}
}

namespace BleJammer {
  void blejamLoop() { unavailableLoop(); }
  void blejamSetup() { drawUnavailable("BLE Jammer"); }
  void exit() {}
}

namespace BleSkimmer {
  void bleSkimmerLoop() { unavailableLoop(); }
  void bleSkimmerSetup() { drawUnavailable("BLE Skimmer"); }
  void exit() {}
}

namespace BleSniffer {
  void blesnifferLoop() { unavailableLoop(); }
  void blesnifferSetup() { drawUnavailable("BLE Sniffer"); }
  void exit() {}
}

namespace EsbReplay {
  void esbReplayLoop() { unavailableLoop(); }
  void esbReplaySetup() { drawUnavailable("ESB Replay"); }
  void exit() {}
}

namespace EsbSniffer {
  void esbSnifferLoop() { unavailableLoop(); }
  void esbSnifferSetup() { drawUnavailable("ESB Sniffer"); }
  void exit() {}
}

namespace GpsSatelliteScanner {
  void session() { drawUnavailable("GPS Sky Plot"); }
}

namespace GpsWardriver {
  void clearSessionRetry() {}
  bool consumeSessionRetry() { return false; }
  void session() { drawUnavailable("GPS Wardriver"); }
  bool statusBarGpsIconActive() { return false; }
  void stopBackgroundIfRunning() {}
}

namespace IRRemoteFeature {
  void loop() { unavailableLoop(); }
  void setup() { drawUnavailable("IR Remote"); }
}

namespace IRSavedProfile {
  void loop() { unavailableLoop(); }
  void setup() { drawUnavailable("IR Saved"); }
}

namespace IRUniversalController {
  void loop() { unavailableLoop(); }
  void setup() { drawUnavailable("IR Universal"); }
}

namespace MouseJack {
  void exit() {}
  void mouseJackLoop() { unavailableLoop(); }
  void mouseJackSetup() { drawUnavailable("MouseJack"); }
}

namespace MouseJackInject {
  void exit() {}
  void mouseJackInjectLoop() { unavailableLoop(); }
  void mouseJackInjectSetup() { drawUnavailable("MouseJack Inject"); }
}

namespace ProtoKill {
  void exit() {}
  void prokillLoop() { unavailableLoop(); }
  void prokillSetup() { drawUnavailable("ProtoKill"); }
}

namespace RfidNfc {
  bool begin() { drawUnavailable("RFID / NFC"); return false; }
  void clearSessionRetry() {}
  bool consumeSessionRetry() { return false; }
  void sessionCardReader() { drawUnavailable("RFID / NFC"); }
  void sessionClone() { drawUnavailable("RFID / NFC"); }
  void sessionDecodeAccess() { drawUnavailable("RFID / NFC"); }
  void sessionDisruptEmulate() { drawUnavailable("RFID / NFC"); }
  void sessionDump() { drawUnavailable("RFID / NFC"); }
  void sessionErase() { drawUnavailable("RFID / NFC"); }
  void sessionJamReader() { drawUnavailable("RFID / NFC"); }
  void sessionTagDisrupt() { drawUnavailable("RFID / NFC"); }
}

namespace SavedProfile {
  void saveLoop() { unavailableLoop(); }
  void saveSetup() { drawUnavailable("SubGHz Saved"); }
}

namespace Scanner {
  void exit() {}
  void scannerLoop() { unavailableLoop(); }
  void scannerSetup() { drawUnavailable("2.4GHz Scanner"); }
}

namespace SubBrute {
  void subBruteLoop() { unavailableLoop(); }
  void subBruteSetup() { drawUnavailable("SubGHz Brute"); }
}

namespace jammingdetector {
  void Loop() { unavailableLoop(); }
  void Setup() { drawUnavailable("Jamming Detector"); }
}

namespace replayat {
  void ReplayAttackLoop() { unavailableLoop(); }
  void ReplayAttackSetup() { drawUnavailable("SubGHz Replay"); }
}

namespace subjammer {
  void subjammerLoop() { unavailableLoop(); }
  void subjammerSetup() { drawUnavailable("SubGHz Jammer"); }
}

