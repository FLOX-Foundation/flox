/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/simulated_clock.h"
#include "flox/backtest/simulated_executor.h"

#include <gtest/gtest.h>

using namespace flox;

namespace
{
constexpr SymbolId BTC = 1;

Order makeIceberg(uint64_t id, Side side, double price, double total, double visible)
{
  Order o;
  o.id = id;
  o.symbol = BTC;
  o.side = side;
  o.type = OrderType::ICEBERG;
  o.price = Price::fromDouble(price);
  o.quantity = Quantity::fromDouble(total);
  o.visibleQuantity = Quantity::fromDouble(visible);
  return o;
}

struct Capture
{
  std::vector<OrderEvent> events;
};

void wire(SimulatedExecutor& ex, Capture& cap)
{
  ex.setOrderEventCallback([&](const OrderEvent& ev)
                           { cap.events.push_back(ev); });
}
}  // namespace

TEST(IcebergOrders, VisibleSliceFillsThenRefreshesAtomically)
{
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  ex.setQueueModel(QueueModel::FULL, 4);
  Capture cap;
  wire(ex, cap);

  // 10 total, 2 visible per slice.
  ex.submitOrder(makeIceberg(1, Side::BUY, 100.0, 10.0, 2.0));

  // Order in book shows only the visible 2.0.
  EXPECT_EQ(ex.icebergHiddenRemainingRaw(1), Quantity::fromDouble(8.0).raw());

  // Trade through 2.0 — visible tranche fills, refresh kicks in instantly.
  ex.onTrade(BTC, Price::fromDouble(100.0), Quantity::fromDouble(2.0), false);

  EXPECT_EQ(ex.icebergHiddenRemainingRaw(1), Quantity::fromDouble(6.0).raw());
}

TEST(IcebergOrders, FullOrderFillsViaSuccessiveRefreshes)
{
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  ex.setQueueModel(QueueModel::FULL, 4);
  Capture cap;
  wire(ex, cap);

  ex.submitOrder(makeIceberg(1, Side::BUY, 100.0, 6.0, 2.0));

  // Three trades of 2.0 each consume the full 6.0.
  for (int i = 0; i < 3; ++i)
  {
    ex.onTrade(BTC, Price::fromDouble(100.0), Quantity::fromDouble(2.0), false);
  }
  EXPECT_EQ(ex.icebergHiddenRemainingRaw(1), 0);

  int fills = 0;
  for (const auto& ev : cap.events)
  {
    if (ev.status == OrderEventStatus::FILLED ||
        ev.status == OrderEventStatus::PARTIALLY_FILLED)
    {
      ++fills;
    }
  }
  EXPECT_GE(fills, 3);
}

TEST(IcebergOrders, CancelClearsHiddenRemainder)
{
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  ex.setQueueModel(QueueModel::FULL, 4);

  ex.submitOrder(makeIceberg(1, Side::BUY, 100.0, 10.0, 2.0));
  EXPECT_EQ(ex.icebergHiddenRemainingRaw(1), Quantity::fromDouble(8.0).raw());

  ex.cancelOrder(1);
  EXPECT_EQ(ex.icebergHiddenRemainingRaw(1), 0);
}

TEST(IcebergOrders, NonIcebergUnaffected)
{
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  ex.setQueueModel(QueueModel::FULL, 4);

  Order o;
  o.id = 2;
  o.symbol = BTC;
  o.side = Side::BUY;
  o.type = OrderType::LIMIT;
  o.price = Price::fromDouble(100.0);
  o.quantity = Quantity::fromDouble(5.0);
  ex.submitOrder(o);

  EXPECT_EQ(ex.icebergHiddenRemainingRaw(2), 0);
}

