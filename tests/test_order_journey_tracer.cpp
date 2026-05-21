/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/backtest_config.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/execution/order_journey_tracer.h"

#include <gtest/gtest.h>

#include <vector>

using namespace flox;

namespace
{
OrderEvent makeSubmittedEvent(OrderId id, int64_t ts)
{
  OrderEvent ev{};
  ev.status = OrderEventStatus::SUBMITTED;
  ev.order.id = id;
  ev.exchangeTsNs = ts;
  ev.timestamps.submittedAtNs = ts;
  return ev;
}

OrderEvent makeAcceptedEvent(OrderId id, int64_t submittedAt, int64_t acceptedAt)
{
  OrderEvent ev{};
  ev.status = OrderEventStatus::ACCEPTED;
  ev.order.id = id;
  ev.exchangeTsNs = acceptedAt;
  ev.timestamps.submittedAtNs = submittedAt;
  ev.timestamps.acceptedAtNs = acceptedAt;
  return ev;
}

OrderEvent makeFillEvent(OrderId id, int64_t submittedAt, int64_t firstFillAt,
                         bool isMaker)
{
  OrderEvent ev{};
  ev.status = OrderEventStatus::FILLED;
  ev.order.id = id;
  ev.exchangeTsNs = firstFillAt;
  ev.fillQty = Quantity::fromDouble(1.0);
  ev.fillPrice = Price::fromDouble(100.0);
  ev.isMaker = isMaker;
  ev.timestamps.submittedAtNs = submittedAt;
  ev.timestamps.firstFillAtNs = firstFillAt;
  ev.timestamps.lastFillAtNs = firstFillAt;
  return ev;
}
}  // namespace

TEST(OrderJourneyTracer, AccumulatesPerOrderTrace)
{
  OrderJourneyTracer tracer;

  tracer.onOrderEvent(makeSubmittedEvent(1, 100));
  tracer.onOrderEvent(makeAcceptedEvent(1, 100, 200));
  tracer.onOrderEvent(makeFillEvent(1, 100, 300, true));

  EXPECT_EQ(tracer.orderCount(), 1u);
  EXPECT_EQ(tracer.recordCount(), 3u);

  auto trace = tracer.journey(1);
  ASSERT_EQ(trace.size(), 3u);
  EXPECT_EQ(trace[0].status, static_cast<uint8_t>(OrderEventStatus::SUBMITTED));
  EXPECT_EQ(trace[1].status, static_cast<uint8_t>(OrderEventStatus::ACCEPTED));
  EXPECT_EQ(trace[2].status, static_cast<uint8_t>(OrderEventStatus::FILLED));
  EXPECT_EQ(trace[0].seq, 0u);
  EXPECT_EQ(trace[2].seq, 2u);
  EXPECT_EQ(trace[2].isMaker, 1);
}

TEST(OrderJourneyTracer, MedianAckLatencyAndTimeToFirstFill)
{
  OrderJourneyTracer tracer;

  for (OrderId id = 1; id <= 5; ++id)
  {
    const int64_t submit = static_cast<int64_t>(id) * 100;
    const int64_t ack = submit + (id * 10);    // 10, 20, 30, 40, 50 ns
    const int64_t fill = submit + (id * 100);  // 100, 200, 300, 400, 500 ns
    tracer.onOrderEvent(makeSubmittedEvent(id, submit));
    tracer.onOrderEvent(makeAcceptedEvent(id, submit, ack));
    tracer.onOrderEvent(makeFillEvent(id, submit, fill, false));
  }

  // ack latencies: 10, 20, 30, 40, 50 → median 30
  EXPECT_DOUBLE_EQ(tracer.medianAckLatencyNs(), 30.0);
  // time-to-first-fill: 100, 200, 300, 400, 500 → median 300
  EXPECT_DOUBLE_EQ(tracer.medianTimeToFirstFillNs(), 300.0);
}

TEST(OrderJourneyTracer, MakerFillRatioAcrossOrders)
{
  OrderJourneyTracer tracer;

  tracer.onOrderEvent(makeFillEvent(1, 0, 100, true));
  tracer.onOrderEvent(makeFillEvent(2, 0, 100, true));
  tracer.onOrderEvent(makeFillEvent(3, 0, 100, false));
  tracer.onOrderEvent(makeFillEvent(4, 0, 100, false));

  EXPECT_DOUBLE_EQ(tracer.makerFillRatio(), 0.5);
}

