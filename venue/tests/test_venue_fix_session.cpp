/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * FIX session-layer recovery (fix_session.h + SessionRegistry event log),
 * driven by a scripted counterparty over TCP:
 *  - Logon with ResetSeqNumFlag, a trading cycle, Heartbeat exchange, clean
 *    Logout;
 *  - our resend: a fill delivered while the client was offline replays on
 *    ResendRequest with PossDupFlag + OrigSendingTime and its ORIGINAL seq,
 *    admin seqs (Heartbeat, Logon reply) collapse into SequenceReset-GapFill;
 *  - their ResendRequest older than the retained log gap-fills up to the
 *    first available seq and replays the rest;
 *  - our inbound gap: the venue sends its own ResendRequest, drops messages
 *    above the hole, and applies them exactly once after the PossDup replay
 *    closes it;
 *  - TestRequest timeout disconnects and cancel-on-disconnect sweeps the
 *    resting order;
 *  - venue restart: a Logon without 141=Y against lost session state answers
 *    Logout with a reason; 141=Y starts a fresh sequence space;
 *  - venue restart WITH the session sidecar (FixSessionSidecar): the sequence
 *    counters survive, a Logon without 141=Y continues the old space, trading
 *    continues, and a resend into the pre-restart range answers GapFill;
 *  - unknown MsgType answers a session Reject (35=3) instead of silence;
 *  - Logon tag 20003 negotiates cancel-on-disconnect on the wire;
 *  - the same session layer runs over WebSocket (one FIX message per Text
 *    frame; FIX heartbeats replace the WS Ping probe).
 */
#include "flox-venue/fix_codec.h"
#include "flox-venue/fix_session.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/session_registry.h"
#include "flox-venue/tcp_gateway.h"
#include "flox-venue/ws_gateway.h"
#include "flox/util/transport.h"
#include "flox/util/websocket.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{
int g_failures = 0;
int g_checks = 0;
void check(bool ok, const char* e, int line)
{
  ++g_checks;
  if (!ok)
  {
    ++g_failures;
    std::printf("  FAIL line %d: %s\n", line, e);
  }
}
#define CHECK(x) check((x), #x, __LINE__)

constexpr SymbolId SYM = 1;
Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  return c;
}

NewOrder mk(OrderId id, Side s, double p, double q, uint64_t acct = 0)
{
  NewOrder o;
  o.id = id;
  o.symbol = SYM;
  o.side = s;
  o.type = OrderType::LIMIT;
  o.price = px(p);
  o.quantity = qty(q);
  o.accountId = acct;
  return o;
}

// The venue under test: engine sink -> registry, one FIX gateway per account.
struct Venue
{
  GatewayCounters counters;
  SessionRegistry registry;
  std::mutex m;  // serializes engine access across gateway handler threads
  MatchingEngine<MatchingBook> eng;
  FixSessionHost fixHost;

  explicit Venue(DeliveryConfig dc = {})
      : registry(dc, &counters),
        eng(cfg(), [this](const OutboundEvent& e)
            { registry.route(e); }),
        fixHost(fixCfg())
  {
  }

  static FixSessionConfig fixCfg()
  {
    FixSessionConfig c;
    c.senderCompId = "VENUE";
    c.targetCompId = "CLIENT";
    return c;
  }

  static SessionRegistry::Encoder encoder(const FixSessionConfig& fc)
  {
    return [fc](const OutboundEvent& e, uint64_t seq, int64_t tsNs, std::vector<uint8_t>& out)
    {
      const std::string msg = FixCodec::encode(e, seq, fc.senderCompId, fc.targetCompId,
                                               FixSession::sendingTime(tsNs));
      if (msg.empty())
      {
        return false;
      }
      out.assign(msg.begin(), msg.end());
      return true;
    };
  }

