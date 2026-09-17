/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * The FIX initiator (flox/connector/fix) against the venue's own acceptor
 * (FixSessionHost + FixConnection behind a TcpGateway), both ends live in one
 * process. This is the test that matters for the pair: a client and a server
 * that each pass their own unit tests can still disagree about recovery, and a
 * session where they disagree does not fail loudly -- it applies orders and
 * fills out of order.
 *
 *  - Logon with 141=Y, an order, the ExecutionReport decoded back into typed
 *    fields, a heartbeat each way, a clean Logout;
 *  - every outbound message round-trips through the venue's decoder, and every
 *    venue report round-trips back through the client decoder, 100.25 included;
 *  - a dropped outbound message: the venue's ResendRequest gets a PossDup
 *    replay at the ORIGINAL 34 with OrigSendingTime, and GapFill over the seqs
 *    the initiator's log does not hold;
 *  - a dropped inbound message: the initiator sends its own ResendRequest,
 *    refuses to apply anything above the hole, and applies the replay exactly
 *    once;
 *  - a reconnect that continues the sequence space, with the counters carried
 *    across an initiator restart through the sidecar;
 *  - the last-look surface: 150=U with 20001/20002, and 150=H when the held
 *    fill does not stand.
 */
#include "flox-venue/fix_codec.h"
#include "flox-venue/fix_session.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/session_registry.h"
#include "flox-venue/tcp_gateway.h"
#include "flox/connector/fix/fix_client_codec.h"
#include "flox/connector/fix/fix_initiator.h"
#include "flox/connector/fix/fix_tcp_client.h"
#include "support/tmp_path.h"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace flox;
using namespace flox::venue;
using flox::venue::test::tmpPath;

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

int64_t wallNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  return c;
}

// The venue under test, identical in shape to the acceptor suite's harness:
// engine sink -> registry -> one FIX gateway per account.
struct Venue
{
  GatewayCounters counters;
  SessionRegistry registry;
  std::mutex m;
  MatchingEngine<MatchingBook> eng;
  FixSessionHost fixHost;

  Venue()
      : registry(DeliveryConfig{}, &counters),
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

  std::unique_ptr<TcpGateway> gateway(uint64_t account)
  {
    auto gw = std::make_unique<TcpGateway>(
        [](const uint8_t* p, size_t n)
        { return FixCodec::decode(std::string(reinterpret_cast<const char*>(p), n)); },
        account);
    gw->setDelivery(&registry, encoder(fixHost.config()));
    gw->setFixSession(&fixHost);
    gw->setCounters(&counters);
    return gw;
  }

  TcpGateway::Handler handler()
  {
    return [this](const InboundCommand& c, const TcpGateway::Responder&, int64_t)
    { submit(c); };
  }

  void submit(const InboundCommand& c)
  {
    std::lock_guard<std::mutex> lk(m);
    eng.submit(c);
  }
};

// The client under test, plus the two seams a session test needs: an outbound
// hole (a frame the wire loses) and an inbound hole (a frame that never reaches
// onFrame). Both are what packet loss looks like from inside the endpoint, and
// both are what the resend machinery exists for.
struct Client
{
  fix::FixInitiatorConfig cfg;
  fix::FixInitiator initiator;
  fix::FixTcpClient tcp;
  std::vector<fix::InboundReport> reports;
  int dropOutbound{0};  // swallow this many of the next outbound app messages
  int dropInbound{0};   // withhold this many of the next inbound frames
  std::vector<std::string> sent;

  explicit Client(fix::FixInitiatorConfig c = defaults()) : cfg(std::move(c)), initiator(cfg)
  {
    initiator.setSend(
        [this](const std::string& m)
        {
          sent.push_back(m);
          if (dropOutbound > 0 && flox::fix::parseFields(m)[35] == "D")
          {
            --dropOutbound;
            return true;  // the venue never sees it: exactly what a lost frame is
          }
          return tcp.send(m);
        });
    initiator.setReportHandler([this](const fix::InboundReport& r)
                               { reports.push_back(r); });
  }

  static fix::FixInitiatorConfig defaults()
  {
    fix::FixInitiatorConfig c;
    c.senderCompId = "CLIENT";
    c.targetCompId = "VENUE";
    c.heartBtIntSec = 1;
    return c;
  }

