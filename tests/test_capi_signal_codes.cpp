/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Every SignalType has to arrive at the C boundary as itself.
//
// The switch that fills FloxSignal.order_type covered ten of the thirteen;
// OCO and the two liquidity signals fell into its default and came out as
// FLOX_SIGNAL_TYPE_MARKET with price 0 and quantity 0. Both shipped bindings
// can emit a liquidity signal (Python through Strategy, Node through
// SignalBuilder), so a pre-trade gate filtering on notional waved them
// through and had nothing to tell them apart from a real market order by.
//
// The strategy side goes through BridgeStrategy directly because the C ABI
// has no emitter for these three; that is the same path python/strategy_
// bindings.h and node/src/strategy.h take.

#include "flox/capi/bridge_strategy.h"
#include "flox/capi/flox_capi.h"
#include "flox/capi/order_type_names.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace
{

struct Seen
{
  std::vector<uint8_t> types;
  std::vector<double> prices;
  std::vector<double> quantities;
  std::vector<double> rangeLowers;
  std::vector<double> rangeUppers;
  std::vector<double> liquidities;
};

void onSignal(void* ud, const FloxSignal* s)
{
  auto* seen = static_cast<Seen*>(ud);
  seen->types.push_back(s->order_type);
  seen->prices.push_back(s->price);
  seen->quantities.push_back(s->quantity);
  seen->rangeLowers.push_back(s->range_lower);
  seen->rangeUppers.push_back(s->range_upper);
  seen->liquidities.push_back(s->liquidity);
}

struct RunnerCtx
{
  FloxRegistryHandle registry{nullptr};
  FloxStrategyHandle strategy{nullptr};
  FloxRunnerHandle runner{nullptr};
  uint32_t symbol{0};

  ~RunnerCtx()
  {
    if (runner != nullptr)
    {
      flox_runner_stop(runner);
      flox_runner_destroy(runner);
    }
    if (strategy != nullptr)
    {
      flox_strategy_destroy(strategy);
    }
    if (registry != nullptr)
    {
      flox_registry_destroy(registry);
    }
  }
};

void makeRunner(RunnerCtx& ctx, Seen& seen)
{
  ctx.registry = flox_registry_create();
  ASSERT_NE(ctx.registry, nullptr);
  ctx.symbol = flox_registry_add_symbol(ctx.registry, "test", "ETH-USDC", 0.01);

  FloxStrategyCallbacks cb{};
  uint32_t syms[] = {ctx.symbol};
  ctx.strategy = flox_strategy_create(/*id=*/1, syms, 1, ctx.registry, cb);
  ASSERT_NE(ctx.strategy, nullptr);

  ctx.runner = flox_runner_create(ctx.registry, onSignal, &seen);
  ASSERT_NE(ctx.runner, nullptr);
  flox_runner_add_strategy(ctx.runner, ctx.strategy);
  flox_runner_start(ctx.runner);
}

flox::BridgeStrategy* bridge(FloxStrategyHandle h)
{
  return static_cast<flox::BridgeStrategy*>(h);
}

}  // namespace

TEST(CapiSignalCodesTest, ProvideLiquidityIsNotReportedAsAMarketOrder)
{
  Seen seen;
  RunnerCtx ctx;
  makeRunner(ctx, seen);

  bridge(ctx.strategy)
      ->publicEmitProvideLiquidity(ctx.symbol, flox::Price::fromDouble(1800.0),
                                   flox::Price::fromDouble(2200.0),
                                   flox::Quantity::fromDouble(5.0));

  ASSERT_EQ(seen.types.size(), 1u);
  EXPECT_EQ(seen.types[0], FLOX_SIGNAL_TYPE_PROVIDE_LIQUIDITY);
  EXPECT_NE(seen.types[0], FLOX_SIGNAL_TYPE_MARKET);
}

TEST(CapiSignalCodesTest, WithdrawLiquidityIsNotReportedAsAMarketOrder)
{
  Seen seen;
  RunnerCtx ctx;
  makeRunner(ctx, seen);

  bridge(ctx.strategy)->publicEmitWithdrawLiquidity(ctx.symbol, flox::Quantity::fromDouble(5.0));

  ASSERT_EQ(seen.types.size(), 1u);
  EXPECT_EQ(seen.types[0], FLOX_SIGNAL_TYPE_WITHDRAW_LIQUIDITY);
}

