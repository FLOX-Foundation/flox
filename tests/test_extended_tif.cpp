/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/backtest_config.h"
#include "flox/backtest/simulated_clock.h"
#include "flox/backtest/simulated_executor.h"

#include <gtest/gtest.h>

#include <vector>

using namespace flox;

namespace
{
constexpr SymbolId BTC = 1;

void pushBook(SimulatedExecutor& exec, SymbolId sym, double bid, double bidQty,
              double ask, double askQty)
{
  std::pmr::monotonic_buffer_resource pool(512);
  std::pmr::vector<BookLevel> bids(&pool);
  std::pmr::vector<BookLevel> asks(&pool);
  bids.emplace_back(Price::fromDouble(bid), Quantity::fromDouble(bidQty));
  asks.emplace_back(Price::fromDouble(ask), Quantity::fromDouble(askQty));
  exec.onBookUpdate(sym, bids, asks);
}

Order makeLimit(OrderId id, Side side, double price, double qty, TimeInForce tif)
{
  Order o;
  o.id = id;
  o.symbol = BTC;
  o.side = side;
  o.type = OrderType::LIMIT;
  o.price = Price::fromDouble(price);
  o.quantity = Quantity::fromDouble(qty);
  o.timeInForce = tif;
  return o;
}

struct EventCapture
{
  std::vector<OrderEvent> events;
  void record(const OrderEvent& e) { events.push_back(e); }
  bool hasStatus(OrderEventStatus s) const
  {
    for (const auto& e : events)
    {
      if (e.status == s)
      {
        return true;
      }
    }
    return false;
  }
  std::string lastRejectReason() const
  {
    for (auto it = events.rbegin(); it != events.rend(); ++it)
    {
      if (it->status == OrderEventStatus::REJECTED)
      {
        return it->rejectReason;
      }
    }
    return "";
  }
};
}  // namespace

TEST(ExtendedTIF, FOK_RejectsWhenInsufficientLiquidity)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  EventCapture cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.record(e); });

  pushBook(exec, BTC, 50000.0, 0.5, 50001.0, 0.5);  // only 0.5 on ask

  // FOK BUY for 1.0 at 50001 — needs 1.0 of ask liquidity.
  exec.submitOrder(makeLimit(1, Side::BUY, 50001.0, 1.0, TimeInForce::FOK));

  EXPECT_TRUE(cap.hasStatus(OrderEventStatus::REJECTED));
  EXPECT_EQ(cap.lastRejectReason(), "fok_not_fillable");
  EXPECT_FALSE(cap.hasStatus(OrderEventStatus::PARTIALLY_FILLED));
  EXPECT_FALSE(cap.hasStatus(OrderEventStatus::FILLED));
}

TEST(ExtendedTIF, FOK_FillsAtomicallyWhenEnoughLiquidity)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  EventCapture cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.record(e); });

  pushBook(exec, BTC, 50000.0, 1.0, 50001.0, 2.0);  // 2.0 on ask

  exec.submitOrder(makeLimit(1, Side::BUY, 50001.0, 1.0, TimeInForce::FOK));

  EXPECT_TRUE(cap.hasStatus(OrderEventStatus::FILLED));
  EXPECT_FALSE(cap.hasStatus(OrderEventStatus::REJECTED));
}

// === T042: FOK fill semantics variants ===

TEST(ExtendedTIF, FOK_DefaultModeIsAnyPrice)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  EXPECT_EQ(exec.fokMode(), SimulatedExecutor::FokMode::AnyPrice);
}

TEST(ExtendedTIF, FOK_SetFokModeByNameAcceptsKnownStrings)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setFokModeByName("single_price");
  EXPECT_EQ(exec.fokMode(), SimulatedExecutor::FokMode::SinglePrice);
  exec.setFokModeByName("any_price");
  EXPECT_EQ(exec.fokMode(), SimulatedExecutor::FokMode::AnyPrice);
  // Unknown name is a no-op.
  exec.setFokModeByName("not-a-mode");
  EXPECT_EQ(exec.fokMode(), SimulatedExecutor::FokMode::AnyPrice);
}