TEST(IcebergOrders, RefreshLatencyDelaysExposure)
{
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  ex.setQueueModel(QueueModel::FULL, 4);
  ex.setIcebergRefreshLatency(1000);  // 1µs

  ex.submitOrder(makeIceberg(1, Side::BUY, 100.0, 4.0, 2.0));
  EXPECT_EQ(ex.icebergHiddenRemainingRaw(1), Quantity::fromDouble(2.0).raw());

  // Fill the visible tranche at t=0; the refresh isn't due until t=1000.
  clock.advanceTo(0);
  ex.onTrade(BTC, Price::fromDouble(100.0), Quantity::fromDouble(2.0), false);
  // Hidden not yet exposed because the refresh deadline hasn't passed.
  EXPECT_EQ(ex.icebergHiddenRemainingRaw(1), Quantity::fromDouble(2.0).raw());

  // Advance past the latency window and feed another tick.
  clock.advanceTo(2000);
  ex.onTrade(BTC, Price::fromDouble(100.0), Quantity::fromDouble(1.0), false);
  EXPECT_EQ(ex.icebergHiddenRemainingRaw(1), 0);
}

// === T041: size jitter + priority modes ===

TEST(IcebergOrders, SizeRandomisationZeroIsDeterministic)
{
  // Default 0% jitter must match T029 deterministic slicing.
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  ex.setQueueModel(QueueModel::FULL, 4);
  EXPECT_DOUBLE_EQ(ex.icebergSizeRandomisationPct(), 0.0);

  ex.submitOrder(makeIceberg(1, Side::BUY, 100.0, 4.0, 1.0));
  EXPECT_EQ(ex.icebergHiddenRemainingRaw(1), Quantity::fromDouble(3.0).raw());
  // Drain the visible tranches one at a time; each refresh exposes
  // exactly 1.0.
  for (int i = 0; i < 3; ++i)
  {
    ex.onTrade(BTC, Price::fromDouble(100.0), Quantity::fromDouble(1.0), false);
  }
  EXPECT_EQ(ex.icebergHiddenRemainingRaw(1), 0);
}

TEST(IcebergOrders, SizeRandomisationKeepsSlicesWithinBound)
{
  // ±10% jitter on a visible size of 10 lots → every drawn slice
  // lies in [9.0, 11.0] (rounded down via raw int).
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  ex.setQueueModel(QueueModel::FULL, 4);
  ex.setIcebergSizeRandomisationPct(0.10);
  ex.setIcebergJitterSeed(42);

  // Total 200 visible-10 → ~20 refreshes worth of jitter draws.
  ex.submitOrder(makeIceberg(1, Side::BUY, 100.0, 200.0, 10.0));

  std::vector<int64_t> slices;
  int64_t prevHidden = ex.icebergHiddenRemainingRaw(1);
  for (int i = 0; i < 100 && ex.icebergHiddenRemainingRaw(1) > 0; ++i)
  {
    ex.onTrade(BTC, Price::fromDouble(100.0), Quantity::fromDouble(10.0), false);
    const int64_t hiddenNow = ex.icebergHiddenRemainingRaw(1);
    if (hiddenNow != prevHidden)
    {
      slices.push_back(prevHidden - hiddenNow);
      prevHidden = hiddenNow;
    }
  }
  ASSERT_FALSE(slices.empty());
  const int64_t lo = Quantity::fromDouble(8.5).raw();
  const int64_t hi = Quantity::fromDouble(11.5).raw();
  for (int64_t s : slices)
  {
    EXPECT_GE(s, lo) << "slice " << s << " below ±10% lower bound";
    EXPECT_LE(s, hi) << "slice " << s << " above ±10% upper bound";
  }
}

TEST(IcebergOrders, PriorityModeDefaultIsBack)
{
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  EXPECT_EQ(ex.icebergPriorityMode(),
            SimulatedExecutor::IcebergPriorityMode::Back);
}

TEST(IcebergOrders, PriorityModeByNameAcceptsBackAndRetain)
{
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  ex.setIcebergPriorityModeByName("retain");
  EXPECT_EQ(ex.icebergPriorityMode(),
            SimulatedExecutor::IcebergPriorityMode::Retain);
  ex.setIcebergPriorityModeByName("back");
  EXPECT_EQ(ex.icebergPriorityMode(),
            SimulatedExecutor::IcebergPriorityMode::Back);
  // Unknown name is a no-op.
  ex.setIcebergPriorityModeByName("garbage");
  EXPECT_EQ(ex.icebergPriorityMode(),
            SimulatedExecutor::IcebergPriorityMode::Back);
}
