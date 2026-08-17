// test_linkserver_native.cpp — end-to-end host<->companion protocol test.
// The "host" is a raw Encoder/Parser; the "companion" is the real LinkServer.
// Build: g++ -std=c++11 -O2 -Wall -Wextra -I../divlink test_linkserver_native.cpp \
//        ../divlink/divlink.cpp ../divlink/link_server.cpp -o /tmp/linkserver_test
#include "divlink.h"
#include "divproto.h"
#include "link_server.h"
#include <cstdio>
#include <cstring>
#include <vector>

using namespace divlink;
using namespace divproto;

static int g_fail = 0, g_pass = 0;
#define CHECK(c,m) do{ if(c){++g_pass;} else {++g_fail; std::printf("  FAIL: %s (line %d)\n",m,__LINE__);} }while(0)

// The host side: a parser capturing frames from the companion.
struct Host {
  Parser parser;
  std::vector<Type> types; std::vector<uint16_t> seqs;
  std::vector<std::vector<uint8_t>> pays;
  Caps caps{}; bool got_caps=false;
} host;

static void host_rx(const Frame& f, void*) {
  host.types.push_back(f.type); host.seqs.push_back(f.seq);
  host.pays.push_back(std::vector<uint8_t>(f.payload, f.payload+f.len));
  if (f.type==Type::CAPS && f.len>=sizeof(Caps)) { memcpy(&host.caps,f.payload,sizeof(Caps)); host.got_caps=true; }
}

// Companion sink -> feed the host parser (the "wire").
static void comp_sink(const uint8_t* d, size_t n, void*) { host.parser.feed(d, n); }

// Companion command handler: accept FEATURE_START, reject everything else.
static int g_feature_started = -1;
static int g_dispatch_count = 0;   // how many times comp_cmd actually ran
static bool comp_cmd(uint8_t chan, const uint8_t* p, uint16_t n, Nak* nak, void*) {
  (void)chan;
  if (n>=2 && p[0]==(uint8_t)Cmd::FEATURE_START) { g_feature_started = p[1]; g_dispatch_count++; return true; }
  *nak = Nak::UNKNOWN_CMD; return false;
}
static InputEvent g_last_input{}; static bool g_got_input=false;
static void comp_input(const InputEvent& ev, void*) { g_last_input=ev; g_got_input=true; }

// Helper: host sends a CMD to the companion.
static void host_send_cmd(LinkServer& srv, uint8_t chan, uint16_t seq, const uint8_t* p, uint16_t n) {
  uint8_t wire[MAX_WIRE];
  size_t w = encode_frame(Type::CMD, chan, seq, p, n, wire, sizeof(wire));
  srv.feed(wire, w);
}

int main() {
  std::printf("=== linkserver end-to-end tests ===\n");
  host.parser.on_frame(host_rx, nullptr);

  Caps caps{}; caps.proto_ver = PROTO_VER; caps.fw_major_minor = 0x0107; // v1.7
  caps.has_psram = 1; caps.feature_bitmap0 = 0xDEADBEEF;
  LinkServer srv;
  srv.begin(comp_sink, nullptr, caps);
  srv.setCmdHandler(comp_cmd, nullptr);
  srv.setInputHandler(comp_input, nullptr);

  // 1) HELLO -> CAPS + ACK
  uint8_t hello = (uint8_t)Cmd::HELLO;
  host_send_cmd(srv, 0, 10, &hello, 1);
  CHECK(host.got_caps, "host received CAPS");
  CHECK(host.caps.fw_major_minor==0x0107 && host.caps.has_psram==1 && host.caps.feature_bitmap0==0xDEADBEEF,
        "CAPS fields intact");
  bool saw_ack_10=false; for(size_t i=0;i<host.types.size();++i) if(host.types[i]==Type::ACK&&host.seqs[i]==10) saw_ack_10=true;
  CHECK(saw_ack_10, "HELLO ACKed with seq 10");

  // 2) FEATURE_START(NRF_SCANNER) -> ACK, handler fired
  uint8_t fs[2] = { (uint8_t)Cmd::FEATURE_START, (uint8_t)Feature::NRF_SCANNER };
  host_send_cmd(srv, (uint8_t)Feature::NRF_SCANNER, 11, fs, 2);
  CHECK(g_feature_started == (int)Feature::NRF_SCANNER, "companion started NRF_SCANNER");
  bool saw_ack_11=false; for(size_t i=0;i<host.types.size();++i) if(host.types[i]==Type::ACK&&host.seqs[i]==11) saw_ack_11=true;
  CHECK(saw_ack_11, "FEATURE_START ACKed");

  // 3) Unknown CMD -> NAK(UNKNOWN_CMD)
  uint8_t junk[1] = { 0xFE };
  host_send_cmd(srv, 3, 12, junk, 1);
  bool saw_nak=false; for(size_t i=0;i<host.types.size();++i)
    if(host.types[i]==Type::NAK&&host.seqs[i]==12&&!host.pays[i].empty()&&host.pays[i][0]==(uint8_t)Nak::UNKNOWN_CMD) saw_nak=true;
  CHECK(saw_nak, "unknown CMD NAKed with reason");

  // 4) Companion EVT -> host sees it
  size_t before = host.types.size();
  PacketStat ps{}; ps.channel=6; ps.mgmt=190; ps.data=324; ps.dropped=2;
  srv.sendEvt((uint8_t)Feature::WIFI_PACKET_MONITOR, (const uint8_t*)&ps, sizeof(ps));
  CHECK(host.types.size()==before+1 && host.types.back()==Type::EVT, "host got EVT");
  if (host.types.back()==Type::EVT) {
    PacketStat r; memcpy(&r, host.pays.back().data(), sizeof(r));
    CHECK(r.channel==6 && r.mgmt==190 && r.data==324 && r.dropped==2, "EVT payload intact");
  }

  // 5) Host INPUT -> companion handler
  InputEvent ev{}; ev.kind=(uint8_t)InputKind::BTN_DOWN; ev.btn=(uint8_t)Btn::SELECT; ev.pressed=1;
  uint8_t wire[MAX_WIRE];
  size_t w = encode_frame(Type::INPUT_EV, 0, 0, (const uint8_t*)&ev, sizeof(ev), wire, sizeof(wire));
  srv.feed(wire, w);
  CHECK(g_got_input && g_last_input.btn==(uint8_t)Btn::SELECT, "companion got INPUT(SELECT)");

  // 6) Duplicate CMD (same seq/chan) is re-ACKed but NOT re-dispatched (H2).
  {
    int before = g_dispatch_count;
    uint8_t fs2[2] = { (uint8_t)Cmd::FEATURE_START, (uint8_t)Feature::SUBGHZ_REPLAY };
    host_send_cmd(srv, (uint8_t)Feature::SUBGHZ_REPLAY, 30, fs2, 2);  // first delivery
    host_send_cmd(srv, (uint8_t)Feature::SUBGHZ_REPLAY, 30, fs2, 2);  // retransmit (same seq)
    CHECK(g_dispatch_count == before + 1, "duplicate CMD dispatched exactly once");
    int acks30 = 0; for (size_t i=0;i<host.types.size();++i) if (host.types[i]==Type::ACK && host.seqs[i]==30) acks30++;
    CHECK(acks30 == 2, "both CMD copies ACKed (idempotent ack)");
  }

  CHECK(srv.rxErr()==0, "companion saw no rx errors");
  std::printf("=== %d passed, %d failed ===\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
