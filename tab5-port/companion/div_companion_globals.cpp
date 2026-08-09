// div_companion_globals.cpp — real definitions of the link-sourced input globals
// the DIV_COMPANION seam writes and the DIV feature input readers consume. Kept
// in a compiled unit (not just INTEGRATION.md) so the seam links standalone.
// (Adversarial-review B1.)
#ifdef DIV_COMPANION
volatile int  g_linkBtn      = 0;      // divproto::Btn value, 0 = none
volatile bool g_linkBtnDown  = false;
volatile int  g_linkTouchX   = 0;
volatile int  g_linkTouchY   = 0;
volatile bool g_linkTouch    = false;
#endif
