// test_divlink_native.cpp — host-native unit tests for the divlink protocol.
// Build:  g++ -std=c++11 -O2 -Wall -Wextra -I../divlink test_divlink_native.cpp ../divlink/divlink.cpp -o /tmp/divlink_test
// Run:    /tmp/divlink_test
#include "divlink.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

using namespace divlink;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, msg) do { \
  if (cond) { ++g_pass; } \
  else { ++g_fail; std::printf("  FAIL: %s (line %d)\n", msg, __LINE__); } \
} while (0)

// --- capture sink for parser callbacks --------------------------------------
struct Capture {
  std::vector<std::vector<uint8_t>> payloads;
  std::vector<Type> types;
  std::vector<uint8_t> chans;
  std::vector<uint16_t> seqs;
};
static void on_frame(const Frame& f, void* user) {
  Capture* c = (Capture*)user;
  c->types.push_back(f.type);
  c->chans.push_back(f.chan);
  c->seqs.push_back(f.seq);
  c->payloads.push_back(std::vector<uint8_t>(f.payload, f.payload + f.len));
}

// ---------------------------------------------------------------------------
static void test_crc_vector() {
  std::printf("[crc16] CCITT-FALSE known vector\n");
  const char* s = "123456789";
  uint16_t c = crc16((const uint8_t*)s, 9);
  CHECK(c == 0x29B1, "CRC-16/CCITT-FALSE(\"123456789\") == 0x29B1");
}

static void test_cobs_roundtrip() {
  std::printf("[cobs] round-trip incl. zeros and long runs\n");
  std::vector<std::vector<uint8_t>> cases;
  cases.push_back({});                                   // empty
  cases.push_back({0});                                  // single zero
  cases.push_back({1,2,3});                              // no zeros
  cases.push_back({0,0,0});                              // all zeros
  cases.push_back({1,0,2,0,3});                          // interleaved
  std::vector<uint8_t> big;                              // > 254 non-zero
  for (int i = 0; i < 600; ++i) big.push_back((uint8_t)(1 + (i % 254)));
  cases.push_back(big);
  std::vector<uint8_t> mixed;                            // zeros inside a long run
  for (int i = 0; i < 600; ++i) mixed.push_back((uint8_t)(i % 256));
  cases.push_back(mixed);

  for (size_t k = 0; k < cases.size(); ++k) {
    const auto& in = cases[k];
    std::vector<uint8_t> enc(in.size() + in.size()/254 + 2);
    size_t en = cobs_encode(in.empty()?(const uint8_t*)"":in.data(), in.size(), enc.data(), enc.size());
    // COBS output must never contain a zero byte.
    bool has_zero = false;
    for (size_t i = 0; i < en; ++i) if (enc[i] == 0) has_zero = true;
    CHECK(!has_zero, "COBS output contains no 0x00");
    std::vector<uint8_t> dec(in.size() + 4);
    size_t dn = cobs_decode(enc.data(), en, dec.data(), dec.size());
    CHECK(dn == in.size(), "COBS decode length matches");
    CHECK(in.empty() || memcmp(dec.data(), in.data(), in.size()) == 0, "COBS decode bytes match");
  }
}

static void test_frame_roundtrip() {
  std::printf("[frame] encode -> parse round-trip (empty/small/max)\n");
  size_t sizes[] = {0, 1, 7, 64, 512, MAX_PAYLOAD};
  for (size_t si = 0; si < sizeof(sizes)/sizeof(sizes[0]); ++si) {
    uint16_t n = (uint16_t)sizes[si];
    std::vector<uint8_t> pay(n);
    for (uint16_t i = 0; i < n; ++i) pay[i] = (uint8_t)(i * 7 + 1);  // includes zeros
    uint8_t wire[MAX_WIRE];
    size_t w = encode_frame(Type::CMD, 5, 0xBEEF, n?pay.data():nullptr, n, wire, sizeof(wire));
    CHECK(w > 0, "encode_frame succeeds");
    CHECK(wire[w-1] == 0x00, "frame ends with delimiter");
    Capture cap; Parser p; p.on_frame(on_frame, &cap);
    p.feed(wire, w);
    CHECK(cap.payloads.size() == 1, "exactly one frame parsed");
    if (cap.payloads.size() == 1) {
      CHECK(cap.types[0] == Type::CMD, "type preserved");
      CHECK(cap.chans[0] == 5, "chan preserved");
      CHECK(cap.seqs[0] == 0xBEEF, "seq preserved");
      CHECK(cap.payloads[0].size() == n, "payload length preserved");
      CHECK(n == 0 || memcmp(cap.payloads[0].data(), pay.data(), n) == 0, "payload bytes preserved");
    }
    CHECK(p.frames_ok() == 1 && p.frames_crc_err() == 0, "counters: 1 ok, 0 err");
  }
}