  bool connect(int port) { return tcp.connect("127.0.0.1", static_cast<uint16_t>(port), 50); }

  // Pump the socket until `pred` holds or the deadline passes. Returns whether
  // the predicate held; the session verdict is reported through `alive`.
  template <typename Pred>
  bool pump(Pred pred, int timeoutMs = 3000, bool* alive = nullptr)
  {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;)
    {
      if (pred())
      {
        return true;
      }
      if (std::chrono::steady_clock::now() >= deadline)
      {
        return false;
      }
      std::string msg;
      const auto s = tcp.read(msg);
      if (s == fix::FixTcpClient::Status::Closed)
      {
        if (alive != nullptr)
        {
          *alive = false;
        }
        return pred();
      }
      if (s == fix::FixTcpClient::Status::Frame)
      {
        if (dropInbound > 0)
        {
          --dropInbound;  // the frame is gone before the session layer sees it
          continue;
        }
        if (initiator.onFrame(msg, wallNs()) == fix::FixInitiator::Verdict::Disconnect)
        {
          if (alive != nullptr)
          {
            *alive = false;
          }
          return pred();
        }
      }
      if (!initiator.onTick(wallNs()))
      {
        if (alive != nullptr)
        {
          *alive = false;
        }
        return pred();
      }
    }
  }

  bool logon(int port)
  {
    return connect(port) && initiator.connect(wallNs()) &&
           pump([this]
                { return initiator.loggedOn(); });
  }

  size_t execReports() const
  {
    size_t n = 0;
    for (const auto& r : reports)
    {
      if (std::holds_alternative<fix::ExecutionReport>(r))
      {
        ++n;
      }
    }
    return n;
  }

  const fix::ExecutionReport* execAt(size_t idx) const
  {
    size_t n = 0;
    for (const auto& r : reports)
    {
      if (const auto* e = std::get_if<fix::ExecutionReport>(&r))
      {
        if (n++ == idx)
        {
          return e;
        }
      }
    }
    return nullptr;
  }
};

fix::NewOrderRequest order(uint64_t id, Side side, const char* price, const char* quantity)
{
  fix::NewOrderRequest o;
  o.clOrdId = id;
  o.symbol = SYM;
  o.side = side;
  o.type = OrderType::LIMIT;
  int64_t raw = 0;
  CHECK(flox::decwire::parse(price, raw));
  o.price = Price::fromRaw(raw);
  CHECK(flox::decwire::parse(quantity, raw));
  o.quantity = Quantity::fromRaw(raw);
  return o;
}

// (1) Logon, an order, a typed ExecutionReport back, a heartbeat each way,
// a clean Logout. The whole happy path, both ends real.
void test_logon_order_report_logout()
{
  std::printf("test_logon_order_report_logout\n");
  Venue v;
  auto gw = v.gateway(1);
  const int port = gw->start(0, v.handler());
  CHECK(port > 0);

  Client c;
  CHECK(c.logon(port));
  CHECK(c.initiator.negotiatedHeartBtIntSec() == 1);  // the acceptor's number governs
  CHECK(c.initiator.seqState().expectedIn == 2);      // Logon reply at 34=1, consumed

  CHECK(c.initiator.submit(order(1, Side::SELL, "100.25", "5"), wallNs()));
  CHECK(c.pump([&c]
               { return c.execReports() >= 1; }));

  const fix::ExecutionReport* e = c.execAt(0);
  CHECK(e != nullptr);
  if (e != nullptr)
  {
    CHECK(e->orderId == 1);
    CHECK(e->clOrdId == 1);
    CHECK(e->symbol == SYM);
    CHECK(e->hasSide && e->side == Side::SELL);
    CHECK(e->execType == fix::ExecType::New);
    CHECK(e->ordStatus == "0");
    // The number that went out as 100.25 comes back as 100.25, not 100.250000
    // and not 100.24999999.
    std::string back;
    flox::decwire::append(back, e->price.raw());
    CHECK(back == "100.25");
    CHECK(e->price.raw() == px(100.25).raw());
    std::string leaves;
    flox::decwire::append(leaves, e->leavesQty.raw());
    CHECK(leaves == "5");
  }

  // The venue heartbeats on its own timer once HeartBtInt of silence passes,
  // and our onTick answers its TestRequest; the session survives the idle
  // window rather than being torn down by it.
  bool alive = true;
  c.pump([]
         { return false; }, 2500, &alive);
  CHECK(alive);
  CHECK(c.initiator.loggedOn());

  CHECK(c.initiator.logout("done", wallNs()));
  alive = true;
  c.pump([]
         { return false; }, 1500, &alive);
  CHECK(!alive);  // the venue confirms the Logout and closes
  c.tcp.close();
  gw->stop();
}

