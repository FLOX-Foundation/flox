/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/backtest_config.h"
#include "flox/backtest/backtest_result.h"
#include "flox/backtest/simulated_clock.h"
#include "flox/backtest/simulated_executor.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

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

Order limitOrder(OrderId id, SymbolId sym, Side side, double price, double qty)
{
  Order o;
  o.id = id;
  o.symbol = sym;
  o.side = side;
  o.type = OrderType::LIMIT;
  o.price = Price::fromDouble(price);
  o.quantity = Quantity::fromDouble(qty);
  return o;
}
}  // namespace

// A limit order that was not marketable on arrival rests in the book. When the
// opposite touch later gaps through it, the venue trades it at the price it
// posted: the gap is not a price improvement handed to the resting side.
TEST(RestingLimitFills, RestingBuyFillsAtItsOwnPriceWhenTheBookGapsThrough)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  pushBook(exec, 1, 99.0, 10.0, 101.0, 10.0);
  exec.submitOrder(limitOrder(1, 1, Side::BUY, 100.0, 2.0));
  ASSERT_EQ(exec.fills().size(), 0u);

  // The book collapses well below our bid.
  pushBook(exec, 1, 89.0, 10.0, 90.0, 10.0);
  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_DOUBLE_EQ(exec.fills()[0].price.toDouble(), 100.0);
  EXPECT_TRUE(exec.fills()[0].isMaker);
}

TEST(RestingLimitFills, RestingSellFillsAtItsOwnPriceWhenTheBookGapsThrough)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  pushBook(exec, 1, 99.0, 10.0, 101.0, 10.0);
  exec.submitOrder(limitOrder(1, 1, Side::SELL, 100.0, 2.0));
  ASSERT_EQ(exec.fills().size(), 0u);

  pushBook(exec, 1, 110.0, 10.0, 111.0, 10.0);
  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_DOUBLE_EQ(exec.fills()[0].price.toDouble(), 100.0);
  EXPECT_TRUE(exec.fills()[0].isMaker);
}

// A limit order that arrives already marketable crosses the book and pays the
// touch. That is a taker fill and it stays one.
TEST(RestingLimitFills, MarketableLimitOnArrivalStaysATakerAtTheTouch)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  pushBook(exec, 1, 99.0, 10.0, 101.0, 10.0);
  exec.submitOrder(limitOrder(1, 1, Side::BUY, 102.0, 2.0));
  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_DOUBLE_EQ(exec.fills()[0].price.toDouble(), 101.0);
  EXPECT_FALSE(exec.fills()[0].isMaker);
}

TEST(RestingLimitFills, MarketOrderKeepsTakingTheTouch)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  pushBook(exec, 1, 99.0, 10.0, 101.0, 10.0);
  Order o;
  o.id = 1;
  o.symbol = 1;
  o.side = Side::BUY;
  o.type = OrderType::MARKET;
  o.quantity = Quantity::fromDouble(2.0);
  exec.submitOrder(o);
  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_DOUBLE_EQ(exec.fills()[0].price.toDouble(), 101.0);
  EXPECT_FALSE(exec.fills()[0].isMaker);
}

namespace
{
struct PassiveRun
{
  double avgFavourableBp{0.0};
  double maxFavourableBp{0.0};
  size_t fills{0};
  size_t makerFills{0};
};

// Quotes both sides one spread off the touch over a random walk and measures
// how much better than its posted price each passive fill came back.
PassiveRun runPassiveQuoting(double perStepSigmaBp, uint64_t seed)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  std::mt19937_64 rng(seed);
  std::normal_distribution<double> step(0.0, perStepSigmaBp / 10000.0);

  double mid = 100.0;
  const double halfSpread = 100.0 * 0.5e-4;  // 1 bp wide book

  PassiveRun out;
  double sumFavourable = 0.0;
  OrderId nextId = 1;
  std::vector<std::pair<OrderId, double>> posted;