  std::unique_ptr<TcpGateway> gateway(uint64_t account, bool cod = false)
  {
    auto gw = std::make_unique<TcpGateway>(
        [](const uint8_t* p, size_t n)
        { return FixCodec::decode(std::string(reinterpret_cast<const char*>(p), n)); },
        account);
    gw->setDelivery(&registry, encoder(fixHost.config()));
    gw->setFixSession(&fixHost);
    gw->setCounters(&counters);
    gw->setCancelOnDisconnect(cod);
    return gw;
  }

  std::unique_ptr<WsGateway> wsGateway(uint64_t account, bool cod = false)
  {
    auto gw = std::make_unique<WsGateway>(
        [](const uint8_t* p, size_t n)
        { return FixCodec::decode(std::string(reinterpret_cast<const char*>(p), n)); },
        account);
    gw->setDelivery(&registry, encoder(fixHost.config()));
    gw->setFixSession(&fixHost);
    gw->setCounters(&counters);
    gw->setCancelOnDisconnect(cod);
    return gw;
  }

  TcpGateway::Handler handler()
  {
    return [this](const InboundCommand& c, const TcpGateway::Responder&)
    { submit(c); };
  }

  void submit(const InboundCommand& c)
  {
    std::lock_guard<std::mutex> lk(m);
    eng.submit(c);
  }

  size_t openOrders(uint64_t account)
  {
    std::lock_guard<std::mutex> lk(m);
    return eng.snapshotAccount(account).openOrders.size();
  }
};

using Fields = std::unordered_map<int, std::string>;

// Scripted FIX counterparty: full-header messages over the gateway's
// length-prefixed transport framing, one FIX message per frame.
struct FixClient
{
  int fd{-1};
  uint64_t seq{1};

  bool connectTo(int port, int rcvTimeoutSec = 4)
  {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<uint16_t>(port));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0)
    {
      return false;
    }
    timeval tv{rcvTimeoutSec, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return true;
  }

  void send(const std::string& m)
  {
    net::writeFrame(fd, reinterpret_cast<const uint8_t*>(m.data()), m.size());
  }

  // Admin message; seqOverride 0 = consume the next own seq.
  void admin(const std::string& type, const std::vector<std::pair<int, std::string>>& fields = {},
             uint64_t seqOverride = 0, bool possDup = false)
  {
    const uint64_t s = seqOverride != 0 ? seqOverride : seq++;
    send(FixCodec::encodeAdmin(type, s, "CLIENT", "VENUE",
                               FixSession::sendingTime(wallClockNs()), fields, possDup));
  }

  void order(uint64_t id, const char* side, const char* price, const char* quantity,
             uint64_t seqOverride = 0, bool possDup = false)
  {
    const uint64_t s = seqOverride != 0 ? seqOverride : seq++;
    std::string b;
    auto add = [&](int t, const std::string& v)
    { b += std::to_string(t) + "=" + v + std::string(1, FixCodec::SOH); };
    add(35, "D");
    add(34, std::to_string(s));
    add(49, "CLIENT");
    add(56, "VENUE");
    add(52, FixSession::sendingTime(wallClockNs()));
    if (possDup)
    {
      add(43, "Y");
    }
    add(11, std::to_string(id));
    add(55, "1");
    add(54, side);
    add(38, quantity);
    add(44, price);
    add(40, "2");
    send(FixCodec::frame(b));
  }

  bool read(Fields& f)
  {
    std::vector<uint8_t> frame;
    if (!net::readFrame(fd, frame))
    {
      return false;
    }
    f = FixCodec::parseFields(std::string(frame.begin(), frame.end()));
    return true;
  }

  // Read until a message of `type` arrives (or timeout / EOF).
  bool readType(const std::string& type, Fields& f)
  {
    while (read(f))
    {
      if (f.count(35) != 0 && f[35] == type)
      {
        return true;
      }
    }
    return false;
  }

  void close()
  {
    if (fd >= 0)
    {
      ::close(fd);
      fd = -1;
    }
  }
  ~FixClient() { close(); }
};