// (2) Every message the client encodes decodes on the venue side with each
// field intact -- not "the venue accepted it", but the venue's own decoder
// returning the values that went in.
void test_outbound_round_trip_through_venue_decoder()
{
  std::printf("test_outbound_round_trip_through_venue_decoder\n");

  fix::NewOrderRequest o;
  o.clOrdId = 77;
  o.accountId = 9;
  o.symbol = 3;
  o.side = Side::SELL;
  o.type = OrderType::LIMIT;
  o.price = px(100.25);
  o.quantity = qty(12.5);
  o.tif = TimeInForce::IOC;
  o.postOnly = true;
  o.reduceOnly = true;
  o.visibleQuantity = qty(2.5);
  const std::string dMsg =
      fix::ClientCodec::encodeSequenced(o, 7, "CLIENT", "VENUE", fix::sendingTime(wallNs()));
  CHECK(FixCodec::checksumValid(dMsg));  // the venue validates OUR checksum
  CHECK(FixCodec::msgSeqNum(dMsg) == 7);
  auto fields = FixCodec::parseFields(dMsg);
  CHECK(fields[44] == "100.25");  // on the wire, exactly
  CHECK(fields[38] == "12.5");
  CHECK(fields[111] == "2.5");
  CHECK(fields[49] == "CLIENT" && fields[56] == "VENUE" && fields.count(52) != 0);

  auto in = FixCodec::decode(dMsg);
  CHECK(in.has_value());
  if (in)
  {
    const auto* n = std::get_if<NewOrder>(&*in);
    CHECK(n != nullptr);
    if (n != nullptr)
    {
      CHECK(n->id == 77 && n->clientOrderId == 77);
      CHECK(n->accountId == 9);
      CHECK(n->symbol == 3);
      CHECK(n->side == Side::SELL);
      CHECK(n->type == OrderType::LIMIT);
      CHECK(n->price.raw() == px(100.25).raw());
      CHECK(n->quantity.raw() == qty(12.5).raw());
      CHECK(n->tif == TimeInForce::IOC);
      CHECK(n->postOnly);
      CHECK(n->reduceOnly);
      CHECK(n->visibleQuantity.raw() == qty(2.5).raw());
    }
  }

  fix::NewOrderRequest stop;
  stop.clOrdId = 78;
  stop.symbol = 3;
  stop.side = Side::BUY;
  stop.type = OrderType::STOP_LIMIT;
  stop.price = px(101.5);
  stop.quantity = qty(1);
  stop.triggerPrice = px(101);
  auto stopIn = FixCodec::decode(
      fix::ClientCodec::encodeSequenced(stop, 8, "CLIENT", "VENUE", fix::sendingTime(wallNs())));
  CHECK(stopIn.has_value());
  if (stopIn)
  {
    const auto* n = std::get_if<NewOrder>(&*stopIn);
    CHECK(n != nullptr && n->type == OrderType::STOP_LIMIT);
    CHECK(n != nullptr && n->triggerPrice.raw() == px(101).raw());
  }

  fix::CancelRequest cxl;
  cxl.origClOrdId = 77;
  cxl.clOrdId = 100;
  cxl.symbol = 3;
  cxl.side = Side::SELL;
  cxl.accountId = 9;
  auto cin = FixCodec::decode(
      fix::ClientCodec::encodeSequenced(cxl, 9, "CLIENT", "VENUE", fix::sendingTime(wallNs())));
  CHECK(cin.has_value());
  if (cin)
  {
    const auto* k = std::get_if<CancelOrder>(&*cin);
    CHECK(k != nullptr);
    if (k != nullptr)
    {
      CHECK(k->id == 77);  // the venue acts on OrigClOrdID, not on our new id
      CHECK(k->symbol == 3);
      CHECK(k->accountId == 9);
    }
  }

  fix::CancelReplaceRequest rep;
  rep.origClOrdId = 77;
  rep.clOrdId = 101;
  rep.symbol = 3;
  rep.side = Side::SELL;
  rep.price = px(99.75);
  rep.quantity = qty(4);
  rep.accountId = 9;
  auto rin = FixCodec::decode(
      fix::ClientCodec::encodeSequenced(rep, 10, "CLIENT", "VENUE", fix::sendingTime(wallNs())));
  CHECK(rin.has_value());
  if (rin)
  {
    const auto* m = std::get_if<ModifyOrder>(&*rin);
    CHECK(m != nullptr);
    if (m != nullptr)
    {
      CHECK(m->id == 77);
      CHECK(m->newPrice.raw() == px(99.75).raw());
      CHECK(m->newQty.raw() == qty(4).raw());
      CHECK(m->accountId == 9);
    }
  }
}

