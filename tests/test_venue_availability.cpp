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
#include "flox/backtest/venue_availability.h"

#include <gtest/gtest.h>

#include <vector>

using namespace flox;

namespace
{
constexpr SymbolId BTC = 1;

Order makeLimit(uint64_t id, Side side, double price, double qty)
{
  Order o;
  o.id = id;
  o.symbol = BTC;
  o.side = side;
  o.type = OrderType::LIMIT;
  o.price = Price::fromDouble(price);
  o.quantity = Quantity::fromDouble(qty);
  return o;
}

struct Capture
{
  std::vector<OrderEventStatus> statuses;
  std::vector<OrderId> orderIds;
};

void wire(SimulatedExecutor& ex, Capture& cap)
{
  ex.setOrderEventCallback([&](const OrderEvent& ev)
                           {
                             cap.statuses.push_back(ev.status);
                             cap.orderIds.push_back(ev.order.id); });
}
}  // namespace

TEST(VenueAvailability, ScheduledOutageCancelsAllOpenOrders)
{
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  Capture cap;
  wire(ex, cap);

  VenueAvailability va;
  va.scheduleOutage(/*start=*/1000, /*duration=*/500, OnOutage::CANCEL_ALL);
  ex.setVenueAvailability(&va);

  clock.advanceTo(0);
  ex.submitOrder(makeLimit(1, Side::BUY, 100.0, 1.0));
  ex.submitOrder(makeLimit(2, Side::BUY, 99.0, 1.0));

  // Step into the outage; the next market event drives the edge detection.
  clock.advanceTo(1000);
  ex.onBar(BTC, Price::fromDouble(100.0));

  size_t canceled = 0;
  for (auto s : cap.statuses)
  {
    if (s == OrderEventStatus::CANCELED)
    {
      ++canceled;
    }
  }
  EXPECT_EQ(canceled, 2u);
}

TEST(VenueAvailability, HoldKeepsOpenOrdersIntact)
{
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  Capture cap;
  wire(ex, cap);

  VenueAvailability va;
  va.scheduleOutage(1000, 500, OnOutage::HOLD);
  ex.setVenueAvailability(&va);

  clock.advanceTo(0);
  ex.submitOrder(makeLimit(1, Side::BUY, 100.0, 1.0));

  clock.advanceTo(1000);
  ex.onBar(BTC, Price::fromDouble(100.0));

  size_t canceled = 0;
  for (auto s : cap.statuses)
  {
    if (s == OrderEventStatus::CANCELED)
    {
      ++canceled;
    }
  }
  EXPECT_EQ(canceled, 0u);
}

TEST(VenueAvailability, MarketDataGappedDuringOutage)
{
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  Capture cap;
  wire(ex, cap);

  VenueAvailability va;
  va.scheduleOutage(1000, 500, OnOutage::HOLD);
  ex.setVenueAvailability(&va);

  clock.advanceTo(0);
  ex.submitOrder(makeLimit(1, Side::BUY, 100.0, 1.0));
  const size_t baselineEvents = cap.statuses.size();

  // Trades during outage are silently dropped; no fill events emitted.
  clock.advanceTo(1100);
  ex.onTrade(BTC, Price::fromDouble(99.0), Quantity::fromDouble(2.0), false);
  EXPECT_EQ(cap.statuses.size(), baselineEvents);
}

TEST(VenueAvailability, BufferedSubmissionsFlushAtRecoveryInFIFOOrder)
{
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  Capture cap;
  wire(ex, cap);

  VenueAvailability va;
  va.scheduleOutage(1000, 500, OnOutage::HOLD);
  ex.setVenueAvailability(&va);

  // Pre-outage: nothing.
  // During outage: three buffered submits.
  clock.advanceTo(1100);
  ex.onBar(BTC, Price::fromDouble(100.0));  // transitions to down state
  ex.submitOrder(makeLimit(10, Side::BUY, 99.0, 1.0));
  ex.submitOrder(makeLimit(11, Side::BUY, 98.0, 1.0));
  ex.submitOrder(makeLimit(12, Side::BUY, 97.0, 1.0));
  // No SUBMITTED events during outage.
  for (auto s : cap.statuses)
  {
    EXPECT_NE(s, OrderEventStatus::SUBMITTED);
  }

  // Recovery: edge detection on the next event flushes in FIFO order.
  clock.advanceTo(2000);
  ex.onBar(BTC, Price::fromDouble(100.0));

  std::vector<OrderId> submittedOrder;
  for (size_t i = 0; i < cap.statuses.size(); ++i)
  {
    if (cap.statuses[i] == OrderEventStatus::SUBMITTED)
    {
      submittedOrder.push_back(cap.orderIds[i]);
    }
  }
  ASSERT_EQ(submittedOrder.size(), 3u);
  EXPECT_EQ(submittedOrder[0], 10u);
  EXPECT_EQ(submittedOrder[1], 11u);
  EXPECT_EQ(submittedOrder[2], 12u);
}

TEST(VenueAvailability, IsUpReflectsScheduledWindow)
{
  VenueAvailability va;
  va.scheduleOutage(100, 50);  // [100, 150)
  EXPECT_TRUE(va.isUp(99));
  EXPECT_FALSE(va.isUp(100));
  EXPECT_FALSE(va.isUp(149));
  EXPECT_TRUE(va.isUp(150));
}

