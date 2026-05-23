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

// === T043: TOP_PRO_LMM + PRO_RATA_WITH_PRIORITY ===

TEST(TopProLmm, TopGetsConfiguredShareThenProRataRest)
{
  // Three orders at the same level, 10 each (total 30). Trade qty 10.
  // topShare = 0.4 → top gets 4. Remaining 6 distributes pro-rata
  // across the tail (orders 2 and 3, weights 10 each → 3 + 3).
  auto t = makeTracker(QueueModel::TOP_PRO_LMM);
  t.setTopPriorityShare(0.4);
  const auto px = Price::fromDouble(50000.0);
  t.addOrder(BTC, Side::BUY, px, /*id=*/1, Quantity::fromDouble(10.0),
             Quantity::fromDouble(30.0));
  t.addOrder(BTC, Side::BUY, px, 2, Quantity::fromDouble(10.0),
             Quantity::fromDouble(30.0));
  t.addOrder(BTC, Side::BUY, px, 3, Quantity::fromDouble(10.0),
             Quantity::fromDouble(30.0));

  std::vector<std::pair<OrderId, Quantity>> filled;
  t.onTrade(BTC, px, Quantity::fromDouble(10.0), filled);
  // Total filled ≈ 10.
  EXPECT_NEAR(Quantity::fromRaw(totalFillRaw(filled)).toDouble(), 10.0, 1e-6);
  // Top gets ~4.
  EXPECT_NEAR(fillFor(filled, 1).toDouble(), 4.0, 1e-6);
}

TEST(TopProLmm, LmmBonusShiftsTailDistribution)
{
  // Top + two tail orders, one LMM. Trade qty 10, topShare 0.4 → top 4.
  // Remaining 6 across tail: order 2 weight 10, order 3 weight 10×1.5=15.
  // → order 2 gets 6 × 10/25 = 2.4, order 3 gets 6 × 15/25 = 3.6.
  auto t = makeTracker(QueueModel::TOP_PRO_LMM);
  t.setTopPriorityShare(0.4);
  t.setLmmBonusMultiplier(1.5);
  t.setLmmOrders({3});
  const auto px = Price::fromDouble(50000.0);
  t.addOrder(BTC, Side::BUY, px, 1, Quantity::fromDouble(10.0),
             Quantity::fromDouble(30.0));
  t.addOrder(BTC, Side::BUY, px, 2, Quantity::fromDouble(10.0),
             Quantity::fromDouble(30.0));
  t.addOrder(BTC, Side::BUY, px, 3, Quantity::fromDouble(10.0),
             Quantity::fromDouble(30.0));

  std::vector<std::pair<OrderId, Quantity>> filled;
  t.onTrade(BTC, px, Quantity::fromDouble(10.0), filled);
  EXPECT_NEAR(fillFor(filled, 1).toDouble(), 4.0, 1e-3);
  EXPECT_NEAR(fillFor(filled, 2).toDouble(), 2.4, 1e-3);
  EXPECT_NEAR(fillFor(filled, 3).toDouble(), 3.6, 1e-3);
}

TEST(TopProLmm, TopShareCappedByOrderRemaining)
{
  // Top is small (size 1); topShare=0.4 → 4 raw. Cap to remaining 1.
  // Rest 9 distributes pro-rata across tail (10 + 10 → 4.5 + 4.5).
  auto t = makeTracker(QueueModel::TOP_PRO_LMM);
  t.setTopPriorityShare(0.4);
  const auto px = Price::fromDouble(50000.0);
  t.addOrder(BTC, Side::BUY, px, 1, Quantity::fromDouble(1.0),
             Quantity::fromDouble(21.0));
  t.addOrder(BTC, Side::BUY, px, 2, Quantity::fromDouble(10.0),
             Quantity::fromDouble(21.0));
  t.addOrder(BTC, Side::BUY, px, 3, Quantity::fromDouble(10.0),
             Quantity::fromDouble(21.0));

  std::vector<std::pair<OrderId, Quantity>> filled;
  t.onTrade(BTC, px, Quantity::fromDouble(10.0), filled);
  EXPECT_NEAR(fillFor(filled, 1).toDouble(), 1.0, 1e-6);
  EXPECT_NEAR(fillFor(filled, 2).toDouble(), 4.5, 1e-3);
  EXPECT_NEAR(fillFor(filled, 3).toDouble(), 4.5, 1e-3);
}

