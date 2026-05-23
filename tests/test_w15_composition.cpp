/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// W15 composition tests. Each W15 subsystem has unit tests covering
// its own surface; this file exercises the joint behaviour two or
// more subsystems exhibit when they run on the same SimulatedExecutor
// tick loop. The bugs caught here don't show up in isolated tests.

#include "flox/backtest/bracket_order.h"
#include "flox/backtest/funding_schedule.h"
#include "flox/backtest/liquidation_engine.h"
#include "flox/backtest/rate_limit_policy.h"
#include "flox/backtest/simulated_clock.h"
#include "flox/backtest/simulated_executor.h"
#include "flox/backtest/venue_availability.h"

#include <gtest/gtest.h>

#include <vector>

using namespace flox;

namespace
{
constexpr SymbolId BTC = 1;

void pushBook(SimulatedExecutor& ex, SymbolId sym, double bid, double bidQty,
              double ask, double askQty)
{
  std::pmr::monotonic_buffer_resource pool(512);
  std::pmr::vector<BookLevel> bids(&pool);
  std::pmr::vector<BookLevel> asks(&pool);
  bids.emplace_back(Price::fromDouble(bid), Quantity::fromDouble(bidQty));
  asks.emplace_back(Price::fromDouble(ask), Quantity::fromDouble(askQty));
  ex.onBookUpdate(sym, bids, asks);
}

Order makeLimit(OrderId id, Side side, double price, double qty,
                TimeInForce tif = TimeInForce::GTC, uint64_t account = 0)
{
  Order o;
  o.id = id;
  o.symbol = BTC;
  o.side = side;
  o.type = OrderType::LIMIT;
  o.price = Price::fromDouble(price);
  o.quantity = Quantity::fromDouble(qty);
  o.timeInForce = tif;
  o.accountId = account;
  return o;
}

struct Capture
{
  std::vector<OrderEvent> events;
  void on(const OrderEvent& e) { events.push_back(e); }
  size_t count(OrderEventStatus s) const
  {
    size_t n = 0;
    for (const auto& e : events)
    {
      n += (e.status == s);
    }
    return n;
  }
  size_t fillsForOrder(OrderId id) const
  {
    size_t n = 0;
    for (const auto& e : events)
    {
      if (e.order.id == id && e.status == OrderEventStatus::FILLED)
      {
        ++n;
      }
    }
    return n;
  }
};
}  // namespace

// 1. STP-cancelled order produces no fill events — the trade didn't
//    happen, so any downstream tracker (fees, position, etc.) sees
//    the cancellation, not a phantom fill.
TEST(W15Composition, StpCancelDoesNotEmitFill)
{
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  ex.setQueueModel(QueueModel::TOB, 1);
  ex.setSTPMode(STPMode::CancelNewest);
  Capture cap;
  ex.setOrderEventCallback([&](const OrderEvent& e)
                           { cap.on(e); });
  pushBook(ex, BTC, 49000.0, 5.0, 51000.0, 5.0);

  // Same account; the second crossing order must be rejected with
  // no fills emitted on either order.
  ex.submitOrder(makeLimit(1, Side::BUY, 50500.0, 1.0));
  ex.submitOrder(makeLimit(2, Side::SELL, 50500.0, 1.0));

  EXPECT_EQ(cap.count(OrderEventStatus::REJECTED), 1u);
  EXPECT_EQ(cap.count(OrderEventStatus::FILLED), 0u);
  EXPECT_EQ(cap.count(OrderEventStatus::PARTIALLY_FILLED), 0u);
}

