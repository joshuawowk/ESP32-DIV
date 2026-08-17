#include "link_client.h"

using namespace divlink;
using namespace divproto;

static const uint32_t CMD_TIMEOUT_MS = 60;
static const uint8_t  CMD_MAX_TRIES  = 4;
static const uint32_t LINK_TIMEOUT_MS = 3000;  // no RX -> mark link down
static const uint32_t HELLO_RETRY_MS  = 1000;  // re-HELLO cadence while down

void LinkClient::begin(HardwareSerial* ser, int rxPin, int txPin, uint32_t baud) {
  ser_ = ser;
  ser_->begin(baud, SERIAL_8N1, rxPin, txPin);
  parser_.on_frame(&LinkClient::frameThunk, this);
}

void LinkClient::frameThunk(const Frame& f, void* u) {
  static_cast<LinkClient*>(u)->onFrame(f);
}

void LinkClient::write_frame(Type t, uint8_t chan, uint16_t seq,
                             const uint8_t* p, uint16_t n) {
  if (!ser_) return;
  uint8_t wire[MAX_WIRE];
  size_t w = encode_frame(t, chan, seq, p, n, wire, sizeof(wire));
  if (w) ser_->write(wire, w);
}

void LinkClient::sendHello() {
  uint8_t op = (uint8_t)Cmd::HELLO;
  sendCmd(0, &op, 1, true);
  last_hello_ms_ = millis();
}

uint16_t LinkClient::sendCmd(uint8_t chan, const uint8_t* payload, uint16_t len, bool reliable) {
  uint16_t s = seq_++;
  if (seq_ == 0) seq_ = 1;
  write_frame(Type::CMD, chan, s, payload, len);
  if (!reliable) return s;
  // Reliability requires tracking the frame for retransmit. If the payload is
  // too large for the inflight buffer or the table is full, the frame is still
  // sent once (best-effort) but we return 0 so the caller knows reliability was
  // NOT guaranteed. (Review M1 — no more silent downgrade.)
  if (len > sizeof(inflight_[0].buf)) { reliable_downgrades_++; return 0; }
  for (int i = 0; i < MAX_INFLIGHT; ++i) {
    if (!inflight_[i].used) {
      inflight_[i].used = true; inflight_[i].seq = s; inflight_[i].t_ms = millis();
      inflight_[i].tries = 1; inflight_[i].chan = chan; inflight_[i].len = len;
      for (uint16_t k = 0; k < len; ++k) inflight_[i].buf[k] = payload[k];
      return s;
    }
  }
  reliable_downgrades_++;   // table full
  return 0;
}

void LinkClient::sendInput(const InputEvent& ev) {
  write_frame(Type::INPUT_EV, 0, 0, (const uint8_t*)&ev, sizeof(ev));  // best-effort
}

void LinkClient::onFrame(const Frame& f) {
  last_rx_ms_ = millis();   // any valid frame = link alive (heartbeat, Review L3)
  switch (f.type) {
    case Type::CAPS:
      if (f.len >= sizeof(Caps)) { memcpy(&caps_, f.payload, sizeof(Caps)); linked_ = true; }
      break;
    case Type::ACK:
      for (int i = 0; i < MAX_INFLIGHT; ++i)
        if (inflight_[i].used && inflight_[i].seq == f.seq) { inflight_[i].used = false; acks_++; break; }
      break;
    case Type::NAK:
      for (int i = 0; i < MAX_INFLIGHT; ++i)
        if (inflight_[i].used && inflight_[i].seq == f.seq) { inflight_[i].used = false; break; }
      break;
    case Type::EVT: case Type::STREAM: case Type::CANVAS: case Type::BLOB: case Type::LOG:
      if (evt_cb_) evt_cb_(f.chan, f.type, f.payload, f.len, evt_user_);
      break;
    default: break;
  }
}

void LinkClient::poll() {
  if (!ser_) return;
  // Drain RX.
  uint8_t tmp[256];
  int avail;
  while ((avail = ser_->available()) > 0) {
    int n = ser_->readBytes(tmp, avail > (int)sizeof(tmp) ? (int)sizeof(tmp) : avail);
    if (n > 0) parser_.feed(tmp, n);
    if (n < (int)sizeof(tmp)) break;
  }
  // Service reliable-CMD retries/timeouts.
  uint32_t now = millis();
  for (int i = 0; i < MAX_INFLIGHT; ++i) {
    if (!inflight_[i].used) continue;
    if (now - inflight_[i].t_ms < CMD_TIMEOUT_MS) continue;
    if (inflight_[i].tries >= CMD_MAX_TRIES) { inflight_[i].used = false; continue; }
    inflight_[i].tries++; inflight_[i].t_ms = now; tx_retries_++;
    write_frame(Type::CMD, inflight_[i].chan, inflight_[i].seq, inflight_[i].buf, inflight_[i].len);
  }

  // Link liveness: drop `linked_` after silence, and re-HELLO until CAPS returns
  // (survives a companion reboot). (Review L3.)
  if (linked_ && (now - last_rx_ms_) > LINK_TIMEOUT_MS) linked_ = false;
  if (!linked_ && (now - last_hello_ms_) > HELLO_RETRY_MS) sendHello();
}
