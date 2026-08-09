#include "link_server.h"
#include <string.h>

using namespace divlink;
using namespace divproto;

void LinkServer::begin(Sink sink, void* sink_user, const Caps& caps) {
  sink_ = sink; sink_user_ = sink_user; caps_ = caps;
  parser_.on_frame(&LinkServer::thunk, this);
}

void LinkServer::thunk(const Frame& f, void* u) {
  static_cast<LinkServer*>(u)->onFrame(f);
}

void LinkServer::tx(Type t, uint8_t chan, uint16_t seq, const uint8_t* p, uint16_t n) {
  if (!sink_) return;
  uint8_t wire[MAX_WIRE];
  size_t w = encode_frame(t, chan, seq, p, n, wire, sizeof(wire));
  if (w) sink_(wire, w, sink_user_);
}

void LinkServer::sendLog(const char* s) {
  uint16_t n = 0; while (s[n] && n < 240) ++n;
  tx(Type::LOG, 0, 0, (const uint8_t*)s, n);
}

void LinkServer::onFrame(const Frame& f) {
  switch (f.type) {
    case Type::CMD: {
      // HELLO is answered with CAPS (and ACK'd) — idempotent, always re-served.
      bool is_hello = (f.len >= 1 && f.payload[0] == (uint8_t)Cmd::HELLO);
      if (is_hello) {
        tx(Type::CAPS, f.chan, f.seq, (const uint8_t*)&caps_, sizeof(caps_));
        tx(Type::ACK, f.chan, f.seq, nullptr, 0);
        seen_[f.chan] = 1; last_seq_[f.chan] = f.seq;
        return;
      }
      // At-most-once: a retransmitted CMD (same seq/chan) is re-ACKed but NOT
      // re-dispatched, so side-effecting commands don't run twice. (Review H2.)
      if (seen_[f.chan] && last_seq_[f.chan] == f.seq) {
        tx(Type::ACK, f.chan, f.seq, nullptr, 0);
        return;
      }
      Nak reason = Nak::UNKNOWN_CMD;
      bool ok = cmd_cb_ ? cmd_cb_(f.chan, f.payload, f.len, &reason, cmd_user_) : false;
      if (ok) {
        tx(Type::ACK, f.chan, f.seq, nullptr, 0);
        seen_[f.chan] = 1; last_seq_[f.chan] = f.seq;
      } else { uint8_t r = (uint8_t)reason; tx(Type::NAK, f.chan, f.seq, &r, 1); }
      break;
    }
    case Type::INPUT_EV:
      if (in_cb_ && f.len >= sizeof(InputEvent)) {
        InputEvent ev; memcpy(&ev, f.payload, sizeof(ev)); in_cb_(ev, in_user_);
      }
      break;
    case Type::HEARTBEAT: default: break;
  }
}
