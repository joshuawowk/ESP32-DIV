#pragma once
// Shared NimBLE host bring-up for the Tab5 (ESP32-P4) BLE features. The host is
// initialized once (hostedInitBLE + nimble_port_init + host task) and reused by
// both the scanner (tab5_ble_scan.cpp) and the advertising features
// (tab5_ble_adv.cpp) so NimBLE is never double-initialized.
#if defined(BOARD_TAB5)
#include <stdint.h>

// Bring up the C6 BT controller (esp-hosted) + NimBLE host if not already up.
// Safe to call repeatedly. Returns false if bring-up failed.
bool tab5BleEnsureHost();

// True once the NimBLE host has synced with the controller (address ready).
bool tab5BleSynced();

// own_addr_type inferred at sync (for scanning). Advertising features set their
// own random address and use BLE_OWN_ADDR_RANDOM directly.
uint8_t tab5BleOwnAddrType();
#endif
