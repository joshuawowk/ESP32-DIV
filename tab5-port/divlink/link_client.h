// link_client.h — host-side divlink transport over an Arduino HardwareSerial
// (UART1 on the M5-Bus "PC" UART G6/G7; §7.1). Provides reliable CMD/ACK with
// retry, a capabilities handshake, and callbacks for telemetry/canvas/log.
#ifndef LINK_CLIENT_H
#define LINK_CLIENT_H

#include <Arduino.h>
#include "divlink.h"
#include "divproto.h"

class LinkClient {
public:
  typedef void (*EvtCb)(uint8_t chan, divlink::Type t, const uint8_t* p, uint16_t n, void* u);

  void begin(HardwareSerial* ser, int rxPin, int txPin, uint32_t baud);
  void poll();                 // pump RX + service CMD retries (call often)

  bool linked() const { return linked_; }
  const divproto::Caps& caps() const { return caps_; }

  void setEvtHandler(EvtCb cb, void* user) { evt_cb_ = cb; evt_user_ = user; }

  void sendHello();
  uint16_t sendCmd(uint8_t chan, const uint8_t* payload, uint16_t len, bool reliable = true);
  void sendInput(const divproto::InputEvent& ev);

  // Diagnostics for the UI status bar.
  uint32_t rxOk()   const { return parser_.frames_ok(); }
  uint32_t rxErr()  const { return parser_.frames_crc_err() + parser_.frames_cobs_err(); }
  uint32_t txRetries() const { return tx_retries_; }
  uint32_t acks()   const { return acks_; }
  uint32_t reliableDowngrades() const { return reliable_downgrades_; }

private:
  static const int MAX_INFLIGHT = 8;
  struct Inflight { bool used; uint16_t seq; uint32_t t_ms; uint8_t tries;
                    uint8_t chan; uint16_t len; uint8_t buf[64]; };

  void write_frame(divlink::Type t, uint8_t chan, uint16_t seq,
                   const uint8_t* p, uint16_t n);
  void onFrame(const divlink::Frame& f);
  static void frameThunk(const divlink::Frame& f, void* u);

  HardwareSerial* ser_ = nullptr;
  divlink::Parser parser_;
  divproto::Caps  caps_{};
  bool     linked_ = false;
  uint16_t seq_ = 1;
  Inflight inflight_[MAX_INFLIGHT] {};
  uint32_t tx_retries_ = 0, acks_ = 0, reliable_downgrades_ = 0;
  uint32_t last_rx_ms_ = 0, last_hello_ms_ = 0;
  EvtCb    evt_cb_ = nullptr; void* evt_user_ = nullptr;
};

#endif // LINK_CLIENT_H