// (3) Every report the venue encodes decodes on the client side with each
// field intact -- the last-look surface included, which is the part a generic
// FIX client would drop on the floor.
void test_inbound_round_trip_from_venue_encoder()
{
  std::printf("test_inbound_round_trip_from_venue_encoder\n");
  const std::string now52 = fix::sendingTime(wallNs());

  OrderAccepted acc;
  acc.id = 5;
  acc.symbol = 2;
  acc.side = Side::BUY;
  acc.price = px(100.25);
  acc.leavesQty = qty(3);
  auto r1 = fix::ClientCodec::decode(
      FixCodec::encode(OutboundEvent{acc}, 4, "VENUE", "CLIENT", now52));
  CHECK(r1.has_value());
  if (r1)
  {
    const auto* e = std::get_if<fix::ExecutionReport>(&*r1);
    CHECK(e != nullptr);
    if (e != nullptr)
    {
      CHECK(e->orderId == 5 && e->clOrdId == 5 && e->symbol == 2);
      CHECK(e->hasSide && e->side == Side::BUY);
      CHECK(e->execType == fix::ExecType::New && e->ordStatus == "0");
      CHECK(e->price.raw() == px(100.25).raw());
      CHECK(e->leavesQty.raw() == qty(3).raw());
      std::string s;
      flox::decwire::append(s, e->price.raw());
      CHECK(s == "100.25");
    }
  }

  OrderExecuted ex;
  ex.id = 5;
  ex.symbol = 2;
  ex.lastQty = qty(1.5);
  ex.lastPx = px(100.25);
  ex.leavesQty = qty(1.5);
  ex.complete = false;
  auto r2 = fix::ClientCodec::decode(
      FixCodec::encode(OutboundEvent{ex}, 5, "VENUE", "CLIENT", now52));
  CHECK(r2.has_value());
  if (r2)
  {
    const auto* e = std::get_if<fix::ExecutionReport>(&*r2);
    CHECK(e != nullptr);
    if (e != nullptr)
    {
      CHECK(e->execType == fix::ExecType::Trade && e->ordStatus == "1");
      CHECK(e->lastQty.raw() == qty(1.5).raw());
      CHECK(e->lastPx.raw() == px(100.25).raw());
      CHECK(e->avgPx.raw() == px(100.25).raw());
      CHECK(e->leavesQty.raw() == qty(1.5).raw());
    }
  }

  OrderCanceled can;
  can.id = 5;
  auto r3 = fix::ClientCodec::decode(
      FixCodec::encode(OutboundEvent{can}, 6, "VENUE", "CLIENT", now52));
  CHECK(r3.has_value());
  if (r3)
  {
    const auto* e = std::get_if<fix::ExecutionReport>(&*r3);
    CHECK(e != nullptr && e->execType == fix::ExecType::Canceled && e->ordStatus == "4");
  }

  OrderModified mod;
  mod.id = 5;
  mod.price = px(99.5);
  mod.leavesQty = qty(2);
  auto r4 = fix::ClientCodec::decode(
      FixCodec::encode(OutboundEvent{mod}, 7, "VENUE", "CLIENT", now52));
  CHECK(r4.has_value());
  if (r4)
  {
    const auto* e = std::get_if<fix::ExecutionReport>(&*r4);
    CHECK(e != nullptr && e->execType == fix::ExecType::Replaced);
    CHECK(e != nullptr && e->price.raw() == px(99.5).raw());
    CHECK(e != nullptr && e->leavesQty.raw() == qty(2).raw());
  }

  OrderRejected rej;
  rej.id = 5;
  rej.reason = RejectReason::UnknownOrder;
  auto r5 = fix::ClientCodec::decode(
      FixCodec::encode(OutboundEvent{rej}, 8, "VENUE", "CLIENT", now52));
  CHECK(r5.has_value());
  if (r5)
  {
    const auto* e = std::get_if<fix::ExecutionReport>(&*r5);
    CHECK(e != nullptr && e->execType == fix::ExecType::Rejected);
    CHECK(e != nullptr && e->text == std::string(toString(RejectReason::UnknownOrder)));
  }

  // 35=9, a different message from an exec report: the client has to see it as
  // one, or a refused cancel looks like an order state change.
  CancelRejected cr;
  cr.id = 5;
  cr.reason = RejectReason::UnknownOrder;
  cr.wasReplace = true;
  auto r6 = fix::ClientCodec::decode(
      FixCodec::encode(OutboundEvent{cr}, 9, "VENUE", "CLIENT", now52));
  CHECK(r6.has_value());
  if (r6)
  {
    const auto* k = std::get_if<fix::CancelReject>(&*r6);
    CHECK(k != nullptr);
    if (k != nullptr)
    {
      CHECK(k->orderId == 5 && k->origClOrdId == 5);
      CHECK(k->responseTo == 2);  // answering a 35=G
      CHECK(k->reason == 1);      // unknown order
      CHECK(k->ordStatus == "8");
      CHECK(k->text == std::string(toString(RejectReason::UnknownOrder)));
    }
  }

  // The last-look pair. 150=U says a fill is held pending the maker's
  // confirmation and 20001/20002 identify it; 150=H says the held fill will
  // not stand. A client that read neither would treat the hold as a plain
  // working order and never learn how it resolved.
  FillHeld fh;
  fh.takerId = 11;
  fh.makerId = 12;
  fh.heldId = 99;
  fh.symbol = 2;
  fh.qty = qty(2);
  fh.price = px(100.25);
  auto r7 = fix::ClientCodec::decode(
      FixCodec::encode(OutboundEvent{fh}, 10, "VENUE", "CLIENT", now52));
  CHECK(r7.has_value());
  if (r7)
  {
    const auto* e = std::get_if<fix::ExecutionReport>(&*r7);
    CHECK(e != nullptr);
    if (e != nullptr)
    {
      CHECK(e->execType == fix::ExecType::FillHeld && e->execTypeRaw == "U");
      CHECK(e->ordStatus == "0");  // still working: nothing has executed
      CHECK(e->heldId == 99 && e->makerId == 12);
      CHECK(e->orderId == 11);
      CHECK(e->lastQty.raw() == qty(2).raw());
      CHECK(e->lastPx.raw() == px(100.25).raw());
    }
  }

  FillRejected fr;
  fr.takerId = 11;
  fr.makerId = 12;
  fr.heldId = 99;
  fr.symbol = 2;
  fr.qty = qty(2);
  fr.price = px(100.25);
  auto r8 = fix::ClientCodec::decode(
      FixCodec::encode(OutboundEvent{fr}, 11, "VENUE", "CLIENT", now52));
  CHECK(r8.has_value());
  if (r8)
  {
    const auto* e = std::get_if<fix::ExecutionReport>(&*r8);
    CHECK(e != nullptr);
    if (e != nullptr)
    {
      CHECK(e->execType == fix::ExecType::TradeCancel && e->execTypeRaw == "H");
      CHECK(e->heldId == 99 && e->makerId == 12);
      CHECK(e->text == "LastLookRejected");
    }
  }
}