uint64_t u64f(Fields& f, int tag)
{
  return f.count(tag) != 0 ? std::strtoull(f[tag].c_str(), nullptr, 10) : 0;
}

// (1) Logon 141=Y, trading cycle, Heartbeat both ways, clean Logout.
void test_logon_trade_heartbeat()
{
  std::printf("test_logon_trade_heartbeat\n");
  Venue v;
  auto gw = v.gateway(1);
  const int port = gw->start(0, v.handler());
  CHECK(port > 0);

  FixClient c;
  CHECK(c.connectTo(port));
  c.admin("A", {{108, "1"}, {141, "Y"}});
  Fields f;
  CHECK(c.readType("A", f));
  CHECK(u64f(f, 34) == 1);   // fresh sequence space
  CHECK(f[141] == "Y");      // reset confirmed
  CHECK(u64f(f, 108) == 1);  // HeartBtInt adopted
  CHECK(f.count(52) != 0);

  c.order(1, "2", "100", "5");
  CHECK(c.readType("8", f));
  CHECK(u64f(f, 34) == 2 && f[150] == "0" && u64f(f, 37) == 1);

  // Idle past HeartBtInt: the venue heartbeats on its own timer.
  CHECK(c.readType("0", f));
  CHECK(u64f(f, 34) >= 3);

  // Answer with our own Heartbeat (keeps the venue's liveness happy), then a
  // clean Logout: the venue confirms with its own 35=5 and closes.
  c.admin("0");
  c.admin("5");
  CHECK(c.readType("5", f));
  CHECK(!c.read(f));  // venue closed the connection after the Logout exchange

  c.close();
  gw->stop();
}

