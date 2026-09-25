/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * submitOrder, cancelOrder and replaceOrder go through the rate-limit
 * policy; setLeverage, submitOrderWithLeverage, placePosTpsl and
 * modifyPosTpsl do not. They are ordinary REST requests against the same
 * venue budget, and the kijun trail walks a stop through modifyPosTpsl on
 * every bar, so the paths that bypass the limiter are the ones that fire
 * most often. A budget that covers only the paths that happen to check it
 * is not a budget.
 *
 * Each test spends the whole budget on a submit -- the one path that is
 * already limited -- and then calls one unlimited path, asserting nothing
 * further reaches the transport. With REJECT that is the entire contract:
 * over the budget, no request leaves.
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

class RecordingTransport final : public ITransport
{
 public:
  void post(std::string_view url, std::string_view body,
            const std::vector<std::pair<std::string_view, std::string_view>>&,
            MoveOnlyFunction<void(std::string_view)> onSuccess,
            MoveOnlyFunction<void(std::string_view)>) override
  {
    urls.push_back(std::string(url));
    bodies.push_back(std::string(body));
    if (onSuccess)
    {
      // Every Bitget endpoint used here answers with code 00000; the
      // pos-tpsl one answers with an array, and reading "data" as an object
      // on that response is harmless because the executor's handler for it
      // asks for an array.
      onSuccess(R"({"code":"00000","msg":"success","data":{"orderId":"BG-1"},"dataArray":[]})");
    }
  }

  std::vector<std::string> urls;
  std::vector<std::string> bodies;
};

Bitget::Params baseParams()
{
  Bitget::Params p;
  p.productType = "USDT-FUTURES";
  p.marginCoin = "USDT";
  p.marginMode = "crossed";
  return p;
}

RateLimitConfig oneTokenReject()
{
  RateLimitConfig cfg;
  cfg.capacity = 1;
  cfg.refillRate = 1;
  cfg.policy = RateLimitPolicy::REJECT;
  return cfg;
}

struct Harness
{
  std::unique_ptr<RecordingTransport> owner{std::make_unique<RecordingTransport>()};
  RecordingTransport* transport{owner.get()};
  SymbolRegistry registry;
  OrderTracker tracker;
  SymbolId symbol{0};
  std::unique_ptr<BitgetOrderExecutorT<WithRateLimit>> executor;

  Harness()
  {
    SymbolInfo info;
    info.exchange = "bitget";
    info.symbol = "BTCUSDT";
    info.type = InstrumentType::Future;
    symbol = registry.registerSymbol(info);

    auto client = std::make_unique<BitgetAuthenticatedRestClient>(
        "k", "s", "p", "https://api.bitget.com", transport);
    executor = std::make_unique<BitgetOrderExecutorT<WithRateLimit>>(
        std::move(client), &registry, &tracker, baseParams(), oneTokenReject());
  }

  Order order(OrderId id) const
  {
    Order o;
    o.id = id;
    o.symbol = symbol;
    o.side = Side::BUY;
    o.type = OrderType::LIMIT;
    o.price = Price::fromDouble(60000.0);
    o.quantity = Quantity::fromDouble(1.0);
    return o;
  }

  // Spends the single token, leaving the bucket empty for the path under
  // test. Returns the number of requests on the wire at that point.
  std::size_t drain()
  {
    executor->submitOrder(order(1));
    EXPECT_EQ(transport->urls.size(), 1u);
    return transport->urls.size();
  }
};

}  // namespace

// Control, green today: the budget really is spent after one submit, so a
// second submit -- the path that does consult the limiter -- is refused.
// Without this, "no further request" below could mean the transport was
// never reachable.
TEST(BitgetRateLimitPaths, SecondSubmitIsThrottled)
{
  Harness h;
  const std::size_t before = h.drain();

  h.executor->submitOrder(h.order(2));

  EXPECT_EQ(h.transport->urls.size(), before);
}

TEST(BitgetRateLimitPaths, SetLeverageIsThrottled)
{
  Harness h;
  const std::size_t before = h.drain();

  h.executor->setLeverage("BTCUSDT", 10);

  EXPECT_EQ(h.transport->urls.size(), before)
      << "setLeverage went out over the budget: " << h.transport->urls.back();
}

TEST(BitgetRateLimitPaths, SubmitOrderWithLeverageIsThrottled)
{
  Harness h;
  const std::size_t before = h.drain();

  h.executor->submitOrderWithLeverage(h.order(2), 10, 0.0, 0.0);

  EXPECT_EQ(h.transport->urls.size(), before)
      << "submitOrderWithLeverage sent its set-leverage leg over the budget: "
      << h.transport->urls.back();
}

TEST(BitgetRateLimitPaths, PlacePosTpslIsThrottled)
{
  Harness h;
  const std::size_t before = h.drain();

  h.executor->placePosTpsl(h.symbol, HoldSide::Long, 58000.0, 0.0, 2);

  EXPECT_EQ(h.transport->urls.size(), before)
      << "placePosTpsl went out over the budget: " << h.transport->urls.back();
}

TEST(BitgetRateLimitPaths, ModifyPosTpslIsThrottled)
{
  Harness h;
  const std::size_t before = h.drain();

  h.executor->modifyPosTpsl(h.symbol, "BG-TPSL-1", 58500.0, 1.0);

  EXPECT_EQ(h.transport->urls.size(), before)
      << "modifyPosTpsl went out over the budget: " << h.transport->urls.back();
}

// A burst across the unlimited paths, the shape the kijun trail actually
// produces: one order and then a run of stop moves. On a budget of one
// token the venue must see one request, not six.
TEST(BitgetRateLimitPaths, BurstAcrossEveryPathIsThrottled)
{
  Harness h;

  h.executor->submitOrder(h.order(1));
  h.executor->setLeverage("BTCUSDT", 10);
  h.executor->placePosTpsl(h.symbol, HoldSide::Long, 58000.0, 0.0, 2);
  h.executor->modifyPosTpsl(h.symbol, "BG-TPSL-1", 58100.0, 1.0);
  h.executor->modifyPosTpsl(h.symbol, "BG-TPSL-1", 58200.0, 1.0);
  h.executor->submitOrderWithLeverage(h.order(3), 10, 0.0, 0.0);

  EXPECT_EQ(h.transport->urls.size(), 1u)
      << h.transport->urls.size() << " requests left on a budget of one token";
}