// (4) A lost outbound message. The venue detects the hole, sends 35=2, and the
// initiator replays the lost order with 43=Y and 122 at its ORIGINAL 34,
// gap-filling the seqs its log does not hold. The venue then applies the order.
void test_outbound_gap_served_with_possdup_replay()
{
  std::printf("test_outbound_gap_served_with_possdup_replay\n");
  Venue v;
  auto gw = v.gateway(1);
  const int port = gw->start(0, v.handler());
  CHECK(port > 0);

  Client c;
  CHECK(c.logon(port));

  c.dropOutbound = 1;
  CHECK(c.initiator.submit(order(1, Side::SELL, "100.25", "5"), wallNs()));  // lost on the wire

  // Nothing above the hole has reached the venue yet, so it has no reason to
  // suspect one. The next thing it sees is our idle Heartbeat, whose 34 sits a
  // step past what it expects -- that is what opens the gap, and it also leaves
  // the initiator's resend log with a seq it does not hold, because admin
  // traffic is sequenced and not logged.
  CHECK(c.pump(
      [&c]
      {
        for (const std::string& m : c.sent)
        {
          if (flox::fix::parseFields(m)[35] == "0")
          {
            return true;
          }
        }
        return false;
      },
      4000));

  // The venue answers the hole with 35=2 and the initiator replays. The order
  // being accepted is the proof the replay closed the gap: until it does, the
  // venue drops everything above the hole.
  CHECK(c.pump([&c]
               { return c.execReports() >= 1; }, 5000));
  const fix::ExecutionReport* first = c.execAt(0);
  CHECK(first != nullptr && first->orderId == 1);
  CHECK(first != nullptr && first->price.raw() == px(100.25).raw());

  // What actually went back on the wire: the replayed 35=D at its ORIGINAL 34
  // with PossDupFlag and OrigSendingTime, and a SequenceReset-GapFill over the
  // Heartbeat seq the log does not hold.
  bool sawPossDupOrder = false;
  bool sawGapFill = false;
  for (const std::string& m : c.sent)
  {
    auto f = flox::fix::parseFields(m);
    if (f[35] == "D" && f[43] == "Y")
    {
      sawPossDupOrder = true;
      CHECK(f.count(122) != 0);
      CHECK(f[11] == "1");
      CHECK(f[44] == "100.25");  // re-encoded, and still exactly 100.25
      CHECK(f[34] == "2");       // the original seq, not a fresh one
    }
    if (f[35] == "4" && f[123] == "Y")
    {
      sawGapFill = true;
      CHECK(f[43] == "Y");
      CHECK(f[34] == "3");  // the Heartbeat's seq
      CHECK(f[36] == "4");
    }
  }
  CHECK(sawPossDupOrder);
  CHECK(sawGapFill);

  // And the session is usable afterwards: the sequence space is whole again.
  CHECK(c.initiator.submit(order(2, Side::SELL, "101.5", "5"), wallNs()));
  CHECK(c.pump([&c]
               { return c.execReports() >= 2; }, 4000));
  const fix::ExecutionReport* second = c.execAt(1);
  CHECK(second != nullptr && second->orderId == 2);

  c.tcp.close();
  gw->stop();
}