  for (int i = 0; i < 3000; ++i)
  {
    mid *= (1.0 + step(rng));
    const double bid = mid - halfSpread;
    const double ask = mid + halfSpread;
    pushBook(exec, 1, bid, 10.0, ask, 10.0);

    const size_t now = exec.fills().size();
    for (size_t f = out.fills; f < now; ++f)
    {
      const auto& fill = exec.fills()[f];
      double postedPx = 0.0;
      for (const auto& p : posted)
      {
        if (p.first == fill.orderId)
        {
          postedPx = p.second;
          break;
        }
      }
      if (postedPx == 0.0)
      {
        continue;
      }
      const double gift = (fill.side == Side::BUY)
                              ? (postedPx - fill.price.toDouble())
                              : (fill.price.toDouble() - postedPx);
      const double bp = (gift > 0.0) ? (gift / postedPx) * 10000.0 : 0.0;
      sumFavourable += bp;
      out.maxFavourableBp = std::max(out.maxFavourableBp, bp);
      if (fill.isMaker)
      {
        ++out.makerFills;
      }
    }
    out.fills = now;

    exec.cancelAllOrders(1);
    // Record the posted prices as the engine's fixed-point values hold them,
    // so the comparison below is against what was really on the book.
    const double buyPx = Price::fromDouble(bid - halfSpread).toDouble();
    const double sellPx = Price::fromDouble(ask + halfSpread).toDouble();
    const OrderId bId = nextId++;
    const OrderId sId = nextId++;
    posted.emplace_back(bId, buyPx);
    posted.emplace_back(sId, sellPx);
    exec.submitOrder(limitOrder(bId, 1, Side::BUY, buyPx, 1.0));
    exec.submitOrder(limitOrder(sId, 1, Side::SELL, sellPx, 1.0));
  }

  out.avgFavourableBp =
      (out.fills > 0) ? (sumFavourable / static_cast<double>(out.fills)) : 0.0;
  return out;
}
}  // namespace

// How far the opposite touch travels between two observations grows with the
// coarseness of the data, so any rule that fills a resting order at the far
// touch leaks more the coarser the bar. Under the posted-price rule the leak
// is zero at every granularity.
TEST(RestingLimitFills, NoFavourableSliceAtAnyDataGranularity)
{
  const double sigmas[] = {0.5, 2.0, 5.0, 20.0, 60.0};
  for (double sigma : sigmas)
  {
    const PassiveRun run = runPassiveQuoting(sigma, 12345u);
    ASSERT_GT(run.fills, 0u) << "no passive fills at sigma " << sigma;
    EXPECT_NEAR(run.avgFavourableBp, 0.0, 1e-9)
        << "per-step sigma " << sigma << " bp, fills " << run.fills
        << ", max favourable " << run.maxFavourableBp << " bp";
  }
}

TEST(RestingLimitFills, EveryPassiveFillIsFlaggedMaker)
{
  const PassiveRun run = runPassiveQuoting(5.0, 999u);
  ASSERT_GT(run.fills, 0u);
  EXPECT_EQ(run.makerFills, run.fills);
}

namespace
{
// Opens and closes a 10-lot position at 100, both sides maker, so the run has
// a closed trade and the fee total is observable.
void roundTripMakerFills(BacktestResult& result)
{
  Fill f{};
  f.orderId = 1;
  f.symbol = 1;
  f.side = Side::BUY;
  f.price = Price::fromDouble(100.0);
  f.quantity = Quantity::fromDouble(10.0);
  f.isMaker = true;
  result.recordFill(f);

  f.orderId = 2;
  f.side = Side::SELL;
  result.recordFill(f);
}
}  // namespace

// A negative per-side rate is a rebate, not a missing value. A venue that pays
// makers has to be expressible, otherwise a rebate strategy cannot be
// backtested at all.
TEST(RestingLimitFills, NegativeMakerRateIsARebate)
{
  BacktestConfig cfg;
  cfg.usePercentageFee = true;
  cfg.feeRate = 0.0005;
  cfg.makerFeeRate = -0.00005;
  cfg.takerFeeRate = 0.0005;
  BacktestResult result(cfg);

  roundTripMakerFills(result);

  // Two legs of 1000 notional at -0.5 bp pay the strategy 0.10.
  EXPECT_NEAR(result.computeStats().totalFees, -0.1, 1e-9);
}

TEST(RestingLimitFills, UnsetPerSideRateFallsBackToFeeRate)
{
  BacktestConfig cfg;
  cfg.usePercentageFee = true;
  cfg.feeRate = 0.0005;
  BacktestResult result(cfg);

  roundTripMakerFills(result);

  EXPECT_NEAR(result.computeStats().totalFees, 1.0, 1e-9);
}
