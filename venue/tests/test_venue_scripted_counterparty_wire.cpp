/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * The scripted maker, seen from where a client actually stands.
 *
 * The scenario tests next door drive the engine directly, which is right for
 * what they assert: the maker's decision is an in-process command either way,
 * and the wire adds nothing to whether it refused on an adverse move. What
 * the wire DOES decide is whether the harness is usable for the thing it
 * exists for -- somebody developing a client against a last-look venue and
 * needing the other side to misbehave on demand.
 *
 * So one test, end to end: a real FIX client over TCP, a real gateway, a real
 * engine, and a scripted maker on the other side. The client sends an order
 * and sees a fill or a reject depending on nothing but the maker's policy.
 */
#include "flox-venue/fix_codec.h"
#include "flox-venue/fix_session.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/script/scripted_maker.h"
#include "flox-venue/session_registry.h"
#include "flox-venue/tcp_gateway.h"
#include "flox/connector/fix/fix_initiator.h"
#include "flox/connector/fix/fix_tcp_client.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace flox;
using namespace flox::venue;
using namespace flox::venue::script;

namespace
{

constexpr SymbolId SYM = 1;
constexpr uint64_t kMaker = 1;
constexpr uint64_t kClient = 9;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

venue::SymbolConfig cfg()
{
  venue::SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  c.lastLookWindowNs = DurationNs{1'000'000'000};
  return c;
}

NewOrder limit(OrderId id, Side s, double p, double q, uint64_t acct)
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

int64_t wallNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// A venue with a scripted maker wired into its outbound stream. The maker
// observes everything the engine emits and answers when told to.
struct ScriptedVenue
{
  GatewayCounters counters;
  SessionRegistry registry;
  std::mutex m;
  ScriptedMaker maker;
  MatchingEngine<MatchingBook> eng;
  FixSessionHost fixHost;
  int64_t ts{0};

  explicit ScriptedVenue(MakerPolicy policy)
      : registry(DeliveryConfig{}, &counters),
        maker(kMaker, policy),
        eng(cfg(),
            [this](const OutboundEvent& e)
            {
              maker.observe(e);
              registry.route(e);
            }),
        fixHost(fixCfg())
  {
    maker.setSymbol(SYM);
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
      const std::string msg =
          FixCodec::encode(e, seq, fc.senderCompId, fc.targetCompId, FixSession::sendingTime(tsNs));
      if (msg.empty())
      {
        return false;
      }
      out.assign(msg.begin(), msg.end());
      return true;
    };
  }

  std::unique_ptr<TcpGateway> gateway()
  {
    auto gw = std::make_unique<TcpGateway>(
        [](const uint8_t* p, size_t n)
        { return FixCodec::decode(std::string(reinterpret_cast<const char*>(p), n)); },
        kClient);
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
    eng.submit(c, ++ts);
  }

  // The maker quotes, non-firm, so a client that hits it is held.
  void quote(OrderId id, Side side, double price)
  {
    NewOrder mk = limit(id, side, price, 5, kMaker);
    mk.lastLook = true;
    submit(InboundCommand{mk});
  }

  void letTheMakerAnswer()
  {
    std::vector<InboundCommand> decisions;
    {
      std::lock_guard<std::mutex> lk(m);
      maker.decide(decisions);
    }
    for (const auto& d : decisions)
    {
      submit(d);
    }
  }
};

fix::FixInitiatorConfig clientCfg()
{
  fix::FixInitiatorConfig c;
  c.senderCompId = "CLIENT";
  c.targetCompId = "VENUE";
  c.heartBtIntSec = 1;
  c.resetSeqNumOnLogon = true;
  return c;
}

}  // namespace

// A client over the wire gets a fill when the scripted maker accepts, and a
// refusal when it does not -- with nothing changed but the policy.
TEST(ScriptedCounterpartyWire, AClientSeesTheMakersPolicyThroughTheGateway)
{
  for (const MakerPolicy policy : {MakerPolicy::AcceptAlways, MakerPolicy::RejectAlways})
  {
    ScriptedVenue venue(policy);
    auto gw = venue.gateway();
    const int port = gw->start(0, venue.handler());
    ASSERT_GT(port, 0);
    venue.quote(500, Side::SELL, 100.0);

    fix::FixInitiator initiator(clientCfg());
    fix::FixTcpClient tcp;
    std::vector<fix::InboundReport> reports;
    initiator.setSend([&tcp](const std::string& m)
                      { return tcp.send(m); });
    initiator.setReportHandler([&reports](const fix::InboundReport& r)
                               { reports.push_back(r); });

    ASSERT_TRUE(tcp.connect("127.0.0.1", static_cast<uint16_t>(port)));
    ASSERT_TRUE(initiator.connect(wallNs()));

    const auto pump = [&](int rounds)
    {
      for (int i = 0; i < rounds; ++i)
      {
        std::string msg;
        while (tcp.read(msg) == fix::FixTcpClient::Status::Frame)
        {
          initiator.onFrame(msg, wallNs());
        }
        initiator.onTick(wallNs());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    };
    pump(10);

    // The client takes the maker's quote. It is held, not printed, because
    // the maker is non-firm.
    fix::NewOrderRequest req;
    req.clOrdId = 7001;
    req.symbol = SYM;
    req.side = Side::BUY;
    req.type = OrderType::LIMIT;
    req.price = px(100.0);
    req.quantity = qty(1);
    ASSERT_TRUE(initiator.submit(req, wallNs()));
    pump(10);

    venue.letTheMakerAnswer();
    pump(20);

    // What the venue actually puts on the wire: an accepted hold prints as
    // ExecType=F (Trade), a refused one as the custom ExecType=H
    // (TradeCancel). Asserting both directions, because "no trade" alone
    // would also pass if the client simply heard nothing.
    bool sawTrade = false;
    bool sawTradeCancel = false;
    bool sawHeld = false;
    for (const auto& r : reports)
    {
      const auto* e = std::get_if<fix::ExecutionReport>(&r);
      if (e == nullptr)
      {
        continue;
      }
      sawHeld = sawHeld || e->execType == fix::ExecType::FillHeld;
      sawTrade = sawTrade || e->execType == fix::ExecType::Trade;
      sawTradeCancel = sawTradeCancel || e->execType == fix::ExecType::TradeCancel;
    }

    EXPECT_TRUE(sawHeld) << "the client was never told its fill was held: it hit a firm quote, "
                            "so the policy under test never came into play";
    if (policy == MakerPolicy::AcceptAlways)
    {
      EXPECT_TRUE(sawTrade) << "the maker accepted and the client never saw the print";
      EXPECT_FALSE(sawTradeCancel) << "the maker accepted and the client was told it was busted";
    }
    else
    {
      EXPECT_TRUE(sawTradeCancel) << "the maker refused and the client was never told";
      EXPECT_FALSE(sawTrade) << "the maker refused and the client was told it traded";
    }

    tcp.close();
    gw->stop();
  }
}