// (5) A lost inbound message. The initiator sends its own 35=2, refuses to
// apply anything above the hole, and applies the venue's PossDup replay
// exactly once -- in order, and without duplicating the report it already had.
void test_inbound_gap_requests_resend_and_applies_once()
{
  std::printf("test_inbound_gap_requests_resend_and_applies_once\n");
  Venue v;
  auto gw = v.gateway(1);
  const int port = gw->start(0, v.handler());
  CHECK(port > 0);

  Client c;
  CHECK(c.logon(port));

  c.dropInbound = 1;  // the next frame the venue sends never reaches the session
  CHECK(c.initiator.submit(order(1, Side::SELL, "100.25", "5"), wallNs()));
  CHECK(c.initiator.submit(order(2, Side::SELL, "101.5", "5"), wallNs()));

  CHECK(c.pump([&c]
               { return c.execReports() >= 2; }, 5000));
  CHECK(c.execReports() == 2);

  // Exactly once each, and in sequence order: the report above the hole was
  // dropped on arrival and only applied after the replay closed the gap.
  const fix::ExecutionReport* first = c.execAt(0);
  const fix::ExecutionReport* second = c.execAt(1);
  CHECK(first != nullptr && first->orderId == 1);
  CHECK(second != nullptr && second->orderId == 2);

  bool sawResendRequest = false;
  for (const std::string& m : c.sent)
  {
    auto f = flox::fix::parseFields(m);
    if (f[35] == "2")
    {
      sawResendRequest = true;
      CHECK(f.count(7) != 0 && f[16] == "0");
    }
  }
  CHECK(sawResendRequest);

  c.tcp.close();
  gw->stop();
}

