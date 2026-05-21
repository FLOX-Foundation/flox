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

using namespace flox;

namespace
{
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

Order limitBuy(OrderId id, SymbolId sym, double price, double qty)
{
  Order o;
  o.id = id;
  o.symbol = sym;
  o.side = Side::BUY;
  o.type = OrderType::LIMIT;
  o.price = Price::fromDouble(price);
  o.quantity = Quantity::fromDouble(qty);
  return o;
}

BacktestConfig configWithCancelLatency(int64_t latencyNs, int64_t jitterNs = 0)
{
  BacktestConfig cfg{};
  cfg.queueModel = QueueModel::TOB;
  cfg.queueDepth = 1;
  cfg.cancelAckLatencyNs = latencyNs;
  cfg.cancelAckJitterNs = jitterNs;
  cfg.cancelAckSeed = 42;
  return cfg;
}
}  // namespace

TEST(CancelLatency, ZeroLatencyKeepsSynchronousBehavior)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.applyConfig(configWithCancelLatency(0));

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 99.0, 1.0));
  exec.cancelOrder(1);

  bool sawCanceled = false;
  bool sawPending = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::PENDING_CANCEL)
    {
      sawPending = true;
    }
    if (ev.status == OrderEventStatus::CANCELED)
    {
      sawCanceled = true;
    }
  }
  EXPECT_TRUE(sawCanceled);
  EXPECT_FALSE(sawPending);
}

TEST(CancelLatency, AsyncCancelEmitsPendingThenCanceledAfterAck)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.applyConfig(configWithCancelLatency(10'000'000));  // 10 ms

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 99.0, 1.0));
  exec.cancelOrder(1);

  // Immediately after cancelOrder: PENDING_CANCEL fired, CANCELED has not.
  bool sawPending = false;
  bool sawCanceledEarly = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::PENDING_CANCEL)
    {
      sawPending = true;
    }
    if (ev.status == OrderEventStatus::CANCELED)
    {
      sawCanceledEarly = true;
    }
  }
  EXPECT_TRUE(sawPending);
  EXPECT_FALSE(sawCanceledEarly);

  // Advance the clock past the ack deadline and feed a no-op book update
  // to give the simulator a tick.
  clock.advanceTo(clock.nowNs() + 11'000'000);
  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);

  bool sawCanceledAfter = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::CANCELED)
    {
      sawCanceledAfter = true;
    }
  }
  EXPECT_TRUE(sawCanceledAfter);
}

TEST(CancelLatency, LateCancelAfterFillEmitsRejected)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.applyConfig(configWithCancelLatency(10'000'000));

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 0.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 100.0, 1.0));
  exec.cancelOrder(1);

  // Aggressive trade arrives within the 10 ms ack window and fills the
  // resting order.
  clock.advanceTo(clock.nowNs() + 1'000'000);  // 1 ms in
  exec.onTrade(1, Price::fromDouble(100.0), Quantity::fromDouble(1.0), false);

  bool sawFilled = false;
  bool sawRejected = false;
  std::string rejectReason;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::FILLED)
    {
      sawFilled = true;
    }
    if (ev.status == OrderEventStatus::REJECTED)
    {
      sawRejected = true;
      rejectReason = ev.rejectReason;
    }
  }
  EXPECT_TRUE(sawFilled);
  EXPECT_TRUE(sawRejected);
  EXPECT_EQ(rejectReason, "late_cancel_after_fill");
}

TEST(CancelLatency, AckDeferredBelowBandFinalizedAbove)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.applyConfig(configWithCancelLatency(10'000'000, 2'000'000));

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 99.0, 1.0));
  exec.cancelOrder(1);

  // 7 ms is strictly below `base - jitter` (= 8 ms). No CANCELED yet.
  clock.advanceTo(clock.nowNs() + 7'000'000);
  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  for (const auto& ev : events)
  {
    EXPECT_NE(ev.status, OrderEventStatus::CANCELED);
  }

  // 13 ms total is strictly above `base + jitter` (= 12 ms). Now the
  // ack must have fired.
  clock.advanceTo(clock.nowNs() + 6'000'000);
  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  bool sawCanceled = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::CANCELED)
    {
      sawCanceled = true;
    }
  }
  EXPECT_TRUE(sawCanceled);
}
