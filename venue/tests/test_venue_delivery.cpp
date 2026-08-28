/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Exec-report delivery perimeter (SessionRegistry + gateways):
 *  - an asynchronous maker report reaches the MAKER's session, not the
 *    aggressor's (the old per-frame responder wrote into whoever's frame was
 *    being handled);
 *  - a rejected frame answers on the wire instead of silence;
 *  - a slow consumer is disconnected and never stalls matching or the other
 *    sessions;
 *  - reconnect recovery: ResendRequest replays missed reports with their
 *    original seqs, AccountSnapshotRequest rebuilds open state, and a
 *    too-old fromSeq gets an explicit SnapshotRequired.
 */
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/sbe.h"
#include "flox-venue/sbe_order_entry_codec.h"
#include "flox-venue/session_registry.h"
#include "flox-venue/session_verbs.h"
#include "flox-venue/tcp_gateway.h"
#include "flox/util/transport.h"

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
#include <thread>
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

uint8_t tmpl(const std::vector<uint8_t>& f)
{
  return static_cast<uint8_t>(SbeOrderEntryCodec::templateId(f.data(), f.size()));
}
uint64_t rootU64(const std::vector<uint8_t>& f, size_t off)
{
  return sbe::getU64(f.data() + sbe::kHeaderSize + off);
}

// The shared venue: one engine whose sink routes through the registry.
struct Venue
{
  GatewayCounters counters;
  SessionRegistry registry;
  std::mutex m;  // serializes engine access across gateway handler threads
  MatchingEngine<MatchingBook> eng;

  explicit Venue(DeliveryConfig dc = {})
      : registry(dc, &counters), eng(cfg(), [this](const OutboundEvent& e)
                                     { registry.route(e); })
  {
  }

  void submit(const InboundCommand& c)
  {
    std::lock_guard<std::mutex> lk(m);
    eng.submit(c);
  }

  static SessionRegistry::Encoder encoder()
  {
    return [](const OutboundEvent& e, uint64_t seq, int64_t, std::vector<uint8_t>& out)
    {
      SbeOrderEntryCodec::encode(e, out, seq);
      return !out.empty();
    };
  }

  std::unique_ptr<TcpGateway> gateway(uint64_t account, bool cod = false)
  {
    auto gw = std::make_unique<TcpGateway>(
        [](const uint8_t* p, size_t n)
        { return SbeOrderEntryCodec::decode(p, n); },
        account);
    gw->setDelivery(&registry, encoder());
    gw->setSessionVerbs(makeSbeSessionVerbs(eng));
    gw->setSessionConfigVerb(makeSbeSessionConfigVerb());
    gw->setCounters(&counters);
    gw->setCancelOnDisconnect(cod);
    return gw;
  }

  size_t openOrders(uint64_t account)
  {
    std::lock_guard<std::mutex> lk(m);
    return eng.snapshotAccount(account).openOrders.size();
  }

  TcpGateway::Handler handler()
  {
    return [this](const InboundCommand& c, const TcpGateway::Responder&, int64_t)
    { submit(c); };
  }
};

struct Client
{
  int fd{-1};