TEST(ExtendedTIF, FOK_SinglePriceRejectsCrossingAtDifferentLevel)
{
  // Book has best ask at 50001. SinglePrice FOK with limit 50002 (more
  // aggressive than TOB) crosses but at a different price level than
  // the limit — single_price semantics reject this.
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);
  exec.setFokMode(SimulatedExecutor::FokMode::SinglePrice);

  EventCapture cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.record(e); });

  pushBook(exec, BTC, 50000.0, 1.0, 50001.0, 2.0);

  exec.submitOrder(makeLimit(1, Side::BUY, 50002.0, 1.0, TimeInForce::FOK));

  EXPECT_TRUE(cap.hasStatus(OrderEventStatus::REJECTED));
  EXPECT_EQ(cap.lastRejectReason(), "fok_unfillable");
}

TEST(ExtendedTIF, FOK_SinglePriceFillsAtExactLevel)
{
  // SinglePrice FOK with limit == TOB price and enough size → fills.
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);
  exec.setFokMode(SimulatedExecutor::FokMode::SinglePrice);

  EventCapture cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.record(e); });

  pushBook(exec, BTC, 50000.0, 1.0, 50001.0, 2.0);

  exec.submitOrder(makeLimit(1, Side::BUY, 50001.0, 1.0, TimeInForce::FOK));

  EXPECT_TRUE(cap.hasStatus(OrderEventStatus::FILLED));
  EXPECT_FALSE(cap.hasStatus(OrderEventStatus::REJECTED));
}

TEST(ExtendedTIF, FOK_SinglePriceRejectsWhenLevelSizeInsufficient)
{
  // SinglePrice FOK with limit == TOB price but qty exceeds level size.
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);
  exec.setFokMode(SimulatedExecutor::FokMode::SinglePrice);

  EventCapture cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.record(e); });

  pushBook(exec, BTC, 50000.0, 0.5, 50001.0, 0.5);

  exec.submitOrder(makeLimit(1, Side::BUY, 50001.0, 1.0, TimeInForce::FOK));

  EXPECT_TRUE(cap.hasStatus(OrderEventStatus::REJECTED));
  // Level size insufficient → existing fok_not_fillable reason.
  EXPECT_EQ(cap.lastRejectReason(), "fok_not_fillable");
}

TEST(ExtendedTIF, FOK_AnyPriceParityWithDefaultBehaviour)
{
  // Sanity: explicitly setting AnyPrice keeps the existing T028
  // semantics from the first two FOK tests above.
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);
  exec.setFokMode(SimulatedExecutor::FokMode::AnyPrice);

  EventCapture cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.record(e); });

  // Crossing but with limit above TOB price — AnyPrice walks any
  // acceptable price and fills.
  pushBook(exec, BTC, 50000.0, 1.0, 50001.0, 2.0);
  exec.submitOrder(makeLimit(1, Side::BUY, 50002.0, 1.0, TimeInForce::FOK));
  EXPECT_TRUE(cap.hasStatus(OrderEventStatus::FILLED));
  EXPECT_FALSE(cap.hasStatus(OrderEventStatus::REJECTED));
}

TEST(ExtendedTIF, IOC_CancelsWhenNoCross)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  EventCapture cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.record(e); });

  pushBook(exec, BTC, 50000.0, 1.0, 50010.0, 1.0);

  // BUY at 50005 — does not cross the 50010 ask. IOC should cancel.
  exec.submitOrder(makeLimit(1, Side::BUY, 50005.0, 1.0, TimeInForce::IOC));

  EXPECT_TRUE(cap.hasStatus(OrderEventStatus::CANCELED));
  EXPECT_FALSE(cap.hasStatus(OrderEventStatus::FILLED));
}

TEST(ExtendedTIF, IOC_FillsCrossingPart)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  EventCapture cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.record(e); });

  pushBook(exec, BTC, 50000.0, 1.0, 50001.0, 1.0);

  exec.submitOrder(makeLimit(1, Side::BUY, 50001.0, 0.5, TimeInForce::IOC));

  EXPECT_TRUE(cap.hasStatus(OrderEventStatus::FILLED));
}

