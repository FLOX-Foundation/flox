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

Order marketBuy(OrderId id, SymbolId sym, double qty)
{
  Order o;
  o.id = id;
  o.symbol = sym;
  o.side = Side::BUY;
  o.type = OrderType::MARKET;
  o.quantity = Quantity::fromDouble(qty);
  return o;
}

Order marketSell(OrderId id, SymbolId sym, double qty)
{
  Order o;
  o.id = id;
  o.symbol = sym;
  o.side = Side::SELL;
  o.type = OrderType::MARKET;
  o.quantity = Quantity::fromDouble(qty);
  return o;
}
}  // namespace

TEST(BacktestSlippage, NoSlippageByDefault)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  pushBook(exec, 1, 100.0, 10.0, 101.0, 10.0);
  exec.submitOrder(marketBuy(1, 1, 1.0));
  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_DOUBLE_EQ(exec.fills()[0].price.toDouble(), 101.0);
}

TEST(BacktestSlippage, FixedBpsBuyAddsOffset)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  SlippageProfile p;
  p.model = SlippageModel::FIXED_BPS;
  p.bps = 100.0;  // 1%
  exec.setDefaultSlippage(p);

  pushBook(exec, 1, 100.0, 10.0, 100.0, 10.0);
  exec.submitOrder(marketBuy(1, 1, 1.0));
  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_NEAR(exec.fills()[0].price.toDouble(), 101.0, 1e-6);
}

TEST(BacktestSlippage, FixedBpsSellSubtractsOffset)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  SlippageProfile p;
  p.model = SlippageModel::FIXED_BPS;
  p.bps = 50.0;  // 0.5%
  exec.setDefaultSlippage(p);

  pushBook(exec, 1, 200.0, 10.0, 200.0, 10.0);
  exec.submitOrder(marketSell(1, 1, 1.0));
  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_NEAR(exec.fills()[0].price.toDouble(), 199.0, 1e-6);
}

TEST(BacktestSlippage, VolumeImpactScalesWithOrderQty)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  SlippageProfile p;
  p.model = SlippageModel::VOLUME_IMPACT;
  p.impactCoeff = 0.01;  // 1% per (ratio=1)
  exec.setDefaultSlippage(p);

  // Level qty 10, order qty 1 -> ratio=0.1 -> 0.1% impact
  pushBook(exec, 1, 100.0, 10.0, 100.0, 10.0);
  exec.submitOrder(marketBuy(1, 1, 1.0));
  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_NEAR(exec.fills()[0].price.toDouble(), 100.1, 1e-3);
}

TEST(BacktestSlippage, PerSymbolOverridesDefault)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  SlippageProfile def;
  def.model = SlippageModel::FIXED_BPS;
  def.bps = 100.0;
  exec.setDefaultSlippage(def);

  SlippageProfile sym;
  sym.model = SlippageModel::NONE;
  exec.setSymbolSlippage(7, sym);

  pushBook(exec, 7, 100.0, 10.0, 100.0, 10.0);
  exec.submitOrder(marketBuy(1, 7, 1.0));
  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_NEAR(exec.fills()[0].price.toDouble(), 100.0, 1e-6);
}

TEST(BacktestSlippage, DefaultAppliesToSymbolWithoutOverride)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  SlippageProfile def;
  def.model = SlippageModel::FIXED_BPS;
  def.bps = 100.0;
  exec.setDefaultSlippage(def);

  // Override symbol 7. Symbol 9 should still use the default.
  SlippageProfile sym;
  sym.model = SlippageModel::NONE;
  exec.setSymbolSlippage(7, sym);

  pushBook(exec, 9, 100.0, 10.0, 100.0, 10.0);
  exec.submitOrder(marketBuy(1, 9, 1.0));
  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_NEAR(exec.fills()[0].price.toDouble(), 101.0, 1e-6);
}

TEST(BacktestSlippage, FixedTicksUsesProfileTickSize)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  SlippageProfile p;
  p.model = SlippageModel::FIXED_TICKS;
  p.ticks = 3;
  p.tickSize = Price::fromDouble(0.25);  // venue tick is 0.25
  exec.setDefaultSlippage(p);

  pushBook(exec, 1, 100.0, 10.0, 100.0, 10.0);
  exec.submitOrder(marketBuy(1, 1, 1.0));
  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_NEAR(exec.fills()[0].price.toDouble(), 100.75, 1e-6);
}

TEST(BacktestSlippage, ApplyConfigPropagatesSlippage)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  BacktestConfig cfg;
  cfg.defaultSlippage = {SlippageModel::FIXED_BPS, 0, Price{}, 10.0, 0.0};
  cfg.perSymbolSlippage.emplace_back(
      7, SlippageProfile{SlippageModel::NONE, 0, Price{}, 0.0, 0.0});
  exec.applyConfig(cfg);

  // Symbol 1 uses default (10 bps).
  pushBook(exec, 1, 100.0, 10.0, 100.0, 10.0);
  exec.submitOrder(marketBuy(1, 1, 1.0));
  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_NEAR(exec.fills()[0].price.toDouble(), 100.1, 1e-6);

  // Symbol 7 is overridden to NONE.
  pushBook(exec, 7, 200.0, 10.0, 200.0, 10.0);
  exec.submitOrder(marketBuy(2, 7, 1.0));
  ASSERT_EQ(exec.fills().size(), 2u);
  EXPECT_NEAR(exec.fills()[1].price.toDouble(), 200.0, 1e-6);
}

TEST(BacktestSlippage, SlippageAndQueueIntegration)
{
  // Slippage only applies to market-style fills, not to queue-matched limit fills.
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::TOB, 1);
  SlippageProfile p;
  p.model = SlippageModel::FIXED_BPS;
  p.bps = 50.0;
  exec.setDefaultSlippage(p);

  pushBook(exec, 1, 100.0, 0.0, 101.0, 5.0);

  // Limit buy at 100 does not cross; queued, no slippage applied when it fills.
  Order limit;
  limit.id = 1;
  limit.symbol = 1;
  limit.side = Side::BUY;
  limit.type = OrderType::LIMIT;
  limit.price = Price::fromDouble(100.0);
  limit.quantity = Quantity::fromDouble(1.0);
  exec.submitOrder(limit);

  exec.onTrade(1, Price::fromDouble(100.0), Quantity::fromDouble(1.0), false);
  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_DOUBLE_EQ(exec.fills()[0].price.toDouble(), 100.0);

  // Market buy gets slippage.
  exec.submitOrder(marketBuy(2, 1, 1.0));
  ASSERT_EQ(exec.fills().size(), 2u);
  EXPECT_NEAR(exec.fills()[1].price.toDouble(), 101.0 * 1.005, 1e-4);
}