// (6) Disconnect and reconnect without 141=Y, continuing the sequence space --
// and the counters carried across a restart of the initiator object through
// the sidecar, the way a process restart would carry them.
void test_reconnect_continues_sequence_through_sidecar()
{
  std::printf("test_reconnect_continues_sequence_through_sidecar\n");
  Venue v;
  auto gw = v.gateway(1);
  const int port = gw->start(0, v.handler());
  CHECK(port > 0);

  const std::string path = tmpPath("fix_initiator", ".seq");
  ::unlink(path.c_str());
  fix::FixSeqState saved;

  {
    Client c;
    c.initiator.setSeqObserver([&saved](const fix::FixSeqState& s)
                               { saved = s; });
    CHECK(c.logon(port));
    CHECK(c.initiator.submit(order(1, Side::SELL, "100.25", "5"), wallNs()));
    CHECK(c.pump([&c]
                 { return c.execReports() >= 1; }));
    CHECK(fix::FixInitiatorSidecar::write(path, c.initiator.seqState()));
    c.tcp.close();  // a hard disconnect, no Logout
  }

  fix::FixSeqState restored;
  CHECK(fix::FixInitiatorSidecar::load(path, restored));
  CHECK(restored.nextOut == saved.nextOut && restored.expectedIn == saved.expectedIn);
  CHECK(restored.nextOut > 2);

  // A fresh initiator, as a restarted process would build it: restore the
  // counters, log on WITHOUT 141=Y, and keep trading in the same space.
  Client c2;
  c2.initiator.restore(restored);
  CHECK(c2.connect(port));
  CHECK(c2.initiator.connect(wallNs()));
  CHECK(c2.pump([&c2]
                { return c2.initiator.loggedOn(); }));
  CHECK(c2.initiator.submit(order(2, Side::SELL, "101.25", "5"), wallNs()));
  CHECK(c2.pump([&c2]
                { return c2.execReports() >= 1; }));
  const fix::ExecutionReport* e = c2.execAt(0);
  CHECK(e != nullptr && e->orderId == 2);

  // No 141=Y went out on the second Logon: the point of the sidecar is that
  // the space continues rather than restarting.
  bool sawReset = false;
  for (const std::string& m : c2.sent)
  {
    auto f = flox::fix::parseFields(m);
    if (f[35] == "A" && f[141] == "Y")
    {
      sawReset = true;
    }
  }
  CHECK(!sawReset);

  c2.tcp.close();
  gw->stop();
  ::unlink(path.c_str());
}

// (7) A resting order is cancelled and replaced through the session, and the
// venue's answers decode as the right message types on the client side.
void test_cancel_and_replace_round_trip()
{
  std::printf("test_cancel_and_replace_round_trip\n");
  Venue v;
  auto gw = v.gateway(1);
  const int port = gw->start(0, v.handler());
  CHECK(port > 0);

  Client c;
  CHECK(c.logon(port));
  CHECK(c.initiator.submit(order(1, Side::SELL, "100.25", "5"), wallNs()));
  CHECK(c.pump([&c]
               { return c.execReports() >= 1; }));

  fix::CancelReplaceRequest rep;
  rep.origClOrdId = 1;
  rep.clOrdId = 1;
  rep.symbol = SYM;
  rep.side = Side::SELL;
  rep.price = px(101.75);
  rep.quantity = qty(4);
  CHECK(c.initiator.replace(rep, wallNs()));
  CHECK(c.pump([&c]
               { return c.execReports() >= 2; }));
  const fix::ExecutionReport* mod = c.execAt(1);
  CHECK(mod != nullptr && mod->execType == fix::ExecType::Replaced);
  CHECK(mod != nullptr && mod->price.raw() == px(101.75).raw());

  fix::CancelRequest cxl;
  cxl.origClOrdId = 1;
  cxl.clOrdId = 1;
  cxl.symbol = SYM;
  cxl.side = Side::SELL;
  CHECK(c.initiator.cancel(cxl, wallNs()));
  CHECK(c.pump([&c]
               { return c.execReports() >= 3; }));
  const fix::ExecutionReport* can = c.execAt(2);
  CHECK(can != nullptr && can->execType == fix::ExecType::Canceled);

  // A cancel for an order the venue never had comes back as 35=9, not as an
  // exec report: the client must see the difference.
  fix::CancelRequest ghost;
  ghost.origClOrdId = 4242;
  ghost.clOrdId = 4242;
  ghost.symbol = SYM;
  ghost.side = Side::SELL;
  CHECK(c.initiator.cancel(ghost, wallNs()));
  CHECK(c.pump(
      [&c]
      {
        for (const auto& r : c.reports)
        {
          if (std::holds_alternative<fix::CancelReject>(r))
          {
            return true;
          }
        }
        return false;
      }));

  c.tcp.close();
  gw->stop();
}

