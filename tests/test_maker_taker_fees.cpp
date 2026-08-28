/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Fill::isMaker, and the per-side fee rates it unlocks.
//
// test_maker_taker.cpp already proves the executor classifies fills
// correctly -- it reports isMaker on every OrderEvent. Fill dropped that
// flag, so anything built from the fill stream could not tell the two apart
// and BacktestConfig had to document feeRate as a "maker/taker average".
// These tests pin the flag's trip into Fill, and the fee split it enables.

#include "flox/backtest/backtest_result.h"
#include "flox/backtest/simulated_clock.h"
#include "flox/backtest/simulated_executor.h"

#include <gtest/gtest.h>

#include <memory_resource>
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

Fill makeFill(Side side, bool isMaker)
{
  Fill f{};
  f.orderId = 1;
  f.symbol = 1;
  f.side = side;
  f.price = Price::fromDouble(100.0);
  f.quantity = Quantity::fromDouble(1.0);
  f.timestampNs = UnixNanos::fromRaw(1'700'000'000'000'000'000);
  f.isMaker = isMaker;
  return f;
}

// A round trip, because computeStats() returns early with zeroed fees while
// no trade has closed. Both legs carry the same liquidity flag, so the total
// is exactly two fills charged at the side's rate.
double feesFor(const BacktestConfig& cfg, bool isMaker)
{
  BacktestResult r(cfg, 4);
  r.recordFill(makeFill(Side::BUY, isMaker));
  r.recordFill(makeFill(Side::SELL, isMaker));
  return r.computeStats().totalFees;
}

}  // namespace

TEST(MakerTakerFees, RestingLimitFillIsMarkedMakerOnTheFill)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);

  pushBook(exec, 1, 100.0, 0.0, 101.0, 5.0);

  Order o;
  o.id = 1;
  o.symbol = 1;
  o.side = Side::BUY;
  o.type = OrderType::LIMIT;
  o.price = Price::fromDouble(100.0);
  o.quantity = Quantity::fromDouble(2.0);
  exec.submitOrder(o);

  exec.onTrade(1, Price::fromDouble(100.0), Quantity::fromDouble(2.0), false);

  ASSERT_FALSE(exec.fills().empty());
  EXPECT_TRUE(exec.fills().front().isMaker);
}

TEST(MakerTakerFees, MarketFillIsMarkedTakerOnTheFill)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);

  Order o;
  o.id = 2;
  o.symbol = 1;
  o.side = Side::BUY;
  o.type = OrderType::MARKET;
  o.quantity = Quantity::fromDouble(1.0);
  exec.submitOrder(o);

  ASSERT_FALSE(exec.fills().empty());
  EXPECT_FALSE(exec.fills().front().isMaker);
}

TEST(MakerTakerFees, UnsetRatesChargeFeeRateOnBothSides)
{
  // The compatibility guarantee: an untouched config must not move results.
  BacktestConfig cfg{};
  cfg.feeRate = 0.001;
  cfg.usePercentageFee = true;

  EXPECT_DOUBLE_EQ(feesFor(cfg, true), feesFor(cfg, false));
  EXPECT_NEAR(feesFor(cfg, false), 2 * 100.0 * 0.001, 1e-9);
}

TEST(MakerTakerFees, SetRatesAreChargedPerSide)
{
  BacktestConfig cfg{};
  cfg.feeRate = 0.001;
  cfg.usePercentageFee = true;
  cfg.makerFeeRate = 0.0002;
  cfg.takerFeeRate = 0.0005;

  EXPECT_NEAR(feesFor(cfg, true), 2 * 100.0 * 0.0002, 1e-9);
  EXPECT_NEAR(feesFor(cfg, false), 2 * 100.0 * 0.0005, 1e-9);
}

TEST(MakerTakerFees, OnlyOneSideSetLeavesTheOtherOnFeeRate)
{
  BacktestConfig cfg{};
  cfg.feeRate = 0.001;
  cfg.usePercentageFee = true;
  cfg.makerFeeRate = 0.0;  // zero is a real rate, not "unset"

  EXPECT_NEAR(feesFor(cfg, true), 0.0, 1e-9);
  EXPECT_NEAR(feesFor(cfg, false), 2 * 100.0 * 0.001, 1e-9);
}

TEST(MakerTakerFees, FixedFeeIgnoresTheSide)
{
  BacktestConfig cfg{};
  cfg.usePercentageFee = false;
  cfg.fixedFeePerTrade = 2.5;
  cfg.makerFeeRate = 0.0;

  EXPECT_NEAR(feesFor(cfg, true), 2 * 2.5, 1e-9);
  EXPECT_NEAR(feesFor(cfg, false), 2 * 2.5, 1e-9);
}