TEST(ProRataWithPriority, MultiplierAllocatesMoreToBoostedOrder)
{
  // Two orders, same size 10. Order 2 has multiplier 1.5. Trade 10.
  // Weights: 10 (id1), 15 (id2). Total 25. id1 = 10×10/25 = 4,
  // id2 = 10×15/25 = 6.
  auto t = makeTracker(QueueModel::PRO_RATA_WITH_PRIORITY);
  const auto px = Price::fromDouble(50000.0);
  t.addOrder(BTC, Side::BUY, px, 1, Quantity::fromDouble(10.0),
             Quantity::fromDouble(20.0));
  t.addOrder(BTC, Side::BUY, px, 2, Quantity::fromDouble(10.0),
             Quantity::fromDouble(20.0));
  t.setOrderPriorityMultiplier(2, 1.5);

  std::vector<std::pair<OrderId, Quantity>> filled;
  t.onTrade(BTC, px, Quantity::fromDouble(10.0), filled);
  EXPECT_NEAR(fillFor(filled, 1).toDouble(), 4.0, 1e-3);
  EXPECT_NEAR(fillFor(filled, 2).toDouble(), 6.0, 1e-3);
}

TEST(ProRataWithPriority, EqualMultipliersBehaveLikePureProRata)
{
  auto t = makeTracker(QueueModel::PRO_RATA_WITH_PRIORITY);
  const auto px = Price::fromDouble(50000.0);
  t.addOrder(BTC, Side::BUY, px, 1, Quantity::fromDouble(10.0),
             Quantity::fromDouble(30.0));
  t.addOrder(BTC, Side::BUY, px, 2, Quantity::fromDouble(20.0),
             Quantity::fromDouble(30.0));
  // No multipliers set: every order defaults to 1.0.
  std::vector<std::pair<OrderId, Quantity>> filled;
  t.onTrade(BTC, px, Quantity::fromDouble(15.0), filled);
  // 15 split 1:2 → 5 / 10.
  EXPECT_NEAR(fillFor(filled, 1).toDouble(), 5.0, 1e-3);
  EXPECT_NEAR(fillFor(filled, 2).toDouble(), 10.0, 1e-3);
}

TEST(TopProLmm, EmptyLmmListFallsBackToProRata)
{
  // LMM list never set → tail distribution should be pure pro-rata.
  auto t = makeTracker(QueueModel::TOP_PRO_LMM);
  t.setTopPriorityShare(0.4);
  const auto px = Price::fromDouble(50000.0);
  t.addOrder(BTC, Side::BUY, px, 1, Quantity::fromDouble(10.0),
             Quantity::fromDouble(30.0));
  t.addOrder(BTC, Side::BUY, px, 2, Quantity::fromDouble(10.0),
             Quantity::fromDouble(30.0));
  t.addOrder(BTC, Side::BUY, px, 3, Quantity::fromDouble(10.0),
             Quantity::fromDouble(30.0));

  std::vector<std::pair<OrderId, Quantity>> filled;
  t.onTrade(BTC, px, Quantity::fromDouble(10.0), filled);
  // Tail orders split 6 evenly: 3 + 3.
  EXPECT_NEAR(fillFor(filled, 1).toDouble(), 4.0, 1e-6);
  EXPECT_NEAR(fillFor(filled, 2).toDouble(), 3.0, 1e-3);
  EXPECT_NEAR(fillFor(filled, 3).toDouble(), 3.0, 1e-3);
}