// (8) Submit-to-bytes latency, p50 and p99, measured across the whole path the
// caller pays for: encode, sequence, frame, write.
void test_submit_to_wire_latency()
{
  std::printf("test_submit_to_wire_latency\n");
  Venue v;
  auto gw = v.gateway(1);
  const int port = gw->start(0, v.handler());
  CHECK(port > 0);

  Client c;
  CHECK(c.logon(port));
  // Short receive timeout: the drain below must not pay a full idle window on
  // every pass, or the wait shows up as latency the initiator never spent.
  const timeval fastDrain{0, 1000};
  ::setsockopt(c.tcp.fd(), SOL_SOCKET, SO_RCVTIMEO, &fastDrain, sizeof fastDrain);

  constexpr int kWarmup = 500;
  constexpr int kRuns = 5000;
  auto percentiles = [](std::vector<int64_t>& s, const char* label)
  {
    std::sort(s.begin(), s.end());
    const int64_t p50 = s[s.size() / 2];
    const int64_t p99 = s[s.size() * 99 / 100];
    std::printf("  %s: p50 %lld ns, p99 %lld ns (n=%zu)\n", label, static_cast<long long>(p50),
                static_cast<long long>(p99), s.size());
    return p50;
  };

  std::vector<int64_t> wire;
  wire.reserve(kRuns);
  for (int i = 0; i < kWarmup + kRuns; ++i)
  {
    fix::NewOrderRequest o = order(static_cast<uint64_t>(1000 + i), Side::SELL, "100.25", "1");
    const auto t0 = std::chrono::steady_clock::now();
    c.initiator.submit(o, wallNs());
    const auto t1 = std::chrono::steady_clock::now();
    if (i >= kWarmup)
    {
      wire.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }
    if ((i % 256) == 0)
    {
      // Drain, so a full socket buffer does not show up as codec cost. The read
      // is non-blocking here: a blocking drain would add its own timeout to
      // every window and swamp the measurement it exists to protect.
      std::string msg;
      while (c.tcp.read(msg) == fix::FixTcpClient::Status::Frame)
      {
      }
    }
  }
  CHECK(percentiles(wire, "submit -> bytes handed to the socket") > 0);
  c.tcp.close();
  gw->stop();

  // The same call with the write taken out, which separates what the session
  // and codec cost from what the kernel costs. Useful because only the first
  // number is the initiator's to improve.
  fix::FixInitiator solo{Client::defaults()};
  size_t sunk = 0;
  solo.setSend(
      [&sunk](const std::string& m)
      {
        sunk += m.size();
        return true;
      });
  solo.connect(wallNs());
  solo.onFrame(fix::encodeAdmin("A", 1, "VENUE", "CLIENT", fix::sendingTime(wallNs()),
                                {{108, "1"}, {141, "Y"}}),
               wallNs());
  CHECK(solo.loggedOn());
  std::vector<int64_t> encodeOnly;
  encodeOnly.reserve(kRuns);
  for (int i = 0; i < kWarmup + kRuns; ++i)
  {
    fix::NewOrderRequest o = order(static_cast<uint64_t>(1000 + i), Side::SELL, "100.25", "1");
    const auto t0 = std::chrono::steady_clock::now();
    solo.submit(o, wallNs());
    const auto t1 = std::chrono::steady_clock::now();
    if (i >= kWarmup)
    {
      encodeOnly.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }
  }
  CHECK(percentiles(encodeOnly, "submit -> encoded and sequenced, no write") > 0);
  CHECK(sunk > 0);
}

}  // namespace

TEST(FixInitiator, AgainstVenueAcceptor)
{
  test_logon_order_report_logout();
  test_outbound_round_trip_through_venue_decoder();
  test_inbound_round_trip_from_venue_encoder();
  test_outbound_gap_served_with_possdup_replay();
  test_inbound_gap_requests_resend_and_applies_once();
  test_reconnect_continues_sequence_through_sidecar();
  test_cancel_and_replace_round_trip();
  test_submit_to_wire_latency();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
