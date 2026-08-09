// gfx.h — thin façade mapping the DIV firmware's TFT_eSPI-style calls onto
// M5GFX (M5.Display). The DIV code calls `tft.fillScreen(...)`, `tft.drawString`
// etc.; on the Tab5 host those resolve to M5.Display. Text metrics delegate to
// M5GFX (plan §17.3 MR-4 — never the len*6 stub). See §6.4.
#ifndef GFX_H
#define GFX_H

#include <M5Unified.h>

// A reference to the live display so ported DIV drawing code can keep using a
// `tft`-like object. M5.Display is an lgfx device with TFT_eSPI-compatible names
// (fillScreen/fillRoundRect/drawString/setTextColor/setCursor/width/height...).
static inline M5GFX& gfx() { return M5.Display; }

// RGB565 colour helpers mirroring the DIV palette names it uses most.
namespace uic {
  static const uint16_t BLACK = 0x0000, WHITE = 0xFFFF, GRAY = 0x8410;
  static const uint16_t RED = 0xF800, GREEN = 0x07E0, BLUE = 0x001F;
  static const uint16_t ORANGE = 0xFD20, DARKBLUE = 0x3166;
}

// Layout derived from the *actual* panel size/rotation, not hard-coded 240x320.
struct Layout {
  int w, h, pad, rowH, titleH;
  void compute() {
    w = gfx().width(); h = gfx().height();
    pad = w / 40; titleH = gfx().fontHeight() * 2 + pad;
    rowH = gfx().fontHeight() + pad;
    if (rowH < 24) rowH = 24;
  }
};

#endif // GFX_H
