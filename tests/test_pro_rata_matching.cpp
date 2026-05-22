/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/order_queue_tracker.h"

#include <gtest/gtest.h>

#include <vector>

using namespace flox;

namespace
{
constexpr SymbolId BTC = 1;

OrderQueueTracker makeTracker(QueueModel m, size_t fifoTopN = 0)
{
  OrderQueueTracker t;
  t.setModel(m, /*depth=*/4);
  t.setFifoTopN(fifoTopN);
  return t;
}

int64_t totalFillRaw(const std::vector<std::pair<OrderId, Quantity>>& fills)
{
  int64_t s = 0;
  for (const auto& f : fills)
  {
    s += f.second.raw();
  }
  return s;
}

Quantity fillFor(const std::vector<std::pair<OrderId, Quantity>>& fills, OrderId id)
{
  for (const auto& f : fills)
  {
    if (f.first == id)
    {
      return f.second;
    }
  }
  return Quantity{};
}
}  // namespace

TEST(ProRataMatching, PureProRataSplitsAcrossAllOrders)
{
  auto t = makeTracker(QueueModel::PRO_RATA);
  // Three orders at the same level: 1, 2, 1 (4 total).
  // Note: addOrder records aheadAtArrival as the level total *at the
  // moment of registration* — feed level totals as the cumulative
  // size so far so we don't have stale "ahead" values blocking
  // pro-rata distribution.
  t.addOrder(BTC, Side::BUY, Price::fromDouble(50000.0), 1, Quantity::fromDouble(1.0),
             Quantity{});
  t.addOrder(BTC, Side::BUY, Price::fromDouble(50000.0), 2, Quantity::fromDouble(2.0),
             Quantity{});
  t.addOrder(BTC, Side::BUY, Price::fromDouble(50000.0), 3, Quantity::fromDouble(1.0),
             Quantity{});

  std::vector<std::pair<OrderId, Quantity>> filled;
  t.onTrade(BTC, Price::fromDouble(50000.0), Quantity::fromDouble(2.0), filled);

  // 2.0 distributed by weight 1:2:1 → 0.5 : 1.0 : 0.5.
  EXPECT_EQ(filled.size(), 3u);
  EXPECT_NEAR(fillFor(filled, 1).toDouble(), 0.5, 1e-6);
  EXPECT_NEAR(fillFor(filled, 2).toDouble(), 1.0, 1e-6);
  EXPECT_NEAR(fillFor(filled, 3).toDouble(), 0.5, 1e-6);
}

TEST(ProRataMatching, ProRataDistributesExactly)
{
  auto t = makeTracker(QueueModel::PRO_RATA);
  t.addOrder(BTC, Side::BUY, Price::fromDouble(50000.0), 1, Quantity::fromDouble(3.0),
             Quantity{});
  t.addOrder(BTC, Side::BUY, Price::fromDouble(50000.0), 2, Quantity::fromDouble(1.0),
             Quantity{});

  std::vector<std::pair<OrderId, Quantity>> filled;
  t.onTrade(BTC, Price::fromDouble(50000.0), Quantity::fromDouble(2.0), filled);

  // Sum of distributed must equal trade qty (within rounding).
  EXPECT_LE(std::abs(totalFillRaw(filled) - Quantity::fromDouble(2.0).raw()), 1);
}

TEST(ProRataMatching, ProRataTradeCappedAtLevelTotal)
{
  auto t = makeTracker(QueueModel::PRO_RATA);
  t.addOrder(BTC, Side::BUY, Price::fromDouble(50000.0), 1, Quantity::fromDouble(1.0),
             Quantity{});
  t.addOrder(BTC, Side::BUY, Price::fromDouble(50000.0), 2, Quantity::fromDouble(1.0),
             Quantity{});

  std::vector<std::pair<OrderId, Quantity>> filled;
  // Trade larger than level total — distribute all 2.0, no overshoot.
  t.onTrade(BTC, Price::fromDouble(50000.0), Quantity::fromDouble(5.0), filled);

  EXPECT_EQ(totalFillRaw(filled), Quantity::fromDouble(2.0).raw());
}