// The gate that made the old behaviour dangerous: reject anything whose
// notional is above a limit. A liquidity signal carries no price and no
// quantity, so it passes -- but only a gate that can see the code knows
// that it should be looking at the range instead.
TEST(CapiSignalCodesTest, ALiquiditySignalIsDistinguishableFromZeroNotionalMarket)
{
  Seen seen;
  RunnerCtx ctx;
  makeRunner(ctx, seen);

  bridge(ctx.strategy)
      ->publicEmitProvideLiquidity(ctx.symbol, flox::Price::fromDouble(1800.0),
                                   flox::Price::fromDouble(2200.0),
                                   flox::Quantity::fromDouble(5.0));
  flox_emit_market_buy(ctx.strategy, ctx.symbol, flox_quantity_from_double(1.0));

  ASSERT_EQ(seen.types.size(), 2u);
  EXPECT_NE(seen.types[0], seen.types[1]);
  EXPECT_EQ(seen.types[1], FLOX_SIGNAL_TYPE_MARKET);
  EXPECT_NEAR(seen.quantities[1], 1.0, 1e-9);
}

// The code told a gate this wasn't a market order; it still couldn't see
// the range or the size, because FloxSignal.price / .quantity carry
// nothing for these two types and nothing else crossed the boundary.
TEST(CapiSignalCodesTest, ProvideLiquidityCarriesRangeAndAmountAcrossTheBoundary)
{
  Seen seen;
  RunnerCtx ctx;
  makeRunner(ctx, seen);

  bridge(ctx.strategy)
      ->publicEmitProvideLiquidity(ctx.symbol, flox::Price::fromDouble(1800.0),
                                   flox::Price::fromDouble(2200.0),
                                   flox::Quantity::fromDouble(5.0));

  ASSERT_EQ(seen.types.size(), 1u);
  EXPECT_EQ(seen.types[0], FLOX_SIGNAL_TYPE_PROVIDE_LIQUIDITY);
  EXPECT_NEAR(seen.rangeLowers[0], 1800.0, 1e-9);
  EXPECT_NEAR(seen.rangeUppers[0], 2200.0, 1e-9);
  EXPECT_NEAR(seen.liquidities[0], 5.0, 1e-9);
}

TEST(CapiSignalCodesTest, WithdrawLiquidityCarriesAmountAcrossTheBoundary)
{
  Seen seen;
  RunnerCtx ctx;
  makeRunner(ctx, seen);

  bridge(ctx.strategy)->publicEmitWithdrawLiquidity(ctx.symbol, flox::Quantity::fromDouble(5.0));

  ASSERT_EQ(seen.types.size(), 1u);
  EXPECT_EQ(seen.types[0], FLOX_SIGNAL_TYPE_WITHDRAW_LIQUIDITY);
  EXPECT_NEAR(seen.liquidities[0], 5.0, 1e-9);
  // Withdraw has no upper bound; both C++ Signal and the wire struct leave
  // it at zero.
  EXPECT_NEAR(seen.rangeUppers[0], 0.0, 1e-9);
}

TEST(CapiSignalCodesTest, EveryCodeHasAName)
{
  using flox::capi::signalTypeName;
  EXPECT_STREQ(signalTypeName(FLOX_SIGNAL_TYPE_ICEBERG), "iceberg");
  EXPECT_STREQ(signalTypeName(FLOX_SIGNAL_TYPE_OCO), "oco");
  EXPECT_STREQ(signalTypeName(FLOX_SIGNAL_TYPE_PROVIDE_LIQUIDITY), "provide_liquidity");
  EXPECT_STREQ(signalTypeName(FLOX_SIGNAL_TYPE_WITHDRAW_LIQUIDITY), "withdraw_liquidity");
  EXPECT_STREQ(signalTypeName(FLOX_SIGNAL_TYPE_WITHDRAW_LIQUIDITY + 1), "unknown");
}
