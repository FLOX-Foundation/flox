/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline order-serialization tests for the Bybit executor (CONN-02,
 * CONN-03, CONN-04, CONN-05). ITransport is a real interface the
 * production AuthenticatedRestClient already takes by pointer, so a fake
 * implementation captures exactly the request bodies this executor
 * builds -- no network, no linker tricks needed for this exchange.
 */

#include "flox-connectors/bybit/authenticated_rest_client.h"
#include "flox-connectors/bybit/bybit_order_executor.h"

#include <flox/common.h>
#include <flox/engine/symbol_registry.h>
#include <flox/execution/abstract_execution_listener.h>
#include <flox/execution/bus/order_execution_bus.h>
#include <flox/execution/events/order_event.h>
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
            MoveOnlyFunction<void(std::string_view)> onError) override
  {
    calls.push_back({std::string(url), std::string(body)});
    if (nextResponseIsError)
    {
      nextResponseIsError = false;
      if (onError)
      {
        onError(nextResponse);
      }
      return;
    }
    if (onSuccess)
    {
      onSuccess(nextResponse);
    }
  }

  std::vector<Call> calls;
  std::string nextResponse = R"({"retCode":0,"retMsg":"OK","result":{"orderId":"EX-1"}})";
  bool nextResponseIsError = false;
};

// Records through the canonical typed-callback path (the same path
// EventDispatcher<OrderEvent>::dispatch / OrderEvent::dispatchTo uses in
// production, i.e. what a real position tracker would see), not the raw
// OrderEvent struct. REJECTED and REJECTED_RATE_LIMIT both dispatch
// through onOrderRejected -- the reason string is what distinguishes them
// here, same as any real listener would have to.
class RecordingListener final : public IOrderExecutionListener
{
 public:
  RecordingListener() : IOrderExecutionListener(7) {}

  void onOrderRejected(const Order& order, const std::string& reason) override
  {
    rejections.push_back({order, reason});
  }

  struct Rejection
  {
    Order order;
    std::string reason;
  };
  std::vector<Rejection> rejections;
};

SymbolId registerLinear(SymbolRegistry& registry, const std::string& symbol = "BTCUSDT")
{
  SymbolInfo info;
  info.exchange = "bybit";
  info.symbol = symbol;
  info.type = InstrumentType::Future;
  return registry.registerSymbol(info);
}

}  // namespace

TEST(BybitOrderSerialization, MarketOrderOmitsPriceAndSendsReduceOnlyAndTif)
{
  auto transport = std::make_unique<FakeTransport>();
  auto* rawTransport = transport.get();
  auto client = std::make_unique<AuthenticatedRestClient>("key", "secret", "https://api.bybit.com",
                                                          rawTransport);

  SymbolRegistry registry;
  OrderTracker tracker;
  SymbolId sym = registerLinear(registry);

  BybitOrderExecutorT<NoPolicies> executor(std::move(client), &registry, &tracker);

  Order order;
  order.id = 1;
  order.symbol = sym;
  order.side = Side::SELL;
  order.type = OrderType::MARKET;
  order.quantity = Quantity::fromDouble(1.0);
  order.flags.reduceOnly = 1;
  order.timeInForce = TimeInForce::IOC;

  executor.submitOrder(order);

  ASSERT_EQ(rawTransport->calls.size(), 1u);
  const std::string& body = rawTransport->calls[0].body;
  EXPECT_NE(body.find(R"("orderType":"Market")"), std::string::npos) << body;
  EXPECT_EQ(body.find(R"("price")"), std::string::npos)
      << "a market order must not send price: " << body;
  EXPECT_NE(body.find(R"("reduceOnly":true)"), std::string::npos) << body;
  EXPECT_NE(body.find(R"("timeInForce":"IOC")"), std::string::npos) << body;
}

