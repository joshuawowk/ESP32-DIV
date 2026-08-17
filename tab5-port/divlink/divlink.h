// divlink.h — framed binary link between the Tab5 host (ESP32-P4) and the
// ESP32-DIV companion (ESP32-S3). SoC-agnostic C++11; no dynamic allocation on
// the hot path, no Arduino/IDF dependencies in the core (transport is injected).
//
// Wire format (per frame), COBS-encoded and terminated by a single 0x00 byte:
//
//   [ver:1][type:1][chan:1][seq:2 LE][len:2 LE][payload:len][crc16:2 LE]
//   \___________________ CRC-16/CCITT-FALSE covers this ______________/^^^^^
//                                                        (crc excludes itself)
//
// COBS gives self-synchronising framing (0x00 is the delimiter and never occurs
// inside an encoded frame), so a receiver can always resync on the next 0x00.
//
// See PORTING_PLAN.md §7 (the inter-processor link).
#ifndef DIVLINK_H
#define DIVLINK_H

#include <stdint.h>
#include <stddef.h>

namespace divlink {

// ---- Protocol constants ----------------------------------------------------
static const uint8_t  PROTO_VER      = 1;
static const size_t   MAX_PAYLOAD    = 4096;   // blobs are chunked above this
static const size_t   HEADER_LEN     = 7;      // ver+type+chan+seq(2)+len(2)
static const size_t   CRC_LEN        = 2;
static const size_t   MAX_FRAME      = HEADER_LEN + MAX_PAYLOAD + CRC_LEN;
// COBS worst case adds ceil(n/254)+1 overhead bytes, plus the 0x00 delimiter.
static const size_t   MAX_WIRE       = MAX_FRAME + (MAX_FRAME / 254) + 2;

// ---- Frame types -----------------------------------------------------------
enum class Type : uint8_t {
  HELLO     = 0x01,  // capability handshake (either direction)
  CAPS      = 0x02,  // capability reply (feature bitmap + fw version)
  HEARTBEAT = 0x03,  // liveness / link keepalive
  CMD       = 0x10,  // reliable command (expects ACK/NAK, carries seq)
  ACK       = 0x11,  // positive ack for a CMD seq
  NAK       = 0x12,  // negative ack for a CMD seq (payload[0]=reason)
  EVT       = 0x20,  // best-effort telemetry event
  STREAM    = 0x21,  // best-effort high-rate stream (lossy by design)
  BLOB      = 0x30,  // windowed bulk transfer chunk
  CANVAS    = 0x40,  // Stage-A mirror: RLE dirty-tile pixels
  INPUT_EV  = 0x41,  // Stage-A mirror: reverse input events (host->companion)
                     // NB: named INPUT_EV — Arduino defines INPUT as a macro.
  LOG       = 0x50,  // human-readable log line (debug)
};

// NAK reason codes.
enum class Nak : uint8_t {
  BAD_CRC = 1, UNKNOWN_CMD = 2, BAD_PARAM = 3, BUSY = 4, UNSUPPORTED = 5,
};

// A parsed, validated frame. `payload` points into the parser's internal
// buffer and is valid only for the duration of the RxHandler callback.
struct Frame {
  Type     type;
  uint8_t  chan;
  uint16_t seq;
  const uint8_t* payload;
  uint16_t len;
};

// ---- CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) -------------------------
uint16_t crc16(const uint8_t* data, size_t len);

// ---- COBS ------------------------------------------------------------------
// Encodes `len` bytes from `in` into `out` (capacity `out_cap`). COBS worst case
// is len + len/254 + 1 bytes (one overhead byte per 254-run). Returns bytes
// written (NOT including the trailing 0x00 the caller appends), or 0 if out_cap
// is insufficient. Decode with cobs_decode (which is likewise bounds-checked).
size_t cobs_encode(const uint8_t* in, size_t len, uint8_t* out, size_t out_cap);
size_t cobs_decode(const uint8_t* in, size_t len, uint8_t* out, size_t out_cap);

// ---- Encoder ---------------------------------------------------------------
// Builds one wire frame (COBS + trailing 0x00) into `out` (>= MAX_WIRE).
// Returns bytes written, or 0 if payload too large / buffer too small.
size_t encode_frame(Type type, uint8_t chan, uint16_t seq,
                    const uint8_t* payload, uint16_t len,
                    uint8_t* out, size_t out_cap);

// ---- Parser (streaming) ----------------------------------------------------
// Feed received bytes; complete, CRC-valid frames are delivered to the handler.
// Zero-copy: the Frame.payload points into this parser's buffer.
class Parser {
public:
  typedef void (*RxHandler)(const Frame& f, void* user);

  Parser() : wp_(0), overflow_(false), user_(nullptr), handler_(nullptr),
             frames_ok_(0), frames_crc_err_(0), frames_cobs_err_(0),
             frames_overflow_(0) {}

  void on_frame(RxHandler h, void* user) { handler_ = h; user_ = user; }

  // Feed a buffer of received bytes. Frames are dispatched synchronously.
  void feed(const uint8_t* data, size_t len);
  void feed_byte(uint8_t b);

  // Diagnostics (surfaced in the UI per §7.2 backpressure/loss counters).
  uint32_t frames_ok()        const { return frames_ok_; }
  uint32_t frames_crc_err()   const { return frames_crc_err_; }
  uint32_t frames_cobs_err()  const { return frames_cobs_err_; }
  uint32_t frames_overflow()  const { return frames_overflow_; }
  void reset() { wp_ = 0; overflow_ = false; }

private:
  void end_of_frame();

  uint8_t  acc_[MAX_WIRE];   // accumulates one COBS block (up to a 0x00)
  size_t   wp_;
  bool     overflow_;
  uint8_t  decoded_[MAX_FRAME];
  void*    user_;
  RxHandler handler_;
  uint32_t frames_ok_, frames_crc_err_, frames_cobs_err_, frames_overflow_;
};

} // namespace divlink
#endif // DIVLINK_H
