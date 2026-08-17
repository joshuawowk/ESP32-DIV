#pragma once
// Tab5 (ESP32-P4) shared helpers for features that cannot run on this hardware.
#if defined(BOARD_TAB5)
// Draw a full-screen "not available on Tab5" notice, wait for a tap, then
// request that the current feature exit. Use at the start of a feature setup
// that depends on hardware the Tab5 lacks (e.g. WiFi raw injection / promiscuous
// capture over the C6 hosted link).
void tab5Unsupported(const char* name);
#endif