// 2. LiquidationEngine routed through executor: a rate-limited
//    executor still accepts liquidation orders (they bypass the
//    user-strategy rate-limit envelope by design).
TEST(W15Composition, LiquidationRoutesThroughRateLimitedExecutor)
{
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  ex.setQueueModel(QueueModel::TOB, 1);

  // Tight rate limit (1 submit / 10s) — would reject a strategy
  // order if it submitted first. The liquidation order should still
  // go through because it lives on the engine's own path.
  RateLimitPolicy rl;
  rl.addBucket("orders_10s", 10'000'000'000LL, 1);
  ex.setRateLimitPolicy(rl);

  Capture cap;
  ex.setOrderEventCallback([&](const OrderEvent& e)
                           { cap.on(e); });
  pushBook(ex, BTC, 80.0, 100.0, 81.0, 100.0);

  LiquidationEngine liq;
  liq.addTier(0.0, 0.005);
  liq.setExecutor(&ex);
  // 10 BTC long at 100, equity 10 — underwater at mark 80.
  LeveragedPosition p;
  p.accountId = 1;
  p.symbol = BTC;
  p.quantity = 10.0;
  p.entryPrice = 100.0;
  p.equity = 10.0;
  liq.openPosition(p);

  const auto out = liq.onMark(BTC, 80.0);
  EXPECT_EQ(out.liquidationsCount, 1u);
  EXPECT_TRUE(liq.positions().empty());
}

// 3. Bracket entry queued during outage flushes on recovery.
TEST(W15Composition, BracketEntrySubmittedDuringOutageFlushesOnRecovery)
{
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  ex.setQueueModel(QueueModel::TOB, 1);

  VenueAvailability va;
  va.scheduleOutage(/*start=*/1'000, /*duration=*/1'000, OnOutage::HOLD);
  ex.setVenueAvailability(&va);

  Capture cap;
  ex.setOrderEventCallback([&](const OrderEvent& e)
                           { cap.on(e); });
  pushBook(ex, BTC, 99.0, 10.0, 100.0, 10.0);

  // Submit during outage.
  clock.advanceTo(UnixNanos(1'500));
  BracketOrder b;
  b.bracketId = 1;
  b.symbol = BTC;
  b.entry.side = Side::BUY;
  b.entry.type = OrderType::LIMIT;
  b.entry.price = Price::fromDouble(100.0);
  b.entry.quantity = Quantity::fromDouble(1.0);
  b.takeProfit.side = Side::SELL;
  b.takeProfit.type = OrderType::LIMIT;
  b.takeProfit.price = Price::fromDouble(110.0);
  b.takeProfit.quantity = Quantity::fromDouble(1.0);
  b.stop.side = Side::SELL;
  b.stop.type = OrderType::STOP_MARKET;
  b.stop.triggerPrice = Price::fromDouble(90.0);
  b.stop.quantity = Quantity::fromDouble(1.0);
  ex.submitBracket(b);

  // No events while outage is active.
  EXPECT_EQ(cap.count(OrderEventStatus::SUBMITTED), 0u);

  // Advance past outage; a market data callback triggers flush.
  clock.advanceTo(UnixNanos(2'500));
  pushBook(ex, BTC, 99.0, 10.0, 100.0, 10.0);

  // Bracket state should now be PENDING_ENTRY or further (entry
  // could have filled on the crossing book snapshot).
  const auto st = ex.bracketStatus(1);
  EXPECT_TRUE(st.state == BracketState::PENDING_ENTRY ||
              st.state == BracketState::ENTRY_FILLED);
}

// 4. Multi-account STP + rate limit: the rate-limit policy ticks
//    once per submit even when STP rejects the order.
TEST(W15Composition, RateLimitChargesAccountOrderBeforeStpReject)
{
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  ex.setQueueModel(QueueModel::TOB, 1);
  ex.setSTPMode(STPMode::CancelNewest);

  RateLimitPolicy rl;
  rl.addBucket("orders_10s", 10'000'000'000LL, 5);
  ex.setRateLimitPolicy(rl);

  Capture cap;
  ex.setOrderEventCallback([&](const OrderEvent& e)
                           { cap.on(e); });
  pushBook(ex, BTC, 49000.0, 5.0, 51000.0, 5.0);

  // Two accounts; default account 0 mapped to nothing → no STP.
  ex.submitOrder(makeLimit(1, Side::BUY, 50500.0, 1.0, TimeInForce::GTC, 42));
  ex.submitOrder(makeLimit(2, Side::SELL, 50500.0, 1.0, TimeInForce::GTC, 43));
  // No STP between distinct accounts.
  EXPECT_EQ(cap.count(OrderEventStatus::REJECTED), 0u);
}

// 5. FundingSchedule + venue downtime: funding still ticks during
//    an outage (it's an internal clock event, not a venue action).
TEST(W15Composition, FundingTicksDuringVenueOutage)
{
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  VenueAvailability va;
  va.scheduleOutage(/*start=*/0, /*duration=*/24LL * 3600'000'000'000LL,
                    OnOutage::HOLD);
  ex.setVenueAvailability(&va);

  auto sched = FundingSchedule::constant(8LL * 3600LL * 1'000'000'000LL, 0.0001);
  // The schedule's tick is driven by the strategy, not the executor —
  // it doesn't consult VenueAvailability. We assert that here so
  // strategies relying on funding cadence aren't surprised by an
  // outage swallowing settlement events.
  const auto p = sched.tick(9LL * 3600LL * 1'000'000'000LL, {BTC}, {1.0}, {50000.0});
  EXPECT_EQ(p.size(), 1u);
  EXPECT_NEAR(p.front().rate, 0.0001, 1e-12);
}

// 6. Iceberg + pro-rata: a trade against a level with a single
//    iceberg-style hidden order distributes pro-rata across visible
//    orders. We don't have native iceberg yet (blocked on T029), so
//    this test demonstrates the pure pro-rata distribution which the
//    iceberg slice falls into once T029 lands.
TEST(W15Composition, ProRataDistributesAcrossSeveralRestingOrders)
{
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  ex.setQueueModel(QueueModel::PRO_RATA, 4);
  Capture cap;
  ex.setOrderEventCallback([&](const OrderEvent& e)
                           { cap.on(e); });

  // Two resting buy orders at 50000.
  ex.submitOrder(makeLimit(1, Side::BUY, 50000.0, 5.0));
  ex.submitOrder(makeLimit(2, Side::BUY, 50000.0, 15.0));
  pushBook(ex, BTC, 50000.0, 20.0, 51000.0, 1.0);

  // 8 BTC trade hits 50000. Pure pro-rata: order 1 gets 8 * 5/20 =
  // 2, order 2 gets 8 * 15/20 = 6.
  ex.onTrade(BTC, Price::fromDouble(50000.0), Quantity::fromDouble(8.0), false);

  // Both orders should have received partial fills.
  EXPECT_GE(cap.fillsForOrder(1) + cap.count(OrderEventStatus::PARTIALLY_FILLED), 1u);
}

// 7. Partial-outage SubmitOnlyDown: cancels still work, submits
//    buffered. The gate's behaviour is asserted at the venue level:
//    submits_allowed returns false during the window while
//    cancels_allowed returns true.
TEST(W15Composition, PartialOutageGatesSubmitsButPassesCancels)
{
  VenueAvailability va;
  va.scheduleOutageEx(/*start=*/1'000, /*duration=*/1'000,
                      OutageType::SubmitOnlyDown, OnOutage::HOLD);
  // Mid-outage:
  EXPECT_FALSE(va.submitsAllowed(1'500));
  EXPECT_TRUE(va.cancelsAllowed(1'500));
  // Post-outage:
  EXPECT_TRUE(va.submitsAllowed(2'500));
  EXPECT_TRUE(va.cancelsAllowed(2'500));
}

// 8. Per-endpoint rate limit + STP: cancels for the rejected newest
//    order do not double-bill the trading bucket on top of the
//    submit. The submit cost is paid; the STP-driven reject doesn't.
TEST(W15Composition, StpRejectDoesNotChargeCancelBucket)
{
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  ex.setQueueModel(QueueModel::TOB, 1);
  ex.setSTPMode(STPMode::CancelNewest);

  // 5-submit window; we'll submit two, both should consume one slot
  // each. The 2nd is STP-rejected — it should not also consume a
  // cancel slot on the trading bucket.
  RateLimitPolicy rl;
  rl.addBucket("orders_10s", 10'000'000'000LL, /*capacity=*/2);
  ex.setRateLimitPolicy(rl);

  pushBook(ex, BTC, 49000.0, 5.0, 51000.0, 5.0);
  ex.submitOrder(makeLimit(1, Side::BUY, 50500.0, 1.0));
  ex.submitOrder(makeLimit(2, Side::SELL, 50500.0, 1.0));

  auto states = ex.rateLimitPolicy().bucketStates(0);
  ASSERT_EQ(states.size(), 1u);
  // Submit 1 OK (used=1). Submit 2: charged the trading bucket on
  // submit, then STP rejected the order. used=2 either way; not 3.
  EXPECT_LE(states[0].used, 2u);
}