static void test_crc_corruption() {
  std::printf("[frame] CRC corruption is rejected\n");
  uint8_t pay[16]; for (int i=0;i<16;++i) pay[i]=(uint8_t)i;
  uint8_t wire[MAX_WIRE];
  size_t w = encode_frame(Type::EVT, 1, 42, pay, 16, wire, sizeof(wire));
  // Flip a bit in the middle of the encoded frame (not the delimiter).
  wire[w/2] ^= 0x40;
  Capture cap; Parser p; p.on_frame(on_frame, &cap);
  p.feed(wire, w);
  CHECK(cap.payloads.empty(), "corrupted frame not delivered");
  CHECK(p.frames_ok() == 0, "no ok frames");
  CHECK(p.frames_crc_err() + p.frames_cobs_err() == 1, "one crc/cobs error counted");
}

static void test_resync_after_garbage() {
  std::printf("[frame] resync after garbage + partial frame\n");
  Capture cap; Parser p; p.on_frame(on_frame, &cap);
  // garbage bytes, then a delimiter, then a valid frame
  uint8_t garbage[] = {0x11,0x22,0x33,0x44,0x55};
  p.feed(garbage, sizeof(garbage));
  uint8_t z = 0x00; p.feed(&z, 1);                 // delimiter flushes garbage
  const char* msg = "hello";
  uint8_t wire[MAX_WIRE];
  size_t w = encode_frame(Type::LOG, 0, 1, (const uint8_t*)msg, 5, wire, sizeof(wire));
  p.feed(wire, w);
  CHECK(cap.payloads.size() == 1, "valid frame recovered after garbage");
  CHECK(cap.payloads.size()==1 && memcmp(cap.payloads[0].data(), msg, 5)==0, "recovered payload correct");
}

static void test_two_frames_and_bytewise() {
  std::printf("[frame] two frames in one buffer; byte-wise feed equivalence\n");
  uint8_t a[3]={1,2,3}, b[4]={9,8,7,6};
  uint8_t wa[MAX_WIRE], wb[MAX_WIRE];
  size_t na = encode_frame(Type::STREAM, 2, 100, a, 3, wa, sizeof(wa));
  size_t nb = encode_frame(Type::STREAM, 2, 101, b, 4, wb, sizeof(wb));
  std::vector<uint8_t> both; both.insert(both.end(), wa, wa+na); both.insert(both.end(), wb, wb+nb);

  Capture c1; Parser p1; p1.on_frame(on_frame, &c1); p1.feed(both.data(), both.size());
  CHECK(c1.payloads.size() == 2, "two frames from one buffer");

  Capture c2; Parser p2; p2.on_frame(on_frame, &c2);
  for (size_t i=0;i<both.size();++i) p2.feed_byte(both[i]);
  CHECK(c2.payloads.size() == 2, "two frames from byte-wise feed");
  CHECK(c2.seqs.size()==2 && c2.seqs[0]==100 && c2.seqs[1]==101, "seqs in order");
}

// Simulate a CMD/ACK exchange across two endpoints wired back-to-back.
static void test_cmd_ack_exchange() {
  std::printf("[proto] simulated CMD/ACK exchange\n");
  struct Endpoint {
    Parser parser; Capture cap;
    std::vector<uint8_t>* wire_out; // where our TX goes (peer's RX)
  };
  static Endpoint host, comp;
  host.parser.on_frame(on_frame, &host.cap);
  comp.parser.on_frame(on_frame, &comp.cap);

  // Host sends CMD(seq=7) -> companion
  uint8_t buf[MAX_WIRE];
  uint8_t params[2] = {0x06, 0x0B}; // e.g. WIFI_DEAUTH(ch=6, count=11)
  size_t w = encode_frame(Type::CMD, 3, 7, params, 2, buf, sizeof(buf));
  comp.parser.feed(buf, w);
  CHECK(comp.cap.types.size()==1 && comp.cap.types[0]==Type::CMD, "companion got CMD");
  CHECK(comp.cap.seqs[0]==7, "companion sees seq 7");

  // Companion replies ACK(seq=7) -> host
  size_t wa = encode_frame(Type::ACK, 3, 7, nullptr, 0, buf, sizeof(buf));
  host.parser.feed(buf, wa);
  CHECK(host.cap.types.size()==1 && host.cap.types[0]==Type::ACK, "host got ACK");
  CHECK(host.cap.seqs[0]==7, "host sees ack seq 7");
}

int main() {
  std::printf("=== divlink native tests ===\n");
  test_crc_vector();
  test_cobs_roundtrip();
  test_frame_roundtrip();
  test_crc_corruption();
  test_resync_after_garbage();
  test_two_frames_and_bytewise();
  test_cmd_ack_exchange();
  std::printf("=== %d passed, %d failed ===\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
