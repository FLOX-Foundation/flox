/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline order-serialization tests for the Bitget executor. Confirms the
 * important negative result from the audit -- Bitget already translates
 * order type and triggerPrice correctly, that must not regress -- and
 * covers what actually needed fixing: force (time-in-force) driven by the
 * order instead of a static config value, and tradeSide/posSide
 * gated by an explicit account position mode instead of firing
 * unconditionally off reduceOnly alone.
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

SymbolId registerUsdtFutures(SymbolRegistry& registry, const std::string& symbol = "BTCUSDT")
{
  SymbolInfo info;
  info.exchange = "bitget";
  info.symbol = symbol;
  info.type = InstrumentType::Future;
  return registry.registerSymbol(info);
}

Bitget::Params baseParams(Bitget::PositionMode mode = Bitget::PositionMode::OneWay)
{
  Bitget::Params p;
  p.productType = "USDT-FUTURES";
  p.marginCoin = "USDT";
  p.marginMode = "crossed";
  p.positionMode = mode;
  return p;
}

}  // namespace

TEST(BitgetOrderSerialization, MarketOrderTranslatesToLowercaseMarketWithNoForce)
{
  auto transport = std::make_unique<FakeTransport>();
  auto* raw = transport.get();
  auto client =
      std::make_unique<BitgetAuthenticatedRestClient>("k", "s", "p", "https://api.bitget.com", raw);

  SymbolRegistry registry;
  OrderTracker tracker;
  SymbolId sym = registerUsdtFutures(registry);

  BitgetOrderExecutorT<NoPolicies> executor(std::move(client), &registry, &tracker, baseParams());

  Order order;
  order.id = 1;
  order.symbol = sym;
  order.side = Side::SELL;
  order.type = OrderType::MARKET;
  order.quantity = Quantity::fromDouble(1.0);
  order.flags.reduceOnly = 1;

  executor.submitOrder(order);

  ASSERT_EQ(raw->calls.size(), 1u);
  const std::string& body = raw->calls[0].body;
  EXPECT_NE(body.find(R"("orderType":"market")"), std::string::npos) << body;
  EXPECT_EQ(body.find(R"("force")"), std::string::npos)
      << "force must not be sent on a market order: " << body;
}

// force must reflect the order's own timeInForce, not a static config
// value ignored per order.
TEST(BitgetOrderSerialization, LimitOrderForceComesFromOrderTimeInForce)
{
  auto transport = std::make_unique<FakeTransport>();
  auto* raw = transport.get();
  auto client =
      std::make_unique<BitgetAuthenticatedRestClient>("k", "s", "p", "https://api.bitget.com", raw);

  SymbolRegistry registry;
  OrderTracker tracker;
  SymbolId sym = registerUsdtFutures(registry);

  BitgetOrderExecutorT<NoPolicies> executor(std::move(client), &registry, &tracker, baseParams());

  Order order;
  order.id = 2;
  order.symbol = sym;
  order.side = Side::BUY;
  order.type = OrderType::LIMIT;
  order.price = Price::fromDouble(60000.0);
  order.quantity = Quantity::fromDouble(1.0);
  order.timeInForce = TimeInForce::FOK;

  executor.submitOrder(order);

  ASSERT_EQ(raw->calls.size(), 1u);
  EXPECT_NE(raw->calls[0].body.find(R"("force":"fok")"), std::string::npos) << raw->calls[0].body;
}

TEST(BitgetOrderSerialization, PostOnlyMapsToPostOnlyForce)
{
  auto transport = std::make_unique<FakeTransport>();
  auto* raw = transport.get();
  auto client =
      std::make_unique<BitgetAuthenticatedRestClient>("k", "s", "p", "https://api.bitget.com", raw);

  SymbolRegistry registry;
  OrderTracker tracker;
  SymbolId sym = registerUsdtFutures(registry);

  BitgetOrderExecutorT<NoPolicies> executor(std::move(client), &registry, &tracker, baseParams());

  Order order;
  order.id = 3;
  order.symbol = sym;
  order.side = Side::BUY;
  order.type = OrderType::LIMIT;
  order.price = Price::fromDouble(60000.0);
  order.quantity = Quantity::fromDouble(1.0);
  order.flags.postOnly = 1;

  executor.submitOrder(order);

  ASSERT_EQ(raw->calls.size(), 1u);
  EXPECT_NE(raw->calls[0].body.find(R"("force":"post_only")"), std::string::npos)
      << raw->calls[0].body;
}