TEST(VenueAvailability, RandomOutagesSamplePoissonProcess)
{
  VenueAvailability va;
  va.autoRandomOutages(/*perDay=*/10.0,
                       /*meanDurationNs=*/100 * 1000 * 1000LL,
                       OnOutage::HOLD,
                       /*seed=*/42);
  // Advance through a 10-day window; expect roughly 100 outages.
  const int64_t tenDaysNs = 10LL * 86400LL * 1'000'000'000LL;
  va.isUp(tenDaysNs);
  const auto& out = va.outages();
  EXPECT_GE(out.size(), 60u);  // loose Poisson tolerance
  EXPECT_LE(out.size(), 160u);
}

TEST(VenueAvailability, RecoveryEdgeFiresOnceThenResets)
{
  VenueAvailability va;
  va.scheduleOutage(100, 50);

  // Up before, down during, up after.
  EXPECT_FALSE(va.consumeRecoveryEdge(50));   // up; no transition yet
  EXPECT_FALSE(va.consumeRecoveryEdge(120));  // down
  EXPECT_TRUE(va.consumeRecoveryEdge(200));   // recovery edge
  EXPECT_FALSE(va.consumeRecoveryEdge(300));  // already consumed
}

TEST(VenueAvailability, EmptyConfigStaysUpForever)
{
  VenueAvailability va;
  EXPECT_TRUE(va.isUp(0));
  EXPECT_TRUE(va.isUp(1'000'000'000'000LL));
  EXPECT_TRUE(va.outages().empty());
}

// === T046: richer downtime pathology ===

TEST(VenueAvailability, OutageTypeTotalKeepsLegacySemantics)
{
  VenueAvailability va;
  va.scheduleOutageEx(100, 100, OutageType::Total);
  EXPECT_FALSE(va.isUp(150));
  EXPECT_FALSE(va.submitsAllowed(150));
  EXPECT_FALSE(va.cancelsAllowed(150));
  EXPECT_FALSE(va.bookUpdatesAllowed(150));
  EXPECT_FALSE(va.tradesAllowed(150));
  EXPECT_DOUBLE_EQ(va.latencyMultiplier(150), 1.0);
}

TEST(VenueAvailability, SubmitOnlyDownGatesSubmitsButNotCancels)
{
  VenueAvailability va;
  va.scheduleOutageEx(100, 100, OutageType::SubmitOnlyDown);
  // Binary isUp remains true — the venue is technically reachable.
  EXPECT_TRUE(va.isUp(150));
  EXPECT_FALSE(va.submitsAllowed(150));
  EXPECT_TRUE(va.cancelsAllowed(150));
  EXPECT_TRUE(va.bookUpdatesAllowed(150));
  EXPECT_TRUE(va.tradesAllowed(150));
}

TEST(VenueAvailability, CancelOnlyDownGatesCancelsButNotSubmits)
{
  VenueAvailability va;
  va.scheduleOutageEx(100, 100, OutageType::CancelOnlyDown);
  EXPECT_TRUE(va.submitsAllowed(150));
  EXPECT_FALSE(va.cancelsAllowed(150));
}

TEST(VenueAvailability, StaleBookDropsBookUpdatesButLetsTradesThrough)
{
  VenueAvailability va;
  va.scheduleOutageEx(100, 100, OutageType::StaleBook);
  EXPECT_TRUE(va.submitsAllowed(150));
  EXPECT_TRUE(va.cancelsAllowed(150));
  EXPECT_FALSE(va.bookUpdatesAllowed(150));
  EXPECT_TRUE(va.tradesAllowed(150));
}

TEST(VenueAvailability, SlowDegradationKeepsActionsButRaisesLatencyMultiplier)
{
  VenueAvailability va;
  va.scheduleOutageEx(100, 100, OutageType::SlowDegradation, OnOutage::HOLD,
                      /*gtcTtlNs=*/0,
                      /*degradationLatencyMultiplier=*/50.0);
  EXPECT_TRUE(va.submitsAllowed(150));
  EXPECT_TRUE(va.cancelsAllowed(150));
  EXPECT_TRUE(va.bookUpdatesAllowed(150));
  EXPECT_DOUBLE_EQ(va.latencyMultiplier(150), 50.0);
  // Outside the window the multiplier drops back to 1.0.
  EXPECT_DOUBLE_EQ(va.latencyMultiplier(250), 1.0);
}

TEST(VenueAvailability, WrongSideRecoveryAccumulatesBpsAtRecoveryEdge)
{
  VenueAvailability va;
  va.scheduleOutageEx(100, 100, OutageType::WrongSideRecovery, OnOutage::HOLD,
                      /*gtcTtlNs=*/0,
                      /*degradationLatencyMultiplier=*/1.0,
                      /*wrongSideRecoveryBps=*/25.0);
  // Mid-outage: hard-down behaves like Total for action gates.
  EXPECT_FALSE(va.isUp(150));
  EXPECT_FALSE(va.submitsAllowed(150));
  EXPECT_FALSE(va.cancelsAllowed(150));
  // Walk past the end so we cross the recovery edge.
  EXPECT_FALSE(va.consumeRecoveryEdge(150));
  EXPECT_TRUE(va.consumeRecoveryEdge(250));
  EXPECT_DOUBLE_EQ(va.consumeWrongSideRecoveryBps(), 25.0);
  // Idempotent.
  EXPECT_DOUBLE_EQ(va.consumeWrongSideRecoveryBps(), 0.0);
}