// The sharpest CONN-02 case: before the fix, STOP_MARKET and TRAILING_STOP
// both serialized to a byte-identical "Limit" order at price 0 -- no
// protective order existed on the exchange for either, though the
// strategy believed a stop was resting. After the fix, STOP_MARKET
// carries a real triggerPrice on a conditional order, and TRAILING_STOP
// (which this connector has no wire shape for) is rejected outright
// instead of silently becoming the same fake limit order.
TEST(BybitOrderSerialization, StopMarketAndTrailingStopNoLongerCollapseToTheSameLimitZero)
{
  SymbolRegistry registry;
  SymbolId sym = registerLinear(registry);

  auto stopTransport = std::make_unique<FakeTransport>();
  auto* rawStop = stopTransport.get();
  auto stopClient =
      std::make_unique<AuthenticatedRestClient>("key", "secret", "https://api.bybit.com", rawStop);
  OrderTracker stopTracker;
  BybitOrderExecutorT<NoPolicies> stopExecutor(std::move(stopClient), &registry, &stopTracker);

  Order stopOrder;
  stopOrder.id = 2;
  stopOrder.symbol = sym;
  stopOrder.side = Side::SELL;
  stopOrder.type = OrderType::STOP_MARKET;
  stopOrder.quantity = Quantity::fromDouble(1.0);
  stopOrder.triggerPrice = Price::fromDouble(58000.0);
  stopOrder.flags.reduceOnly = 1;

  stopExecutor.submitOrder(stopOrder);

  ASSERT_EQ(rawStop->calls.size(), 1u);
  const std::string& stopBody = rawStop->calls[0].body;
  EXPECT_NE(stopBody.find(R"("orderType":"Market")"), std::string::npos) << stopBody;
  EXPECT_NE(stopBody.find(R"("triggerPrice":"58000)"), std::string::npos) << stopBody;
  EXPECT_NE(stopBody.find(R"("triggerDirection")"), std::string::npos) << stopBody;
  EXPECT_EQ(stopBody.find(R"("price")"), std::string::npos) << stopBody;

  auto trailingTransport = std::make_unique<FakeTransport>();
  auto* rawTrailing = trailingTransport.get();
  auto trailingClient = std::make_unique<AuthenticatedRestClient>(
      "key", "secret", "https://api.bybit.com", rawTrailing);
  OrderTracker trailingTracker;
  OrderExecutionBus bus;
  RecordingListener listener;
  bus.subscribe(&listener);
  bus.start();
  BybitOrderExecutorT<NoPolicies> trailingExecutor(std::move(trailingClient), &registry,
                                                   &trailingTracker);
  trailingExecutor.setOrderBus(&bus);

  Order trailingOrder = stopOrder;
  trailingOrder.id = 3;
  trailingOrder.type = OrderType::TRAILING_STOP;
  trailingOrder.trailingCallbackRate = 100;  // 1%

  trailingExecutor.submitOrder(trailingOrder);
  bus.flush();
  bus.stop();

  EXPECT_EQ(rawTrailing->calls.size(), 0u)
      << "an unimplemented trailing stop must never reach the transport as a fake limit order";
  ASSERT_EQ(listener.rejections.size(), 1u);
}

TEST(BybitOrderSerialization, RejectedSubmitPublishesEventInsteadOfSilentlyDropping)
{
  auto transport = std::make_unique<FakeTransport>();
  auto* rawTransport = transport.get();
  rawTransport->nextResponse = R"({"retCode":110007,"retMsg":"ab not enough for new order"})";
  auto client = std::make_unique<AuthenticatedRestClient>("key", "secret", "https://api.bybit.com",
                                                          rawTransport);

  SymbolRegistry registry;
  OrderTracker tracker;
  SymbolId sym = registerLinear(registry);
  OrderExecutionBus bus;
  RecordingListener listener;
  bus.subscribe(&listener);
  bus.start();

  BybitOrderExecutorT<NoPolicies> executor(std::move(client), &registry, &tracker);
  executor.setOrderBus(&bus);

  Order order;
  order.id = 10;
  order.symbol = sym;
  order.side = Side::BUY;
  order.type = OrderType::LIMIT;
  order.price = Price::fromDouble(100.0);
  order.quantity = Quantity::fromDouble(1.0);

  executor.submitOrder(order);
  bus.flush();
  bus.stop();

  ASSERT_EQ(listener.rejections.size(), 1u);
  EXPECT_NE(listener.rejections[0].reason.find("110007"), std::string::npos);
  EXPECT_FALSE(tracker.exists(order.id))
      << "a rejected submit must not leave a tracker record behind";
}

TEST(BybitOrderSerialization, ReplaceReadsRealExchangeIdInsteadOfEmptyString)
{
  auto transport = std::make_unique<FakeTransport>();
  auto* rawTransport = transport.get();
  auto client = std::make_unique<AuthenticatedRestClient>("key", "secret", "https://api.bybit.com",
                                                          rawTransport);

  SymbolRegistry registry;
  OrderTracker tracker;
  SymbolId sym = registerLinear(registry);

  BybitOrderExecutorT<NoPolicies> executor(std::move(client), &registry, &tracker);

  Order order;
  order.id = 1001;
  order.symbol = sym;
  order.side = Side::BUY;
  order.type = OrderType::LIMIT;
  order.price = Price::fromDouble(100.0);
  order.quantity = Quantity::fromDouble(1.0);

  rawTransport->nextResponse = R"({"retCode":0,"retMsg":"OK","result":{"orderId":"EX-1"}})";
  executor.submitOrder(order);
  ASSERT_TRUE(tracker.exists(1001));

  Order replacement = order;
  replacement.id = 1002;
  replacement.price = Price::fromDouble(101.0);

  rawTransport->nextResponse = R"({"retCode":0,"retMsg":"OK","result":{"orderId":"EX-1"}})";
  executor.replaceOrder(1001, replacement);

  auto state = tracker.get(1002);
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ(state->exchangeOrderId, "EX-1")
      << "onReplaced must not be called with a literal empty exchangeOrderId";

  // Now cancel the replaced order and verify the request actually carries
  // the real id, not "".
  executor.cancelOrder(1002);
  ASSERT_EQ(rawTransport->calls.size(), 3u);  // submit, replace, cancel
  const std::string& cancelBody = rawTransport->calls[2].body;
  EXPECT_NE(cancelBody.find(R"("orderId":"EX-1")"), std::string::npos)
      << "cancel must not build {\"orderId\":\"\"}: " << cancelBody;
}

TEST(BybitOrderSerialization, ClientSideRateLimitRejectionIsNotSilent)
{
  auto transport = std::make_unique<FakeTransport>();
  auto* rawTransport = transport.get();
  auto client = std::make_unique<AuthenticatedRestClient>("key", "secret", "https://api.bybit.com",
                                                          rawTransport);

  SymbolRegistry registry;
  OrderTracker tracker;
  SymbolId sym = registerLinear(registry);
  OrderExecutionBus bus;
  RecordingListener listener;
  bus.subscribe(&listener);
  bus.start();

  RateLimitConfig rlConfig;
  rlConfig.capacity = 1;
  rlConfig.refillRate = 1;
  rlConfig.policy = RateLimitPolicy::REJECT;

  BybitOrderExecutorT<WithRateLimit> executor(std::move(client), &registry, &tracker,
                                              std::move(rlConfig));
  executor.setOrderBus(&bus);

  Order order;
  order.id = 20;
  order.symbol = sym;
  order.side = Side::BUY;
  order.type = OrderType::LIMIT;
  order.price = Price::fromDouble(100.0);
  order.quantity = Quantity::fromDouble(1.0);

  executor.submitOrder(order);  // consumes the single token
  ASSERT_EQ(rawTransport->calls.size(), 1u);
  ASSERT_TRUE(tracker.exists(20));

  Order order2 = order;
  order2.id = 21;
  executor.submitOrder(order2);  // must be rejected client-side

  bus.flush();
  bus.stop();

  EXPECT_EQ(rawTransport->calls.size(), 1u)
      << "a client-side rate-limited submit must never reach the transport";
  EXPECT_FALSE(tracker.exists(21));

  bool sawRateLimited = false;
  for (const auto& r : listener.rejections)
  {
    if (r.reason == "client-side rate limit")
    {
      sawRateLimited = true;
    }
  }
  EXPECT_TRUE(sawRateLimited)
      << "before the fix, a rate-limited submit produced a log line and nothing else";
}

TEST(BybitOrderSerialization, ClientSideRateLimitedCancelDoesNotSilentlyVanish)
{
  auto transport = std::make_unique<FakeTransport>();
  auto* rawTransport = transport.get();
  auto client = std::make_unique<AuthenticatedRestClient>("key", "secret", "https://api.bybit.com",
                                                          rawTransport);

  SymbolRegistry registry;
  OrderTracker tracker;
  SymbolId sym = registerLinear(registry);
  OrderExecutionBus bus;
  RecordingListener listener;
  bus.subscribe(&listener);
  bus.start();

  RateLimitConfig rlConfig;
  rlConfig.capacity = 1;
  rlConfig.refillRate = 1;
  rlConfig.policy = RateLimitPolicy::REJECT;

  BybitOrderExecutorT<WithRateLimit> executor(std::move(client), &registry, &tracker,
                                              std::move(rlConfig));
  executor.setOrderBus(&bus);

  Order order;
  order.id = 30;
  order.symbol = sym;
  order.side = Side::BUY;
  order.type = OrderType::LIMIT;
  order.price = Price::fromDouble(100.0);
  order.quantity = Quantity::fromDouble(1.0);

  executor.submitOrder(order);  // consumes the token
  ASSERT_EQ(rawTransport->calls.size(), 1u);

  executor.cancelOrder(30);  // the stop that must be pulled -- rate-limited away
  bus.flush();
  bus.stop();

  EXPECT_EQ(rawTransport->calls.size(), 1u)
      << "the cancel must never reach the transport when client-side rate-limited";
  EXPECT_TRUE(tracker.isActive(30))
      << "the tracker correctly still shows the order active -- the point is that the caller "
         "now gets an event saying the cancel never went out, instead of silence";

  bool sawRateLimited = false;
  for (const auto& r : listener.rejections)
  {
    if (r.reason == "client-side rate limit")
    {
      sawRateLimited = true;
    }
  }
  EXPECT_TRUE(sawRateLimited);
}
