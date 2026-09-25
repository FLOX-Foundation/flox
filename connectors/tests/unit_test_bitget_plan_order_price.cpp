/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline order-serialization tests for the two ways the Bitget executor
 * loses information on its conditional-order paths:
 *
 *  - submitPlanOrder writes "orderType":"market" unconditionally and never
 *    sends a price, so STOP_LIMIT and TAKE_PROFIT_LIMIT reach the venue as
 *    stop-market. The strategy asked for a bounded execution price and got
 *    an unbounded one, with no rejection and no event to say so --
 *    docs/explanation/connectors.md is explicit that an unsupported order
 *    must be rejected rather than sent as an approximation.
 *
 *  - every trigger price goes through trimDouble() with its default of one
 *    fractional digit, an assumption its own comment spells out ("BTC perp
 *    = 1 digit"). The instrument's precision is in the registry the
 *    executor already holds (SymbolInfo::tickSize); on anything finer than
 *    0.1 the protective trigger is silently moved, and on a symbol priced
 *    below ~0.05 it is sent as "0".
 *
 * Both are asserted on the exact bytes handed to the transport, because
 * that string is the entire contract with the venue.
 */

#include "flox-connectors/bitget/authenticated_rest_client.h"
#include "flox-connectors/bitget/bitget_order_executor.h"

#include <flox/common.h>
#include <flox/engine/symbol_registry.h>
#include <flox/execution/order.h>
#include <flox/execution/order_tracker.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using namespace flox;

namespace
{

struct Call
{
  std::string url;
  std::string body;
};

class FakeTransport final : public ITransport
{
 public:
  void post(std::string_view url, std::string_view body,
            const std::vector<std::pair<std::string_view, std::string_view>>&,
            MoveOnlyFunction<void(std::string_view)> onSuccess,
            MoveOnlyFunction<void(std::string_view)>) override
  {
    calls.push_back({std::string(url), std::string(body)});
    if (onSuccess)
    {
      onSuccess(nextResponse);
    }
  }

  std::vector<Call> calls;
  std::string nextResponse = R"({"code":"00000","msg":"success","data":{"orderId":"BG-1"}})";
};

// The pos-tpsl endpoint answers with an array, not an object.
constexpr const char* kPosTpslResponse =
    R"({"code":"00000","msg":"success","data":[{"orderId":"BG-TPSL-1"}]})";

Bitget::Params baseParams()
{
  Bitget::Params p;
  p.productType = "USDT-FUTURES";
  p.marginCoin = "USDT";
  p.marginMode = "crossed";
  p.positionMode = Bitget::PositionMode::OneWay;
  return p;
}

SymbolId registerSymbol(SymbolRegistry& registry, const std::string& symbol, double tickSize)
{
  SymbolInfo info;
  info.exchange = "bitget";
  info.symbol = symbol;
  info.type = InstrumentType::Future;
  info.tickSize = Price::fromDouble(tickSize);
  return registry.registerSymbol(info);
}

struct Harness
{
  std::unique_ptr<FakeTransport> owner{std::make_unique<FakeTransport>()};
  FakeTransport* transport{owner.get()};
  SymbolRegistry registry;
  OrderTracker tracker;
  std::unique_ptr<BitgetOrderExecutorT<NoPolicies>> executor;

  Harness()
  {
    auto client = std::make_unique<BitgetAuthenticatedRestClient>(
        "k", "s", "p", "https://api.bitget.com", transport);
    executor = std::make_unique<BitgetOrderExecutorT<NoPolicies>>(std::move(client), &registry,
                                                                  &tracker, baseParams());
  }
};

}  // namespace

// A STOP_LIMIT is a stop with a limit price. The limit price is the whole
// difference between it and a STOP_MARKET, and it is the leg that bounds
// slippage on a protective exit.
TEST(BitgetPlanOrderPrice, StopLimitCarriesItsLimitPrice)
{
  Harness h;
  SymbolId sym = registerSymbol(h.registry, "BTCUSDT", 0.1);

  Order order;
  order.id = 11;
  order.symbol = sym;
  order.side = Side::SELL;
  order.type = OrderType::STOP_LIMIT;
  order.quantity = Quantity::fromDouble(1.0);
  order.triggerPrice = Price::fromDouble(58000.0);
  order.price = Price::fromDouble(57900.0);
  order.flags.reduceOnly = 1;

  h.executor->submitOrder(order);

  ASSERT_EQ(h.transport->calls.size(), 1u);
  const std::string& body = h.transport->calls[0].body;

  EXPECT_NE(body.find(R"("planType":"normal_plan")"), std::string::npos)
      << "the plan type must be the one that carries an execution price: " << body;
  EXPECT_NE(body.find(R"("triggerPrice":"58000)"), std::string::npos) << body;
  EXPECT_NE(body.find(R"("orderType":"limit")"), std::string::npos)
      << "a STOP_LIMIT sent as market is a silent downgrade to stop-market: " << body;
  EXPECT_NE(body.find(R"("price":"57900)"), std::string::npos)
      << "the limit price never reached the venue: " << body;
}

TEST(BitgetPlanOrderPrice, TakeProfitLimitCarriesItsLimitPrice)
{
  Harness h;
  SymbolId sym = registerSymbol(h.registry, "BTCUSDT", 0.1);

  Order order;
  order.id = 12;
  order.symbol = sym;
  order.side = Side::SELL;
  order.type = OrderType::TAKE_PROFIT_LIMIT;
  order.quantity = Quantity::fromDouble(2.0);
  order.triggerPrice = Price::fromDouble(71000.0);
  order.price = Price::fromDouble(71100.0);
  order.flags.reduceOnly = 1;

  h.executor->submitOrder(order);

  ASSERT_EQ(h.transport->calls.size(), 1u);
  const std::string& body = h.transport->calls[0].body;

  EXPECT_NE(body.find(R"("orderType":"limit")"), std::string::npos) << body;
  EXPECT_NE(body.find(R"("price":"71100)"), std::string::npos) << body;
}

// Control, green today: the market variants have no limit price and must
// keep going out as market with no price field. Whatever carries the limit
// price for STOP_LIMIT must not leak into these.
TEST(BitgetPlanOrderPrice, StopMarketStaysMarketWithNoPrice)
{
  Harness h;
  SymbolId sym = registerSymbol(h.registry, "BTCUSDT", 0.1);

  Order order;
  order.id = 13;
  order.symbol = sym;
  order.side = Side::SELL;
  order.type = OrderType::STOP_MARKET;
  order.quantity = Quantity::fromDouble(1.0);
  order.triggerPrice = Price::fromDouble(58000.0);
  order.flags.reduceOnly = 1;

  h.executor->submitOrder(order);

  ASSERT_EQ(h.transport->calls.size(), 1u);
  const std::string& body = h.transport->calls[0].body;
  EXPECT_NE(body.find(R"("orderType":"market")"), std::string::npos) << body;
  EXPECT_EQ(body.find(R"("price":")"), std::string::npos)
      << "a stop-market has no limit price to send: " << body;
}

// The registry already knows this instrument quotes to 1e-8. Rounding the
// trigger to one digit moves a protective stop by up to 0.05 of quote
// currency and, on a venue that validates against the symbol's precision,
// is a different price from the one the strategy computed.
TEST(BitgetPlanOrderPrice, PosTpslTriggerKeepsInstrumentPrecision)
{
  Harness h;
  h.transport->nextResponse = kPosTpslResponse;
  SymbolId sym = registerSymbol(h.registry, "PEPEUSDT", 0.00000001);

  h.executor->placePosTpsl(sym, HoldSide::Long, 67123.45678901, 0.0, 21);

  ASSERT_EQ(h.transport->calls.size(), 1u);
  const std::string& body = h.transport->calls[0].body;
  EXPECT_NE(body.find(R"("stopLossTriggerPrice":"67123.45678901")"), std::string::npos)
      << "the trigger was reformatted at a precision the instrument does not use: " << body;
}

// The same rounding on a symbol whose price lives below the rounding
// granularity destroys the trigger outright rather than nudging it.
TEST(BitgetPlanOrderPrice, PosTpslTriggerSurvivesOnALowPricedSymbol)
{
  Harness h;
  h.transport->nextResponse = kPosTpslResponse;
  SymbolId sym = registerSymbol(h.registry, "DOGEUSDT", 0.0001);

  h.executor->placePosTpsl(sym, HoldSide::Short, 0.08423, 0.0, 22);

  ASSERT_EQ(h.transport->calls.size(), 1u);
  const std::string& body = h.transport->calls[0].body;
  EXPECT_NE(body.find(R"("stopLossTriggerPrice":"0.0842")"), std::string::npos)
      << "a trigger below the hardcoded 0.1 granularity is rounded away entirely: " << body;
}

TEST(BitgetPlanOrderPrice, ModifyPosTpslTriggerKeepsInstrumentPrecision)
{
  Harness h;
  SymbolId sym = registerSymbol(h.registry, "PEPEUSDT", 0.00000001);

  h.executor->modifyPosTpsl(sym, "BG-TPSL-1", 67123.45678901, 1.0);

  ASSERT_EQ(h.transport->calls.size(), 1u);
  const std::string& body = h.transport->calls[0].body;
  EXPECT_NE(body.find(R"("triggerPrice":"67123.45678901")"), std::string::npos)
      << "walking a stop to a price the instrument supports must not round it: " << body;
}

// Control, green today: on an instrument that really does quote to 0.1 the
// wire format is unchanged. Precision must come from the registry, not be
// widened for everyone.
TEST(BitgetPlanOrderPrice, PosTpslTriggerOnACoarseInstrumentIsUnchanged)
{
  Harness h;
  h.transport->nextResponse = kPosTpslResponse;
  SymbolId sym = registerSymbol(h.registry, "BTCUSDT", 0.1);

  h.executor->placePosTpsl(sym, HoldSide::Long, 67123.45678901, 0.0, 23);

  ASSERT_EQ(h.transport->calls.size(), 1u);
  const std::string& body = h.transport->calls[0].body;
  EXPECT_NE(body.find(R"("stopLossTriggerPrice":"67123.5")"), std::string::npos) << body;
}
