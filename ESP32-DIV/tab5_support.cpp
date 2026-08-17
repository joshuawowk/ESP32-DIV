#if defined(BOARD_TAB5)
#include "config.h"
#include "shared.h"
#include "Touchscreen.h"
#include "tab5_support.h"

extern TFT_eSPI tft;

void tab5Unsupported(const char* name) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(name, TFT_WIDTH / 2, TFT_HEIGHT / 2 - 46, 4);
  tft.drawString("Needs radio hardware not", TFT_WIDTH / 2, TFT_HEIGHT / 2 - 6, 2);
  tft.drawString("available on the Tab5 (C6)", TFT_WIDTH / 2, TFT_HEIGHT / 2 + 14, 2);
  tft.drawString("Tap to go back", TFT_WIDTH / 2, TFT_HEIGHT / 2 + 50, 2);
  tft.setTextDatum(TL_DATUM);

  // The tap that selected this feature may still be held. Wait for it to
  // release (level), then for a fresh tap (rising edge), so the notice is read
  // and dismissed deliberately. isTouchDown* is level; readTouchXY is now
  // edge-triggered (see Touchscreen.cpp), so it fires only on a new tap.
  int x = 0, y = 0;
  while (isTouchDownDismiss()) delay(20);   // release of the selecting tap
  while (!readTouchXY(x, y)) delay(20);     // fresh dismiss tap
  feature_exit_requested = true;
}
#endif