TEST(OrderJourneyTracer, BoundedByMaxOrdersEvictsOldest)
{
  OrderJourneyTracer::Config cfg{};
  cfg.maxOrders = 3;
  cfg.maxRecordsPerOrder = 4;
  OrderJourneyTracer tracer{cfg};

  for (OrderId id = 1; id <= 10; ++id)
  {
    tracer.onOrderEvent(makeSubmittedEvent(id, static_cast<int64_t>(id) * 10));
  }

  EXPECT_EQ(tracer.orderCount(), 3u);
  // Only the three most recently inserted orders should remain (8, 9, 10).
  EXPECT_TRUE(tracer.journey(8).size() == 1);
  EXPECT_TRUE(tracer.journey(9).size() == 1);
  EXPECT_TRUE(tracer.journey(10).size() == 1);
  EXPECT_TRUE(tracer.journey(1).empty());
}

TEST(OrderJourneyTracer, MaxRecordsPerOrderCaps)
{
  OrderJourneyTracer::Config cfg{};
  cfg.maxOrders = 16;
  cfg.maxRecordsPerOrder = 2;
  OrderJourneyTracer tracer{cfg};

  tracer.onOrderEvent(makeSubmittedEvent(1, 100));
  tracer.onOrderEvent(makeAcceptedEvent(1, 100, 200));
  tracer.onOrderEvent(makeFillEvent(1, 100, 300, true));  // dropped

  auto trace = tracer.journey(1);
  EXPECT_EQ(trace.size(), 2u);
}

TEST(OrderJourneyTracer, SamplingDropsOrdersBelowThreshold)
{
  OrderJourneyTracer::Config cfg{};
  cfg.sampleRate = 0.0;
  OrderJourneyTracer tracer{cfg};

  for (OrderId id = 1; id <= 100; ++id)
  {
    tracer.onOrderEvent(makeSubmittedEvent(id, static_cast<int64_t>(id) * 10));
  }
  EXPECT_EQ(tracer.orderCount(), 0u);

  OrderJourneyTracer::Config full{};
  full.sampleRate = 1.0;
  OrderJourneyTracer tracerFull{full};
  for (OrderId id = 1; id <= 100; ++id)
  {
    tracerFull.onOrderEvent(makeSubmittedEvent(id, static_cast<int64_t>(id) * 10));
  }
  EXPECT_EQ(tracerFull.orderCount(), 100u);
}

TEST(OrderJourneyTracer, AttachesToBacktestRunnerAsExecutionListener)
{
  BacktestConfig cfg{};
  cfg.feeRate = 0.0;
  cfg.initialCapital = 100'000.0;
  BacktestRunner runner(cfg);

  auto tracer = std::make_unique<OrderJourneyTracer>();
  OrderJourneyTracer* tracerPtr = tracer.get();
  runner.addExecutionListener(tracerPtr);

  // Drive a single market buy through the runner's simulated path.
  Order o{};
  o.id = 42;
  o.symbol = 1;
  o.side = Side::BUY;
  o.type = OrderType::MARKET;
  o.quantity = Quantity::fromDouble(1.0);

  auto& exec = runner.executor();
  // Seed a book.
  std::pmr::monotonic_buffer_resource pool(512);
  std::pmr::vector<BookLevel> bids(&pool);
  std::pmr::vector<BookLevel> asks(&pool);
  bids.emplace_back(Price::fromDouble(100.0), Quantity::fromDouble(5.0));
  asks.emplace_back(Price::fromDouble(101.0), Quantity::fromDouble(5.0));
  exec.onBookUpdate(1, bids, asks);

  exec.submitOrder(o);

  EXPECT_GE(tracerPtr->orderCount(), 1u);
  auto trace = tracerPtr->journey(42);
  EXPECT_FALSE(trace.empty());
  // SUBMITTED + ACCEPTED + FILLED at minimum.
  EXPECT_GE(trace.size(), 3u);
}