// (2) Our resend: the client misses a fill while offline; after a reconnect
// Logon WITHOUT 141 at the expected inbound seq, its ResendRequest gets the
// fill as a PossDup replay (43=Y, 122 set, ORIGINAL 34) and GapFills over the
// admin seqs (Heartbeat echo, Logon reply).
void test_our_resend_possdup_and_gapfill()
{
  std::printf("test_our_resend_possdup_and_gapfill\n");
  Venue v;
  auto gw = v.gateway(1);
  const int port = gw->start(0, v.handler());
  CHECK(port > 0);

  {
    FixClient c;
    CHECK(c.connectTo(port));
    c.admin("A", {{108, "30"}});  // fresh session, no reset needed (34=1)
    Fields f;
    CHECK(c.readType("A", f));
    CHECK(u64f(f, 34) == 1);

    c.order(1, "2", "100", "5");  // maker rests
    CHECK(c.readType("8", f));
    CHECK(u64f(f, 34) == 2);

    c.admin("1", {{112, "ping"}});  // venue Heartbeat echo consumes seq 3 (admin hole)
    CHECK(c.readType("0", f));
    CHECK(u64f(f, 34) == 3 && f[112] == "ping");
    c.close();  // disconnect with the order resting; client has sent seqs 1..3
  }

  // A foreign aggressor fills the resting maker while account 1 is offline:
  // the Executed report is sequenced (seq 4) into the event log.
  v.submit(InboundCommand{mk(2, Side::BUY, 100, 5, /*acct*/ 2)});
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (v.registry.lastSeq(1) < 4 && std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(v.registry.lastSeq(1) == 4);

  // Reconnect without 141 at the expected inbound seq (4). The Logon reply
  // carries seq 5 -- the client sees the gap and requests 3..infinity.
  FixClient c;
  CHECK(c.connectTo(port));
  c.seq = 4;
  c.admin("A", {{108, "30"}});
  Fields f;
  CHECK(c.readType("A", f));
  CHECK(u64f(f, 34) == 5);

  c.admin("2", {{7, "3"}, {16, "0"}});
  // Strict replay order: GapFill over the Heartbeat (3 -> 4), the PossDup
  // fill at its ORIGINAL seq 4, GapFill over the Logon reply (5 -> 6).
  CHECK(c.read(f));
  CHECK(f[35] == "4" && u64f(f, 34) == 3 && f[123] == "Y" && u64f(f, 36) == 4 && f[43] == "Y");
  CHECK(c.read(f));
  CHECK(f[35] == "8" && u64f(f, 34) == 4);
  CHECK(f[43] == "Y");       // PossDupFlag
  CHECK(f.count(122) != 0);  // OrigSendingTime from the first transmission
  CHECK(f[150] == "F");      // ExecType Trade
  CHECK(f[39] == "2");       // filled
  CHECK(u64f(f, 37) == 1 && f[32] == "5");
  CHECK(c.read(f));
  CHECK(f[35] == "4" && u64f(f, 34) == 5 && u64f(f, 36) == 6);
  CHECK(v.counters.resendServed.load() == 1);  // FIX resend bumps the shared counter

  c.close();
  gw->stop();
}

// (3) Their ResendRequest older than the retained log: GapFill up to the
// first available seq, then the honest replay of what is retained.
void test_their_resend_older_than_log()
{
  std::printf("test_their_resend_older_than_log\n");
  Venue v(DeliveryConfig{/*queueCapacity*/ 64, /*resendLogCapacity*/ 1});
  auto gw = v.gateway(1);
  const int port = gw->start(0, v.handler());
  CHECK(port > 0);

  FixClient c;
  CHECK(c.connectTo(port));
  c.admin("A", {{108, "30"}});
  Fields f;
  CHECK(c.readType("A", f));

  c.order(1, "2", "100", "1");  // Accepted seq 2 -- trimmed (log holds 1 event)
  CHECK(c.readType("8", f));
  CHECK(u64f(f, 34) == 2);
  c.order(2, "2", "101", "1");  // Accepted seq 3 -- retained
  CHECK(c.readType("8", f));
  CHECK(u64f(f, 34) == 3);

  c.admin("2", {{7, "2"}, {16, "0"}});
  CHECK(c.read(f));
  CHECK(f[35] == "4" && u64f(f, 34) == 2 && f[123] == "Y" && u64f(f, 36) == 3);  // trimmed part
  CHECK(c.read(f));
  CHECK(f[35] == "8" && u64f(f, 34) == 3 && f[43] == "Y" && u64f(f, 37) == 2);

  c.close();
  gw->stop();
}

// (4) Our inbound gap: the venue sends its own 35=2, DROPS messages above the
// hole, and applies them exactly once after the PossDup replay closes it.
void test_inbound_gap_resend_and_recover()
{
  std::printf("test_inbound_gap_resend_and_recover\n");
  Venue v;
  auto gw = v.gateway(1);
  const int port = gw->start(0, v.handler());
  CHECK(port > 0);

  FixClient c;
  CHECK(c.connectTo(port));
  c.admin("A", {{108, "30"}});
  Fields f;
  CHECK(c.readType("A", f));

  // Seq 2 "gets lost": the client jumps to 3. Both orders above the hole are
  // dropped; the venue requests a resend from 2.
  c.order(10, "2", "100", "1", /*seq*/ 3);
  c.order(11, "2", "101", "1", /*seq*/ 4);
  CHECK(c.readType("2", f));
  CHECK(u64f(f, 7) == 2 && u64f(f, 16) == 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(v.openOrders(1) == 0);  // nothing above the hole was applied

  // Close the hole: the "lost" seq 2 was a Heartbeat, then replay both orders
  // with PossDup. Each applies exactly once, in order.
  c.admin("0", {}, /*seq*/ 2);
  c.order(10, "2", "100", "1", /*seq*/ 3, /*possDup*/ true);
  c.order(11, "2", "101", "1", /*seq*/ 4, /*possDup*/ true);
  CHECK(c.readType("8", f));
  CHECK(u64f(f, 37) == 10 && f[150] == "0");
  CHECK(c.readType("8", f));
  CHECK(u64f(f, 37) == 11 && f[150] == "0");
  CHECK(v.openOrders(1) == 2);

  // A PossDup replay of an already-seen seq is silently dropped.
  c.order(10, "2", "100", "1", /*seq*/ 3, /*possDup*/ true);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(v.openOrders(1) == 2);

  c.close();
  gw->stop();
}

// (5) Liveness: no inbound traffic -> Heartbeat, then TestRequest, then
// disconnect; cancel-on-disconnect sweeps the resting order.
void test_testrequest_timeout_cod()
{
  std::printf("test_testrequest_timeout_cod\n");
  Venue v;
  auto gw = v.gateway(1, /*cod*/ true);
  const int port = gw->start(0, v.handler());
  CHECK(port > 0);

  FixClient c;
  CHECK(c.connectTo(port, /*rcvTimeoutSec*/ 4));
  c.admin("A", {{108, "1"}, {141, "Y"}});
  Fields f;
  CHECK(c.readType("A", f));
  c.order(20, "2", "100", "1");
  CHECK(c.readType("8", f));
  CHECK(v.openOrders(1) == 1);

  // Go silent. Expected: venue Heartbeat at ~1s, TestRequest at ~1.2s, and a
  // disconnect ~1.2 intervals later with no answer.
  bool sawHeartbeat = false;
  bool sawTestRequest = false;
  while (c.read(f))
  {
    sawHeartbeat = sawHeartbeat || f[35] == "0";
    sawTestRequest = sawTestRequest || f[35] == "1";
  }
  CHECK(sawHeartbeat);
  CHECK(sawTestRequest);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (v.openOrders(1) != 0 && std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  CHECK(v.openOrders(1) == 0);  // COD swept the resting order
  CHECK(v.counters.idleDisconnects.load() >= 1);

  c.close();
  gw->stop();
}

// (6) Venue restart: session state is in-memory, so a Logon that continues an
// old sequence space without 141=Y is answered with a Logout naming the
// reason; a Logon with 141=Y starts clean.
void test_restart_requires_reset()
{
  std::printf("test_restart_requires_reset\n");
  Venue v;  // "restarted": no session has ever attached here
  auto gw = v.gateway(1);
  const int port = gw->start(0, v.handler());
  CHECK(port > 0);

  {
    FixClient c;
    CHECK(c.connectTo(port));
    c.admin("A", {{108, "30"}}, /*seq*/ 7);  // stale pre-restart sequence space
    Fields f;
    CHECK(c.readType("5", f));
    CHECK(f[58].find("141=Y") != std::string::npos);
    CHECK(!c.read(f));  // disconnected
  }

  FixClient c;
  CHECK(c.connectTo(port));
  c.admin("A", {{108, "30"}, {141, "Y"}});
  Fields f;
  CHECK(c.readType("A", f));
  CHECK(u64f(f, 34) == 1 && f[141] == "Y");
  c.order(30, "2", "100", "1");
  CHECK(c.readType("8", f));
  CHECK(u64f(f, 37) == 30 && u64f(f, 34) == 2);

  c.close();
  gw->stop();
}

// (7) Venue restart WITH the session sidecar: seq counters survive, a Logon
// without 141=Y at the correct 34 continues the old sequence space, trading
// continues, and a resend into the pre-restart range answers GapFill (the
// event log is deliberately not persisted).
void test_restart_with_sidecar()
{
  std::printf("test_restart_with_sidecar\n");
  const std::string sidecar = "/tmp/flox_test_fix_sidecar.fixsessions";
  std::remove(sidecar.c_str());

  auto v1 = std::make_unique<Venue>();
  {
    auto gw = v1->gateway(1);
    const int port = gw->start(0, v1->handler());
    CHECK(port > 0);

    FixClient c;
    CHECK(c.connectTo(port));
    c.admin("A", {{108, "30"}});  // fresh session: seq 1, no reset needed
    Fields f;
    CHECK(c.readType("A", f));
    CHECK(u64f(f, 34) == 1);
    c.order(1, "2", "100", "1");
    CHECK(c.readType("8", f));
    CHECK(u64f(f, 34) == 2);  // client has sent 1..2, venue has sent 1..2
    c.close();
    gw->stop();
  }
  // The checkpoint boundary: sidecar written with the journal's discipline.
  CHECK(FixSessionSidecar::write(sidecar, v1->fixHost, v1->registry));
  v1.reset();  // the venue process dies

  // "Restart": a fresh host + registry rebuilt from the sidecar.
  Venue v2;
  CHECK(FixSessionSidecar::load(sidecar, v2.fixHost, v2.registry));
  auto gw = v2.gateway(1);
  const int port = gw->start(0, v2.handler());
  CHECK(port > 0);

  FixClient c;
  CHECK(c.connectTo(port));
  c.seq = 3;                    // continuing the pre-restart sequence space
  c.admin("A", {{108, "30"}});  // NO 141=Y -- works against restored state
  Fields f;
  CHECK(c.readType("A", f));
  CHECK(u64f(f, 34) == 3);   // outbound seq continues (pre-restart lastSeq 2)
  CHECK(f.count(141) == 0);  // no forced reset

  c.order(2, "2", "101", "1");  // trading continues in the same space
  CHECK(c.readType("8", f));
  CHECK(u64f(f, 37) == 2 && u64f(f, 34) == 4);

  // Their 35=2 into the pre-restart range: the log did not survive, so the
  // whole range collapses into SequenceReset-GapFill -- an explicit hole,
  // never a silent one or a replay of invented history.
  c.admin("2", {{7, "1"}, {16, "2"}});
  CHECK(c.readType("4", f));
  CHECK(u64f(f, 34) == 1 && f[123] == "Y" && u64f(f, 36) == 3 && f[43] == "Y");
  CHECK(v2.counters.resendServed.load() >= 1);

  c.close();
  gw->stop();
  std::remove(sidecar.c_str());
}

// (8) Unknown MsgType: consumed in sequence and answered with a session
// Reject (35=3, 45=RefSeqNum, 372=RefMsgType); the session stays alive and
// application flow continues.
void test_session_reject_unknown_type()
{
  std::printf("test_session_reject_unknown_type\n");
  Venue v;
  auto gw = v.gateway(1);
  const int port = gw->start(0, v.handler());
  CHECK(port > 0);

  FixClient c;
  CHECK(c.connectTo(port));
  c.admin("A", {{108, "30"}, {141, "Y"}});
  Fields f;
  CHECK(c.readType("A", f));

  c.admin("B", {{58, "hello"}});  // News: not in the supported set
  CHECK(c.readType("3", f));
  CHECK(u64f(f, 45) == 2);  // RefSeqNum of the offending message
  CHECK(f[372] == "B");     // RefMsgType
  CHECK(f.count(58) != 0);

  c.order(40, "2", "100", "1");  // the session survived the reject
  CHECK(c.readType("8", f));
  CHECK(u64f(f, 37) == 40 && f[150] == "0");

  c.close();
  gw->stop();
}

// (9) COD negotiation on the wire: Logon tag 20003 overrides the gateway
// default in both directions.
void test_cod_negotiation()
{
  std::printf("test_cod_negotiation\n");
  Venue v;

  // Gateway default OFF, Logon 20003=Y: disconnect sweeps the resting order.
  {
    auto gw = v.gateway(1, /*cod*/ false);
    const int port = gw->start(0, v.handler());
    CHECK(port > 0);
    FixClient c;
    CHECK(c.connectTo(port));
    c.admin("A", {{108, "30"}, {141, "Y"}, {20003, "Y"}});
    Fields f;
    CHECK(c.readType("A", f));
    c.order(50, "2", "100", "1");
    CHECK(c.readType("8", f));
    CHECK(v.openOrders(1) == 1);
    c.close();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (v.openOrders(1) != 0 && std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(v.openOrders(1) == 0);  // negotiated COD swept the order
    gw->stop();
  }

  // Gateway default ON, Logon 20003=N: the resting order survives.
  {
    auto gw = v.gateway(2, /*cod*/ true);
    const int port = gw->start(0, v.handler());
    CHECK(port > 0);
    FixClient c;
    CHECK(c.connectTo(port));
    c.admin("A", {{108, "30"}, {141, "Y"}, {20003, "N"}});
    Fields f;
    CHECK(c.readType("A", f));
    c.order(51, "2", "100", "1");
    CHECK(c.readType("8", f));
    CHECK(v.openOrders(2) == 1);
    c.close();
    gw->stop();                   // joins the connection thread: any COD flush has run
    CHECK(v.openOrders(2) == 1);  // negotiated OFF: nothing swept
  }
}

// Scripted FIX-over-WebSocket counterparty: one FIX message per masked Text
// frame; server frames parse through the shared ws:: helpers.
struct WsFixClient
{
  int fd{-1};
  uint64_t seq{1};
  std::vector<uint8_t> buf;

  bool connectTo(int port, int rcvTimeoutSec = 4)
  {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<uint16_t>(port));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0)
    {
      return false;
    }
    timeval tv{rcvTimeoutSec, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    const std::string hs =
        "GET /fix HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    if (::write(fd, hs.data(), hs.size()) != static_cast<ssize_t>(hs.size()))
    {
      return false;
    }
    std::string resp;
    uint8_t tb[2048];
    while (resp.find("\r\n\r\n") == std::string::npos)
    {
      const ssize_t r = ::read(fd, tb, sizeof tb);
      if (r <= 0)
      {
        return false;
      }
      resp.append(reinterpret_cast<char*>(tb), static_cast<size_t>(r));
    }
    return resp.find("101 Switching Protocols") != std::string::npos;
  }

  void send(const std::string& m)
  {
    std::vector<uint8_t> f;
    f.push_back(0x81);  // FIN + Text
    const size_t n = m.size();
    const uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};
    if (n < 126)
    {
      f.push_back(static_cast<uint8_t>(0x80 | n));
    }
    else
    {
      f.push_back(0x80 | 126);
      f.push_back(static_cast<uint8_t>(n >> 8));
      f.push_back(static_cast<uint8_t>(n));
    }
    for (int i = 0; i < 4; ++i)
    {
      f.push_back(mask[i]);
    }
    for (size_t i = 0; i < n; ++i)
    {
      f.push_back(static_cast<uint8_t>(m[i] ^ mask[i & 3]));
    }
    ::write(fd, f.data(), f.size());
  }

  void admin(const std::string& type, const std::vector<std::pair<int, std::string>>& fields = {},
             uint64_t seqOverride = 0, bool possDup = false)
  {
    const uint64_t s = seqOverride != 0 ? seqOverride : seq++;
    send(FixCodec::encodeAdmin(type, s, "CLIENT", "VENUE",
                               FixSession::sendingTime(wallClockNs()), fields, possDup));
  }

  void order(uint64_t id, const char* side, const char* price, const char* quantity)
  {
    const uint64_t s = seq++;
    std::string b;
    auto add = [&](int t, const std::string& v)
    { b += std::to_string(t) + "=" + v + std::string(1, FixCodec::SOH); };
    add(35, "D");
    add(34, std::to_string(s));
    add(49, "CLIENT");
    add(56, "VENUE");
    add(52, FixSession::sendingTime(wallClockNs()));
    add(11, std::to_string(id));
    add(55, "1");
    add(54, side);
    add(38, quantity);
    add(44, price);
    add(40, "2");
    send(FixCodec::frame(b));
  }

  // One FIX message = one WS data frame (control frames are skipped).
  bool read(Fields& f)
  {
    uint8_t tb[2048];
    while (true)
    {
      ws::Opcode op{};
      std::vector<uint8_t> payload;
      const size_t consumed = ws::parseFrame(buf.data(), buf.size(), op, payload);
      if (consumed == ws::kParseError)
      {
        return false;
      }
      if (consumed == 0)
      {
        const ssize_t r = ::read(fd, tb, sizeof tb);
        if (r <= 0)
        {
          return false;
        }
        buf.insert(buf.end(), tb, tb + r);
        continue;
      }
      buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(consumed));
      if (op == ws::Opcode::Text || op == ws::Opcode::Binary)
      {
        f = FixCodec::parseFields(std::string(payload.begin(), payload.end()));
        return true;
      }
      if (op == ws::Opcode::Close)
      {
        return false;
      }
      // Ping/Pong: skip (FIX heartbeats carry liveness on these sessions)
    }
  }

  bool readType(const std::string& type, Fields& f)
  {
    while (read(f))
    {
      if (f.count(35) != 0 && f[35] == type)
      {
        return true;
      }
    }
    return false;
  }

  void close()
  {
    if (fd >= 0)
    {
      ::close(fd);
      fd = -1;
    }
  }
  ~WsFixClient() { close(); }
};

// (10) The same FIX session layer over WebSocket: Logon 141=Y, a trading
// cycle, the venue's own Heartbeat, and a short resend (PossDup replay with
// the original seq) -- one FIX message per Text frame. The full recovery
// matrix stays on the TCP tests; the session layer is transport-independent.
void test_ws_fix_logon_trade_heartbeat_resend()
{
  std::printf("test_ws_fix_logon_trade_heartbeat_resend\n");
  Venue v;
  auto gw = v.wsGateway(1);
  const int port = gw->start(0, v.handler());
  CHECK(port > 0);

  WsFixClient c;
  CHECK(c.connectTo(port));
  c.admin("A", {{108, "1"}, {141, "Y"}});
  Fields f;
  CHECK(c.readType("A", f));
  CHECK(u64f(f, 34) == 1 && f[141] == "Y" && u64f(f, 108) == 1);

  c.order(1, "2", "100", "5");
  CHECK(c.readType("8", f));
  CHECK(u64f(f, 34) == 2 && f[150] == "0" && u64f(f, 37) == 1);

  // Idle past HeartBtInt: the venue heartbeats on its own timer (over WS the
  // FIX heartbeat replaces the Ping probe).
  CHECK(c.readType("0", f));
  CHECK(u64f(f, 34) >= 3);
  c.admin("0");

  // Short resend: request the Accepted (seq 2) again -- PossDup replay with
  // the ORIGINAL seq, GapFill over the admin seqs, all in Text frames.
  c.admin("2", {{7, "2"}, {16, "2"}});
  CHECK(c.readType("8", f));
  CHECK(u64f(f, 34) == 2 && f[43] == "Y" && f.count(122) != 0 && u64f(f, 37) == 1);
  CHECK(v.counters.resendServed.load() >= 1);

  c.admin("5");
  CHECK(c.readType("5", f));  // Logout confirmed

  c.close();
  gw->stop();
}

}  // namespace

TEST(FixSessionLayer, EngineSuite)
{
  test_logon_trade_heartbeat();
  test_our_resend_possdup_and_gapfill();
  test_their_resend_older_than_log();
  test_inbound_gap_resend_and_recover();
  test_testrequest_timeout_cod();
  test_restart_requires_reset();
  test_restart_with_sidecar();
  test_session_reject_unknown_type();
  test_cod_negotiation();
  test_ws_fix_logon_trade_heartbeat_resend();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