TEST(ProRataWithFifo, TopNGetsFifoRestProRata)
{
  // 4 orders: FIFO top 2 consume first, then pro-rata across the rest.
  auto t = makeTracker(QueueModel::PRO_RATA_WITH_FIFO, /*fifoTopN=*/2);
  t.addOrder(BTC, Side::BUY, Price::fromDouble(50000.0), 1, Quantity::fromDouble(1.0),
             Quantity{});
  t.addOrder(BTC, Side::BUY, Price::fromDouble(50000.0), 2, Quantity::fromDouble(1.0),
             Quantity{});
  t.addOrder(BTC, Side::BUY, Price::fromDouble(50000.0), 3, Quantity::fromDouble(2.0),
             Quantity{});
  t.addOrder(BTC, Side::BUY, Price::fromDouble(50000.0), 4, Quantity::fromDouble(2.0),
             Quantity{});

  std::vector<std::pair<OrderId, Quantity>> filled;
  // Trade 4: first 2 FIFO fully (1 + 1 = 2), remaining 2 pro-rata
  // across orders 3 and 4 (2 : 2 → 1 each).
  t.onTrade(BTC, Price::fromDouble(50000.0), Quantity::fromDouble(4.0), filled);

  EXPECT_NEAR(fillFor(filled, 1).toDouble(), 1.0, 1e-6);
  EXPECT_NEAR(fillFor(filled, 2).toDouble(), 1.0, 1e-6);
  EXPECT_NEAR(fillFor(filled, 3).toDouble(), 1.0, 1e-6);
  EXPECT_NEAR(fillFor(filled, 4).toDouble(), 1.0, 1e-6);
}

TEST(ProRataWithFifo, SmallTradeOnlyHitsFifoFront)
{
  auto t = makeTracker(QueueModel::PRO_RATA_WITH_FIFO, /*fifoTopN=*/3);
  t.addOrder(BTC, Side::BUY, Price::fromDouble(50000.0), 1, Quantity::fromDouble(2.0),
             Quantity{});
  t.addOrder(BTC, Side::BUY, Price::fromDouble(50000.0), 2, Quantity::fromDouble(2.0),
             Quantity{});
  t.addOrder(BTC, Side::BUY, Price::fromDouble(50000.0), 3, Quantity::fromDouble(2.0),
             Quantity{});
  t.addOrder(BTC, Side::BUY, Price::fromDouble(50000.0), 4, Quantity::fromDouble(5.0),
             Quantity{});

  std::vector<std::pair<OrderId, Quantity>> filled;
  // Trade 1.0 only touches the front order (FIFO).
  t.onTrade(BTC, Price::fromDouble(50000.0), Quantity::fromDouble(1.0), filled);

  EXPECT_EQ(filled.size(), 1u);
  EXPECT_EQ(filled[0].first, 1u);
  EXPECT_NEAR(filled[0].second.toDouble(), 1.0, 1e-6);
}

TEST(ProRataMatching, FifoModeUnaffectedByMode)
{
  // Baseline: NONE / TOB / FULL all preserve the FIFO behaviour.
  for (auto mode : {QueueModel::TOB, QueueModel::FULL})
  {
    auto t = makeTracker(mode);
    t.addOrder(BTC, Side::BUY, Price::fromDouble(50000.0), 1, Quantity::fromDouble(2.0),
               Quantity{});
    t.addOrder(BTC, Side::BUY, Price::fromDouble(50000.0), 2, Quantity::fromDouble(2.0),
               Quantity{});
    std::vector<std::pair<OrderId, Quantity>> filled;
    t.onTrade(BTC, Price::fromDouble(50000.0), Quantity::fromDouble(1.5), filled);
    // FIFO: first order eats everything.
    ASSERT_EQ(filled.size(), 1u);
    EXPECT_EQ(filled[0].first, 1u);
  }
}

TEST(ProRataMatching, EmptyLevelIgnored)
{
  auto t = makeTracker(QueueModel::PRO_RATA);
  std::vector<std::pair<OrderId, Quantity>> filled;
  t.onTrade(BTC, Price::fromDouble(50000.0), Quantity::fromDouble(1.0), filled);
  EXPECT_TRUE(filled.empty());
}
