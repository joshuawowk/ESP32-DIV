// divproto.h — application layer over divlink: the ESP32-DIV feature registry,
// command opcodes, and telemetry structs shared by the Tab5 host and the S3
// companion. Wire structs are packed and little-endian (both MCUs are LE).
//
// Transport/framing lives in divlink.{h,cpp}; this file defines *what* the
// CMD/EVT/STREAM payloads mean. See PORTING_PLAN.md §5 and §7.3.
#ifndef DIVPROTO_H
#define DIVPROTO_H

#include <stdint.h>

// Defensive: some radio libraries (#include'd on the companion) define these as
// object-like macros, which would break the enum definitions below (an enum
// class does NOT shield its enumerators from the preprocessor). (Review L1.)
#ifdef IR
#undef IR
#endif
#ifdef GPS
#undef GPS
#endif
#ifdef TOUCH
#undef TOUCH
#endif

namespace divproto {

// Every ESP32-DIV feature, grouped by radio. Sent as the first byte of a
// FEATURE_START command payload; also the `chan` for that feature's session.
enum class Feature : uint8_t {
  NONE = 0,
  // --- Wi-Fi (native on the companion S3; some also Tab5-C6-native) ---
  WIFI_SCAN = 0x10, WIFI_PACKET_MONITOR, WIFI_BEACON_SPAM, WIFI_DEAUTH,
  WIFI_DEAUTH_DETECT, WIFI_CAPTIVE_PORTAL, WIFI_PROBE_FLOOD, WIFI_HIDDEN_SSID,
  WIFI_WPS_SCAN, WIFI_ARP_SCAN, WIFI_KARMA,
  // --- BLE (companion NimBLE primary) ---
  BLE_SCAN = 0x30, BLE_SNIFF, BLE_SPOOF, BLE_SOUR_APPLE, BLE_JAMMER /*nRF24*/,
  BLE_AIRTAG_SPOOF, BLE_AIRTAG_SNIFF, BLE_SKIMMER, BLE_DUCKY,
  // --- 2.4 GHz nRF24 ---
  NRF_SCANNER = 0x50, NRF_PROTOKILL, NRF_ESB_SNIFF, NRF_ESB_REPLAY,
  NRF_MOUSEJACK, NRF_MOUSEJACK_INJECT,
  // --- Sub-GHz CC1101 ---
  SUBGHZ_REPLAY = 0x70, SUBGHZ_SAVE, SUBGHZ_JAMMER, SUBGHZ_BRUTE, SUBGHZ_JAMDET,
  // --- Other ---
  RFID = 0x90, IR = 0x92, GPS = 0x94,
};

// CMD opcodes (payload byte 0). Feature-specific params follow.
enum class Cmd : uint8_t {
  HELLO = 0x01,        // -> CAPS (fw ver + feature bitmap)
  FEATURE_START = 0x02,// [Feature][params...]
  FEATURE_STOP  = 0x03,// []
  SET_PARAM     = 0x04,// [key][value...] (tune active feature)
  BLOB_FETCH    = 0x05,// [path_len][path][u32 off][u32 len] -> BLOB chunks
  PING          = 0x06,
  // Mirror (Stage-A) control:
  CANVAS_CFG    = 0x10,// [u16 w][u16 h][u8 scale][u8 bpp]
};

// ---- Handshake -------------------------------------------------------------
struct __attribute__((packed)) Caps {
  uint8_t  proto_ver;      // divlink::PROTO_VER
  uint16_t fw_major_minor; // companion DIV firmware version
  uint8_t  has_psram;      // 1 if the companion can run the Stage-A mirror
  uint32_t feature_bitmap0;// which radios are wired/present
  uint32_t feature_bitmap1;
};

// ---- Reverse input (host -> companion, Stage-A mirror) ---------------------
enum class InputKind : uint8_t { BTN_DOWN = 1, BTN_UP = 2, TOUCH = 3 };
enum class Btn : uint8_t { UP = 1, DOWN, LEFT, RIGHT, SELECT, BACK };
struct __attribute__((packed)) InputEvent {
  uint8_t  kind;   // InputKind
  uint8_t  btn;    // Btn (when kind is BTN_*)
  uint16_t x, y;   // touch coords (when kind==TOUCH)
  uint8_t  pressed;
};

// ---- Canvas tile (companion -> host, Stage-A mirror) -----------------------
// Followed by RLE-encoded RGB565 pixels for the tile rectangle.
struct __attribute__((packed)) CanvasTile {
  uint16_t x, y, w, h;
  uint16_t rle_len;   // bytes of RLE payload that follow this header
};

// ---- Telemetry (companion -> host, native Stage-B rendering) ---------------
struct __attribute__((packed)) WifiAp {
  uint8_t  bssid[6];
  int8_t   rssi;
  uint8_t  channel;
  uint8_t  enc;        // 0 open,1 wep,2 wpa,3 wpa2,4 wpa3
  uint8_t  ssid_len;
  char     ssid[33];
};
struct __attribute__((packed)) BleDev {
  uint8_t  addr[6];
  int8_t   rssi;
  uint8_t  name_len;
  char     name[29];
};
struct __attribute__((packed)) GpsFix {
  int32_t  lat_e7, lon_e7;  // degrees * 1e7
  uint16_t sats;
  uint16_t speed_cms;       // cm/s
  uint32_t utc;             // epoch seconds
  uint8_t  fix_valid;
};
struct __attribute__((packed)) RssiFrame {  // sub-GHz / nRF spectrum row
  uint32_t freq_hz;
  uint8_t  n;               // number of bins that follow
  // int8_t rssi[n] follows
};
struct __attribute__((packed)) PacketStat { // packet-monitor waterfall row
  uint8_t  channel;
  uint16_t mgmt, data, ctrl;
  uint32_t dropped;         // link/companion drop counter (backpressure)
};

} // namespace divproto
#endif // DIVPROTO_H
