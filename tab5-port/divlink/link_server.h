// link_server.h — companion-side (ESP32-DIV S3) divlink endpoint. Transport-
// agnostic: the caller supplies a byte-sink (UART write) so this compiles and
// unit-tests on the host with no Arduino dependency. Handles the HELLO->CAPS
// handshake, auto-ACKs reliable CMDs, dispatches to a feature handler, and
// provides EVT/STREAM/CANVAS/LOG senders. See PORTING_PLAN.md §7, §10.
#ifndef LINK_SERVER_H
#define LINK_SERVER_H

#include "divlink.h"
#include "divproto.h"

class LinkServer {
public:
  // sink(bytes,len,user) transmits raw wire bytes to the host.
  typedef void (*Sink)(const uint8_t* data, size_t len, void* user);
  // Feature command handler: return true to ACK, false to NAK(reason).
  typedef bool (*CmdHandler)(uint8_t chan, const uint8_t* payload, uint16_t len,
                             divlink::Nak* nak_reason, void* user);
  // Reverse input from the host (Stage-A mirror).
  typedef void (*InputHandler)(const divproto::InputEvent& ev, void* user);

  void begin(Sink sink, void* sink_user, const divproto::Caps& caps);
  void setCmdHandler(CmdHandler h, void* u) { cmd_cb_ = h; cmd_user_ = u; }
  void setInputHandler(InputHandler h, void* u) { in_cb_ = h; in_user_ = u; }

  void feed(const uint8_t* data, size_t len) { parser_.feed(data, len); }

  // Telemetry / bulk senders (best-effort unless noted).
  void sendEvt(uint8_t chan, const uint8_t* p, uint16_t n)    { tx(divlink::Type::EVT, chan, 0, p, n); }
  void sendStream(uint8_t chan, const uint8_t* p, uint16_t n) { tx(divlink::Type::STREAM, chan, 0, p, n); }
  void sendCanvas(const uint8_t* p, uint16_t n)              { tx(divlink::Type::CANVAS, 0, 0, p, n); }
  void sendLog(const char* s);

  uint32_t rxOk()  const { return parser_.frames_ok(); }
  uint32_t rxErr() const { return parser_.frames_crc_err() + parser_.frames_cobs_err(); }

private:
  void onFrame(const divlink::Frame& f);
  static void thunk(const divlink::Frame& f, void* u);
  void tx(divlink::Type t, uint8_t chan, uint16_t seq, const uint8_t* p, uint16_t n);

  divlink::Parser  parser_;
  divproto::Caps   caps_{};
  // Per-channel last-accepted CMD seq, for at-most-once dispatch on retransmit.
  uint16_t last_seq_[256] {};
  uint8_t  seen_[256] {};
  Sink   sink_ = nullptr;     void* sink_user_ = nullptr;
  CmdHandler cmd_cb_ = nullptr; void* cmd_user_ = nullptr;
  InputHandler in_cb_ = nullptr; void* in_user_ = nullptr;
};

#endif // LINK_SERVER_H