TEST(ExtendedTIF, GTD_ExpiresAfterDeadline)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  EventCapture cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.record(e); });

  pushBook(exec, BTC, 49000.0, 1.0, 51000.0, 1.0);  // wide spread, won't cross

  // GTD limit BUY at 49500 — rests; expires in 1s.
  Order o = makeLimit(1, Side::BUY, 49500.0, 1.0, TimeInForce::GTD);
  o.expiresAfter =
      TimePoint(std::chrono::nanoseconds(static_cast<int64_t>(clock.nowNs()) + 1'000'000'000LL));
  exec.submitOrder(o);

  // Tick the clock past the deadline; need a market event to drive
  // the finalizer chain.
  clock.advanceTo(UnixNanos(2'000'000'000LL));
  pushBook(exec, BTC, 49000.0, 1.0, 51000.0, 1.0);

  EXPECT_TRUE(cap.hasStatus(OrderEventStatus::EXPIRED));
}

TEST(ExtendedTIF, GTD_StaysWhenNotYetExpired)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  EventCapture cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.record(e); });

  pushBook(exec, BTC, 49000.0, 1.0, 51000.0, 1.0);

  Order o = makeLimit(1, Side::BUY, 49500.0, 1.0, TimeInForce::GTD);
  o.expiresAfter =
      TimePoint(std::chrono::nanoseconds(static_cast<int64_t>(clock.nowNs()) + 10'000'000'000LL));
  exec.submitOrder(o);

  // Clock advances inside the budget.
  clock.advanceTo(UnixNanos(1'000'000'000LL));
  pushBook(exec, BTC, 49000.0, 1.0, 51000.0, 1.0);

  EXPECT_FALSE(cap.hasStatus(OrderEventStatus::EXPIRED));
}

TEST(ExtendedTIF, ReduceOnly_RejectsWhenFlat)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  EventCapture cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.record(e); });

  pushBook(exec, BTC, 50000.0, 1.0, 50001.0, 1.0);

  Order o = makeLimit(1, Side::BUY, 50001.0, 0.5, TimeInForce::IOC);
  o.flags.reduceOnly = 1;
  exec.submitOrder(o);

  EXPECT_TRUE(cap.hasStatus(OrderEventStatus::REJECTED));
  EXPECT_EQ(cap.lastRejectReason(), "reduce_only");
}

TEST(ExtendedTIF, ReduceOnly_RejectsWhenWouldGrowSameSide)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  EventCapture cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.record(e); });

  pushBook(exec, BTC, 50000.0, 5.0, 50001.0, 5.0);

  // Open long via IOC fill.
  Order open = makeLimit(1, Side::BUY, 50001.0, 1.0, TimeInForce::IOC);
  exec.submitOrder(open);
  ASSERT_TRUE(cap.hasStatus(OrderEventStatus::FILLED));
  cap.events.clear();

  // Reduce-only BUY (same side) — would grow long, must reject.
  Order o = makeLimit(2, Side::BUY, 50001.0, 0.5, TimeInForce::IOC);
  o.flags.reduceOnly = 1;
  exec.submitOrder(o);
  EXPECT_TRUE(cap.hasStatus(OrderEventStatus::REJECTED));
  EXPECT_EQ(cap.lastRejectReason(), "reduce_only");
}

TEST(ExtendedTIF, ReduceOnly_TruncatesWhenOvershooting)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  EventCapture cap;
  exec.setOrderEventCallback([&](const OrderEvent& e)
                             { cap.record(e); });

  pushBook(exec, BTC, 50000.0, 5.0, 50001.0, 5.0);

  // Open 1.0 long.
  exec.submitOrder(makeLimit(1, Side::BUY, 50001.0, 1.0, TimeInForce::IOC));
  ASSERT_TRUE(cap.hasStatus(OrderEventStatus::FILLED));
  cap.events.clear();

  // Reduce-only SELL for 2.0 at the bid — would flip to short. Should
  // truncate to 1.0 (matches current open).
  Order o = makeLimit(2, Side::SELL, 50000.0, 2.0, TimeInForce::IOC);
  o.flags.reduceOnly = 1;
  exec.submitOrder(o);

  // Look at the fill event quantity (truncated to 1.0, not 2.0).
  bool sawFilledOne = false;
  for (const auto& e : cap.events)
  {
    if (e.status == OrderEventStatus::FILLED && e.order.id == 2)
    {
      EXPECT_EQ(e.order.quantity.toDouble(), 1.0);
      sawFilledOne = true;
    }
  }
  EXPECT_TRUE(sawFilledOne);
}
