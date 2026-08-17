// divlink.cpp — implementation. See divlink.h.
#include "divlink.h"

namespace divlink {

// ---- CRC-16/CCITT-FALSE ----------------------------------------------------
uint16_t crc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= (uint16_t)data[i] << 8;
    for (int b = 0; b < 8; ++b) {
      if (crc & 0x8000) crc = (uint16_t)((crc << 1) ^ 0x1021);
      else              crc = (uint16_t)(crc << 1);
    }
  }
  return crc;
}

// ---- COBS ------------------------------------------------------------------
// Classic Consistent Overhead Byte Stuffing. Output contains no 0x00 bytes.
size_t cobs_encode(const uint8_t* in, size_t len, uint8_t* out, size_t out_cap) {
  if (out_cap < 1) return 0;
  size_t rp = 0, wp = 0;
  size_t code_idx = wp++;      // reserve slot for the first code byte
  uint8_t code = 1;
  while (rp < len) {
    if (in[rp] == 0) {
      out[code_idx] = code;
      if (wp >= out_cap) return 0;
      code_idx = wp++;
      code = 1;
    } else {
      if (wp >= out_cap) return 0;
      out[wp++] = in[rp];
      if (++code == 0xFF) {    // 254 non-zero bytes: emit and restart
        out[code_idx] = code;
        if (wp >= out_cap) return 0;
        code_idx = wp++;
        code = 1;
      }
    }
    ++rp;
  }
  out[code_idx] = code;        // code_idx was reserved in-bounds
  return wp;
}

size_t cobs_decode(const uint8_t* in, size_t len, uint8_t* out, size_t out_cap) {
  size_t rp = 0, wp = 0;
  while (rp < len) {
    uint8_t code = in[rp++];
    if (code == 0) return 0;                     // stray delimiter: malformed
    for (uint8_t i = 1; i < code; ++i) {
      if (rp >= len || wp >= out_cap) return 0;  // truncated / overflow
      out[wp++] = in[rp++];
    }
    if (code < 0xFF && rp < len) {               // implicit zero (not at end)
      if (wp >= out_cap) return 0;
      out[wp++] = 0;
    }
  }
  return wp;
}

// ---- Encoder ---------------------------------------------------------------
size_t encode_frame(Type type, uint8_t chan, uint16_t seq,
                    const uint8_t* payload, uint16_t len,
                    uint8_t* out, size_t out_cap) {
  if (len > MAX_PAYLOAD) return 0;
  uint8_t raw[MAX_FRAME];
  size_t n = 0;
  raw[n++] = PROTO_VER;
  raw[n++] = (uint8_t)type;
  raw[n++] = chan;
  raw[n++] = (uint8_t)(seq & 0xFF);
  raw[n++] = (uint8_t)(seq >> 8);
  raw[n++] = (uint8_t)(len & 0xFF);
  raw[n++] = (uint8_t)(len >> 8);
  for (uint16_t i = 0; i < len; ++i) raw[n++] = payload ? payload[i] : 0;
  uint16_t c = crc16(raw, n);
  raw[n++] = (uint8_t)(c & 0xFF);
  raw[n++] = (uint8_t)(c >> 8);

  if (out_cap < n + (n / 254) + 2) return 0;
  size_t w = cobs_encode(raw, n, out, out_cap);
  if (w == 0 || w >= out_cap) return 0;
  out[w++] = 0x00;   // frame delimiter
  return w;
}

// ---- Parser ----------------------------------------------------------------
void Parser::feed(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; ++i) feed_byte(data[i]);
}

void Parser::feed_byte(uint8_t b) {
  if (b == 0x00) {
    end_of_frame();
    wp_ = 0;
    overflow_ = false;
    return;
  }
  if (wp_ < sizeof(acc_)) {
    acc_[wp_++] = b;
  } else {
    overflow_ = true;   // block too long; will be dropped at delimiter
  }
}

void Parser::end_of_frame() {
  if (wp_ == 0) return;                 // empty (back-to-back delimiters)
  if (overflow_) { ++frames_overflow_; return; }

  size_t dn = cobs_decode(acc_, wp_, decoded_, sizeof(decoded_));
  if (dn < HEADER_LEN + CRC_LEN) { ++frames_cobs_err_; return; }

  size_t body = dn - CRC_LEN;
  uint16_t got = (uint16_t)(decoded_[dn - 2] | (decoded_[dn - 1] << 8));
  uint16_t want = crc16(decoded_, body);
  if (got != want) { ++frames_crc_err_; return; }

  uint16_t len = (uint16_t)(decoded_[5] | (decoded_[6] << 8));
  if ((size_t)len + HEADER_LEN != body) { ++frames_crc_err_; return; }  // length mismatch
  if (decoded_[0] != PROTO_VER) { ++frames_crc_err_; return; }          // version gate

  ++frames_ok_;
  if (!handler_) return;
  Frame f;
  f.type    = (Type)decoded_[1];
  f.chan    = decoded_[2];
  f.seq     = (uint16_t)(decoded_[3] | (decoded_[4] << 8));
  f.payload = (len ? &decoded_[HEADER_LEN] : nullptr);
  f.len     = len;
  handler_(f, user_);
}

} // namespace divlink