  bool connectTo(int port, int rcvbuf = 0)
  {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (rcvbuf > 0)
    {
      ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<uint16_t>(port));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0)
    {
      return false;
    }
    timeval tv{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return true;
  }
  void sendCmd(const InboundCommand& c)
  {
    std::vector<uint8_t> b;
    SbeOrderEntryCodec::encode(c, b);
    net::writeFrame(fd, b.data(), b.size());
  }
  void sendRaw(const std::vector<uint8_t>& b) { net::writeFrame(fd, b.data(), b.size()); }
  bool readFrame(std::vector<uint8_t>& f) { return net::readFrame(fd, f); }
  // Read until a frame with `wantTmpl` arrives (or timeout); returns it.
  bool readUntil(uint8_t wantTmpl, std::vector<uint8_t>& out)
  {
    std::vector<uint8_t> f;
    while (readFrame(f))
    {
      if (tmpl(f) == wantTmpl)
      {
        out = f;
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
  ~Client() { close(); }
};

// (1) The asynchronous maker exec report is delivered to the MAKER's session.
void test_cross_session_delivery()
{
  std::printf("test_cross_session_delivery\n");
  Venue v;
  auto gwA = v.gateway(1);
  auto gwB = v.gateway(2);
  const int pA = gwA->start(0, v.handler());
  const int pB = gwB->start(0, v.handler());
  CHECK(pA > 0 && pB > 0);

  Client a;
  Client b;
  CHECK(a.connectTo(pA) && b.connectTo(pB));

  // Maker rests on session A.
  a.sendCmd(InboundCommand{mk(1, Side::SELL, 100, 5)});
  std::vector<uint8_t> f;
  CHECK(a.readUntil(static_cast<uint8_t>(SbeOrderEntryCodec::OutTmpl::Accepted), f));
  CHECK(rootU64(f, 0) == 1);  // orderId
  CHECK(SbeOrderEntryCodec::seqOf(f.data(), f.size()) == 1);

  // Aggressor on session B fills it.
  b.sendCmd(InboundCommand{mk(2, Side::BUY, 100, 5)});
  CHECK(b.readUntil(static_cast<uint8_t>(SbeOrderEntryCodec::OutTmpl::Executed), f));
  CHECK(rootU64(f, 0) == 2);  // the taker's own report

  // The maker fill arrives on session A -- an event A never asked for on this
  // frame exchange (the old responder had no path here).
  CHECK(a.readUntil(static_cast<uint8_t>(SbeOrderEntryCodec::OutTmpl::Executed), f));
  CHECK(rootU64(f, 0) == 1);             // maker order id
  CHECK(f[sbe::kHeaderSize + 37] == 1);  // complete flag (5 of 5 filled)
  CHECK(SbeOrderEntryCodec::seqOf(f.data(), f.size()) > 1);

  a.close();
  b.close();
  gwA->stop();
  gwB->stop();
}

// (2) A rejected frame answers on the wire (sequenced OrderRejected), not
// silence.
void test_reject_on_wire()
{
  std::printf("test_reject_on_wire\n");
  Venue v;
  auto gw = v.gateway(1);
  const int port = gw->start(0, v.handler());
  CHECK(port > 0);

  Client c;
  CHECK(c.connectTo(port));
  c.sendRaw({0xDE, 0xAD, 0xBE, 0xEF});  // non-decodable frame

  std::vector<uint8_t> f;
  CHECK(c.readUntil(static_cast<uint8_t>(SbeOrderEntryCodec::OutTmpl::Rejected), f));
  CHECK(f[sbe::kHeaderSize + 12] == static_cast<uint8_t>(RejectReason::MalformedMessage));
  CHECK(SbeOrderEntryCodec::seqOf(f.data(), f.size()) == 1);

  c.close();
  gw->stop();
}

// (3) A stalled reader is disconnected by queue overflow; matching and the
// other sessions keep running; the counter records it.
void test_slow_consumer_disconnect()
{
  std::printf("test_slow_consumer_disconnect\n");
  Venue v(DeliveryConfig{/*queueCapacity*/ 8, /*resendLogCapacity*/ 16});
  auto gwA = v.gateway(1);
  auto gwB = v.gateway(2);
  const int pA = gwA->start(0, v.handler());
  const int pB = gwB->start(0, v.handler());
  CHECK(pA > 0 && pB > 0);

  Client a;  // will stop reading
  Client b;
  CHECK(a.connectTo(pA, /*rcvbuf*/ 2048) && b.connectTo(pB));

  // Make sure A is attached (round-trip one ack).
  a.sendCmd(InboundCommand{mk(1, Side::SELL, 149, 1)});
  std::vector<uint8_t> f;
  CHECK(a.readUntil(static_cast<uint8_t>(SbeOrderEntryCodec::OutTmpl::Accepted), f));

  // Flood account 1 with routed events while A never reads: kernel buffers
  // fill, the writer blocks, the bounded queue overflows -> disconnect policy.
  for (int i = 0; i < 200000 && v.counters.slowConsumerDisconnects.load() == 0; ++i)
  {
    v.registry.route(
        OutboundEvent{OrderCanceled{static_cast<OrderId>(1000 + i), SYM,
                                    CancelReason::UserRequested, /*account*/ 1}});
  }
  CHECK(v.counters.slowConsumerDisconnects.load() >= 1);

  // Matching keeps serving other sessions.
  b.sendCmd(InboundCommand{mk(2, Side::BUY, 99, 1)});
  CHECK(b.readUntil(static_cast<uint8_t>(SbeOrderEntryCodec::OutTmpl::Accepted), f));

  a.close();
  b.close();
  gwA->stop();
  gwB->stop();
}

// (4) Reconnect recovery: a fill that happened while the session was offline
// is served by ResendRequest with its original seq; AccountSnapshotRequest
// rebuilds the open-order state; a too-old fromSeq answers SnapshotRequired.
void test_resend_and_snapshot()
{
  std::printf("test_resend_and_snapshot\n");
  Venue v;
  auto gw = v.gateway(1);
  const int port = gw->start(0, v.handler());
  CHECK(port > 0);

  uint64_t seenSeq = 0;
  {
    Client a;
    CHECK(a.connectTo(port));
    a.sendCmd(InboundCommand{mk(1, Side::SELL, 100, 5)});
    std::vector<uint8_t> f;
    CHECK(a.readUntil(static_cast<uint8_t>(SbeOrderEntryCodec::OutTmpl::Accepted), f));
    seenSeq = SbeOrderEntryCodec::seqOf(f.data(), f.size());
    CHECK(seenSeq == 1);
    a.close();  // disconnect with the order resting
  }

  // A foreign aggressor fills the resting maker while account 1 is offline;
  // the reports are sequenced into account 1's log.
  v.submit(InboundCommand{mk(2, Side::BUY, 100, 5, /*acct*/ 2)});
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (v.registry.lastSeq(1) <= seenSeq && std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(v.registry.lastSeq(1) > seenSeq);

  // Reconnect + ResendRequest(fromSeq = seen + 1): the missed Trade and the
  // maker's Executed replay with their original seqs.
  Client a2;
  CHECK(a2.connectTo(port));
  std::vector<uint8_t> req;
  SbeOrderEntryCodec::encodeResendRequest(seenSeq + 1, req);
  a2.sendRaw(req);
  std::vector<uint8_t> f;
  CHECK(a2.readUntil(static_cast<uint8_t>(SbeOrderEntryCodec::OutTmpl::Executed), f));
  CHECK(rootU64(f, 0) == 1);  // the missed maker fill
  CHECK(SbeOrderEntryCodec::seqOf(f.data(), f.size()) > seenSeq);
  CHECK(v.counters.resendServed.load() >= 1);

  // Place a new resting order, then snapshot: it must show exactly that order
  // and a flat position, terminated by SnapshotEnd.
  a2.sendCmd(InboundCommand{mk(3, Side::SELL, 101, 2)});
  CHECK(a2.readUntil(static_cast<uint8_t>(SbeOrderEntryCodec::OutTmpl::Accepted), f));
  const uint64_t seqBeforeSnap = SbeOrderEntryCodec::seqOf(f.data(), f.size());
  SbeOrderEntryCodec::encodeSnapshotRequest(req);
  a2.sendRaw(req);
  CHECK(a2.readUntil(static_cast<uint8_t>(SbeOrderEntryCodec::OutTmpl::Accepted), f));
  CHECK(rootU64(f, 0) == 3);                                  // the open order, replayed as a snapshot record
  CHECK(SbeOrderEntryCodec::seqOf(f.data(), f.size()) == 0);  // snapshot frames are unsequenced
  CHECK(a2.readUntil(static_cast<uint8_t>(SbeOrderEntryCodec::OutTmpl::SnapshotEnd), f));
  const auto se = SbeOrderEntryCodec::decodeSnapshotEnd(f.data(), f.size());
  CHECK(se.has_value());
  CHECK(se->account == 1 && se->openOrders == 1 && se->positionQtyRaw == 0);
  // SnapshotEnd carries the stream's lastSeq: gap detection resumes from the
  // exact point, and the next sequenced event continues the numbering (the
  // snapshot itself consumed no seqs and entered no resend log).
  CHECK(se->lastSeq == seqBeforeSnap);
  CHECK(se->lastSeq == v.registry.lastSeq(1));
  a2.sendCmd(InboundCommand{mk(4, Side::SELL, 102, 1)});
  CHECK(a2.readUntil(static_cast<uint8_t>(SbeOrderEntryCodec::OutTmpl::Accepted), f));
  CHECK(SbeOrderEntryCodec::seqOf(f.data(), f.size()) == se->lastSeq + 1);
  a2.close();
  gw->stop();
}

// (5) A fromSeq older than the retained log answers SnapshotRequired -- an
// explicit signal, never a silent hole.
void test_resend_too_old()
{
  std::printf("test_resend_too_old\n");
  Venue v(DeliveryConfig{/*queueCapacity*/ 64, /*resendLogCapacity*/ 1});
  auto gw = v.gateway(1);
  const int port = gw->start(0, v.handler());
  CHECK(port > 0);

  Client a;
  CHECK(a.connectTo(port));
  a.sendCmd(InboundCommand{mk(1, Side::SELL, 100, 1)});
  std::vector<uint8_t> f;
  CHECK(a.readUntil(static_cast<uint8_t>(SbeOrderEntryCodec::OutTmpl::Accepted), f));
  a.sendCmd(InboundCommand{mk(2, Side::SELL, 101, 1)});
  CHECK(a.readUntil(static_cast<uint8_t>(SbeOrderEntryCodec::OutTmpl::Accepted), f));

  std::vector<uint8_t> req;
  SbeOrderEntryCodec::encodeResendRequest(1, req);  // seq 1 was trimmed (log holds 1 frame)
  a.sendRaw(req);
  CHECK(a.readUntil(static_cast<uint8_t>(SbeOrderEntryCodec::OutTmpl::SnapshotRequired), f));
  CHECK(rootU64(f, 0) == v.registry.lastSeq(1));  // lastSeq echoed for re-sync

  a.close();
  gw->stop();
}

// (6) SetSessionConfig negotiates cancel-on-disconnect on the wire: enabling
// it makes a disconnect sweep the resting order; disabling it overrides a
// gateway-default ON. Fire-and-forget -- no reply frame, the sweep (or its
// absence) is the observable effect.
void test_session_config_cod()
{
  std::printf("test_session_config_cod\n");
  Venue v;

  // Gateway default OFF, SetSessionConfig{cod=1}: disconnect sweeps.
  {
    auto gw = v.gateway(1, /*cod*/ false);
    const int port = gw->start(0, v.handler());
    CHECK(port > 0);
    Client a;
    CHECK(a.connectTo(port));
    std::vector<uint8_t> req;
    SbeOrderEntryCodec::encodeSetSessionConfig(true, req);
    a.sendRaw(req);
    a.sendCmd(InboundCommand{mk(1, Side::SELL, 100, 1)});
    std::vector<uint8_t> f;
    CHECK(a.readUntil(static_cast<uint8_t>(SbeOrderEntryCodec::OutTmpl::Accepted), f));
    CHECK(v.openOrders(1) == 1);
    a.close();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (v.openOrders(1) != 0 && std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(v.openOrders(1) == 0);  // negotiated COD swept the order
    gw->stop();
  }

  // Gateway default ON, SetSessionConfig{cod=0}: the resting order survives.
  {
    auto gw = v.gateway(2, /*cod*/ true);
    const int port = gw->start(0, v.handler());
    CHECK(port > 0);
    Client a;
    CHECK(a.connectTo(port));
    std::vector<uint8_t> req;
    SbeOrderEntryCodec::encodeSetSessionConfig(false, req);
    a.sendRaw(req);
    a.sendCmd(InboundCommand{mk(2, Side::SELL, 100, 1)});
    std::vector<uint8_t> f;
    CHECK(a.readUntil(static_cast<uint8_t>(SbeOrderEntryCodec::OutTmpl::Accepted), f));
    CHECK(v.openOrders(2) == 1);
    a.close();
    gw->stop();                   // joins the connection thread: any COD flush has run
    CHECK(v.openOrders(2) == 1);  // negotiated OFF: nothing swept
  }
}

// (7) BalanceUpdate reaches the owner over the wire: a deposit reports the
// credited balance, a covered withdraw the debited one, and an uncovered
// withdraw an explicit WithdrawRejected with unchanged balances -- all
// sequenced on the exec-report stream.
void test_balance_update_on_wire()
{
  std::printf("test_balance_update_on_wire\n");
  constexpr AssetId QUOTE = 1;
  Venue v;
  Ledger led;
  {
    std::lock_guard<std::mutex> lk(v.m);
    v.eng.setLedger(&led, /*venueAccount*/ 900);
  }
  auto gw = v.gateway(1);
  const int port = gw->start(0, v.handler());
  CHECK(port > 0);

  Client a;
  CHECK(a.connectTo(port));
  // Round-trip one frame so the session is attached before the deposit fires
  // (the buy has no funds yet -> a sequenced InsufficientFunds reject, seq 1).
  a.sendCmd(InboundCommand{mk(1, Side::BUY, 100, 1)});
  std::vector<uint8_t> f;
  CHECK(a.readUntil(static_cast<uint8_t>(SbeOrderEntryCodec::OutTmpl::Rejected), f));

  v.submit(InboundCommand{Deposit{1, QUOTE, 5'000, SYM}});
  CHECK(a.readUntil(static_cast<uint8_t>(SbeOrderEntryCodec::OutTmpl::BalanceUpdate), f));
  auto bu = SbeOrderEntryCodec::decodeBalanceUpdate(f.data(), f.size());
  CHECK(bu.has_value());
  CHECK(bu->account == 1 && bu->asset == QUOTE);
  CHECK(bu->availableRaw == 5'000 && bu->reservedRaw == 0);
  CHECK(bu->reason == BalanceReason::Deposit);
  CHECK(SbeOrderEntryCodec::seqOf(f.data(), f.size()) == 2);  // sequenced stream

  // Covered withdraw: balances shrink, reason Withdraw.
  v.submit(InboundCommand{Withdraw{1, QUOTE, 2'000, SYM}});
  CHECK(a.readUntil(static_cast<uint8_t>(SbeOrderEntryCodec::OutTmpl::BalanceUpdate), f));
  bu = SbeOrderEntryCodec::decodeBalanceUpdate(f.data(), f.size());
  CHECK(bu.has_value());
  CHECK(bu->availableRaw == 3'000 && bu->reason == BalanceReason::Withdraw);

  // Uncovered withdraw: nothing moves, the client is told explicitly.
  v.submit(InboundCommand{Withdraw{1, QUOTE, 1'000'000, SYM}});
  CHECK(a.readUntil(static_cast<uint8_t>(SbeOrderEntryCodec::OutTmpl::BalanceUpdate), f));
  bu = SbeOrderEntryCodec::decodeBalanceUpdate(f.data(), f.size());
  CHECK(bu.has_value());
  CHECK(bu->availableRaw == 3'000 && bu->reason == BalanceReason::WithdrawRejected);
  CHECK(SbeOrderEntryCodec::seqOf(f.data(), f.size()) == 4);

  a.close();
  gw->stop();
}

}  // namespace

TEST(Delivery, EngineSuite)
{
  test_cross_session_delivery();
  test_reject_on_wire();
  test_slow_consumer_disconnect();
  test_resend_and_snapshot();
  test_resend_too_old();
  test_session_config_cod();
  test_balance_update_on_wire();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