// Negative result to protect: Bitget already handles stop/take-profit
// order types and triggerPrice correctly via the plan-order path. This
// must not regress.
TEST(BitgetOrderSerialization, StopMarketStillRoutesToPlanOrderWithTriggerPrice)
{
  auto transport = std::make_unique<FakeTransport>();
  auto* raw = transport.get();
  auto client =
      std::make_unique<BitgetAuthenticatedRestClient>("k", "s", "p", "https://api.bitget.com", raw);

  SymbolRegistry registry;
  OrderTracker tracker;
  SymbolId sym = registerUsdtFutures(registry);

  BitgetOrderExecutorT<NoPolicies> executor(std::move(client), &registry, &tracker, baseParams());

  Order order;
  order.id = 4;
  order.symbol = sym;
  order.side = Side::SELL;
  order.type = OrderType::STOP_MARKET;
  order.quantity = Quantity::fromDouble(1.0);
  order.triggerPrice = Price::fromDouble(58000.0);
  order.flags.reduceOnly = 1;

  executor.submitOrder(order);

  ASSERT_EQ(raw->calls.size(), 1u);
  const std::string& body = raw->calls[0].body;
  EXPECT_NE(body.find(R"("planType":"normal_plan")"), std::string::npos) << body;
  EXPECT_NE(body.find(R"("triggerPrice":"58000)"), std::string::npos) << body;
  EXPECT_NE(body.find(R"("orderType":"market")"), std::string::npos) << body;
  // tradeSide/posSide gating by position mode applies to this
  // path too -- it is exercised by the OneWay/Hedge tests below against
  // the regular submitOrder path; not re-asserted here.
}

// tradeSide/posSide must not fire unconditionally off reduceOnly
// alone -- they now require the account's position mode to be known.
TEST(BitgetOrderSerialization, OneWayModeOmitsTradeSideAndPosSide)
{
  auto transport = std::make_unique<FakeTransport>();
  auto* raw = transport.get();
  auto client =
      std::make_unique<BitgetAuthenticatedRestClient>("k", "s", "p", "https://api.bitget.com", raw);

  SymbolRegistry registry;
  OrderTracker tracker;
  SymbolId sym = registerUsdtFutures(registry);

  BitgetOrderExecutorT<NoPolicies> executor(std::move(client), &registry, &tracker,
                                            baseParams(Bitget::PositionMode::OneWay));

  Order order;
  order.id = 5;
  order.symbol = sym;
  order.side = Side::SELL;
  order.type = OrderType::LIMIT;
  order.price = Price::fromDouble(60000.0);
  order.quantity = Quantity::fromDouble(1.0);
  order.flags.reduceOnly = 1;
  order.flags.holdSide = static_cast<uint8_t>(HoldSide::Long);

  executor.submitOrder(order);

  ASSERT_EQ(raw->calls.size(), 1u);
  const std::string& body = raw->calls[0].body;
  EXPECT_EQ(body.find(R"("tradeSide")"), std::string::npos)
      << "one-way mode must not send tradeSide: " << body;
  EXPECT_EQ(body.find(R"("posSide")"), std::string::npos)
      << "one-way mode must not send posSide: " << body;
}

TEST(BitgetOrderSerialization, HedgeModeSendsTradeSideAndPosSide)
{
  auto transport = std::make_unique<FakeTransport>();
  auto* raw = transport.get();
  auto client =
      std::make_unique<BitgetAuthenticatedRestClient>("k", "s", "p", "https://api.bitget.com", raw);

  SymbolRegistry registry;
  OrderTracker tracker;
  SymbolId sym = registerUsdtFutures(registry);

  BitgetOrderExecutorT<NoPolicies> executor(std::move(client), &registry, &tracker,
                                            baseParams(Bitget::PositionMode::Hedge));

  Order order;
  order.id = 6;
  order.symbol = sym;
  order.side = Side::SELL;
  order.type = OrderType::LIMIT;
  order.price = Price::fromDouble(60000.0);
  order.quantity = Quantity::fromDouble(1.0);
  order.flags.reduceOnly = 1;
  order.flags.holdSide = static_cast<uint8_t>(HoldSide::Long);

  executor.submitOrder(order);

  ASSERT_EQ(raw->calls.size(), 1u);
  const std::string& body = raw->calls[0].body;
  EXPECT_NE(body.find(R"("tradeSide":"close")"), std::string::npos) << body;
  EXPECT_NE(body.find(R"("posSide":"long")"), std::string::npos) << body;
}

// Same silent-fallback danger as HL/Bybit trailing stop: this connector
// has no implementation for trailing stop, so it must reject rather than
// send it through the regular limit/market path and drop trailing
// semantics entirely.
TEST(BitgetOrderSerialization, TrailingStopIsRejectedNotSilentlyDowngraded)
{
  auto transport = std::make_unique<FakeTransport>();
  auto* raw = transport.get();
  auto client =
      std::make_unique<BitgetAuthenticatedRestClient>("k", "s", "p", "https://api.bitget.com", raw);

  SymbolRegistry registry;
  OrderTracker tracker;
  SymbolId sym = registerUsdtFutures(registry);

  BitgetOrderExecutorT<NoPolicies> executor(std::move(client), &registry, &tracker, baseParams());

  Order order;
  order.id = 7;
  order.symbol = sym;
  order.side = Side::SELL;
  order.type = OrderType::TRAILING_STOP;
  order.quantity = Quantity::fromDouble(1.0);
  order.trailingCallbackRate = 100;

  executor.submitOrder(order);

  EXPECT_EQ(raw->calls.size(), 0u);
  EXPECT_FALSE(tracker.exists(7));
}
