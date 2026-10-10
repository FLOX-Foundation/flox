/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Every SignalType has to arrive at the C boundary as itself.
//
// The switch that fills LrvxSignal.order_type covered ten of the thirteen;
// OCO and the two liquidity signals fell into its default and came out as
// LRVX_SIGNAL_TYPE_MARKET with price 0 and quantity 0. Both shipped bindings
// can emit a liquidity signal (Python through Strategy, Node through
// SignalBuilder), so a pre-trade gate filtering on notional waved them
// through and had nothing to tell them apart from a real market order by.
//
// The strategy side goes through BridgeStrategy directly because the C ABI
// has no emitter for these three; that is the same path python/strategy_
// bindings.h and node/src/strategy.h take.

#include "lrvx/capi/bridge_strategy.h"
#include "lrvx/capi/lrvx_capi.h"
#include "lrvx/capi/order_type_names.hpp"

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

void onSignal(void* ud, const LrvxSignal* s)
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
  LrvxRegistryHandle registry{nullptr};
  LrvxStrategyHandle strategy{nullptr};
  LrvxRunnerHandle runner{nullptr};
  uint32_t symbol{0};

  ~RunnerCtx()
  {
    if (runner != nullptr)
    {
      lrvx_runner_stop(runner);
      lrvx_runner_destroy(runner);
    }
    if (strategy != nullptr)
    {
      lrvx_strategy_destroy(strategy);
    }
    if (registry != nullptr)
    {
      lrvx_registry_destroy(registry);
    }
  }
};

void makeRunner(RunnerCtx& ctx, Seen& seen)
{
  ctx.registry = lrvx_registry_create();
  ASSERT_NE(ctx.registry, nullptr);
  ctx.symbol = lrvx_registry_add_symbol(ctx.registry, "test", "ETH-USDC", 0.01);

  LrvxStrategyCallbacks cb{};
  uint32_t syms[] = {ctx.symbol};
  ctx.strategy = lrvx_strategy_create(/*id=*/1, syms, 1, ctx.registry, cb);
  ASSERT_NE(ctx.strategy, nullptr);

  ctx.runner = lrvx_runner_create(ctx.registry, onSignal, &seen);
  ASSERT_NE(ctx.runner, nullptr);
  lrvx_runner_add_strategy(ctx.runner, ctx.strategy);
  lrvx_runner_start(ctx.runner);
}

lrvx::BridgeStrategy* bridge(LrvxStrategyHandle h)
{
  return static_cast<lrvx::BridgeStrategy*>(h);
}

}  // namespace

TEST(CapiSignalCodesTest, ProvideLiquidityIsNotReportedAsAMarketOrder)
{
  Seen seen;
  RunnerCtx ctx;
  makeRunner(ctx, seen);

  bridge(ctx.strategy)
      ->publicEmitProvideLiquidity(ctx.symbol, lrvx::Price::fromDouble(1800.0),
                                   lrvx::Price::fromDouble(2200.0),
                                   lrvx::Quantity::fromDouble(5.0));

  ASSERT_EQ(seen.types.size(), 1u);
  EXPECT_EQ(seen.types[0], LRVX_SIGNAL_TYPE_PROVIDE_LIQUIDITY);
  EXPECT_NE(seen.types[0], LRVX_SIGNAL_TYPE_MARKET);
}

TEST(CapiSignalCodesTest, WithdrawLiquidityIsNotReportedAsAMarketOrder)
{
  Seen seen;
  RunnerCtx ctx;
  makeRunner(ctx, seen);

  bridge(ctx.strategy)->publicEmitWithdrawLiquidity(ctx.symbol, lrvx::Quantity::fromDouble(5.0));

  ASSERT_EQ(seen.types.size(), 1u);
  EXPECT_EQ(seen.types[0], LRVX_SIGNAL_TYPE_WITHDRAW_LIQUIDITY);
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
      ->publicEmitProvideLiquidity(ctx.symbol, lrvx::Price::fromDouble(1800.0),
                                   lrvx::Price::fromDouble(2200.0),
                                   lrvx::Quantity::fromDouble(5.0));
  lrvx_emit_market_buy(ctx.strategy, ctx.symbol, lrvx_quantity_from_double(1.0));

  ASSERT_EQ(seen.types.size(), 2u);
  EXPECT_NE(seen.types[0], seen.types[1]);
  EXPECT_EQ(seen.types[1], LRVX_SIGNAL_TYPE_MARKET);
  EXPECT_NEAR(seen.quantities[1], 1.0, 1e-9);
}

// The code told a gate this wasn't a market order; it still couldn't see
// the range or the size, because LrvxSignal.price / .quantity carry
// nothing for these two types and nothing else crossed the boundary.
TEST(CapiSignalCodesTest, ProvideLiquidityCarriesRangeAndAmountAcrossTheBoundary)
{
  Seen seen;
  RunnerCtx ctx;
  makeRunner(ctx, seen);

  bridge(ctx.strategy)
      ->publicEmitProvideLiquidity(ctx.symbol, lrvx::Price::fromDouble(1800.0),
                                   lrvx::Price::fromDouble(2200.0),
                                   lrvx::Quantity::fromDouble(5.0));

  ASSERT_EQ(seen.types.size(), 1u);
  EXPECT_EQ(seen.types[0], LRVX_SIGNAL_TYPE_PROVIDE_LIQUIDITY);
  EXPECT_NEAR(seen.rangeLowers[0], 1800.0, 1e-9);
  EXPECT_NEAR(seen.rangeUppers[0], 2200.0, 1e-9);
  EXPECT_NEAR(seen.liquidities[0], 5.0, 1e-9);
}

TEST(CapiSignalCodesTest, WithdrawLiquidityCarriesAmountAcrossTheBoundary)
{
  Seen seen;
  RunnerCtx ctx;
  makeRunner(ctx, seen);

  bridge(ctx.strategy)->publicEmitWithdrawLiquidity(ctx.symbol, lrvx::Quantity::fromDouble(5.0));

  ASSERT_EQ(seen.types.size(), 1u);
  EXPECT_EQ(seen.types[0], LRVX_SIGNAL_TYPE_WITHDRAW_LIQUIDITY);
  EXPECT_NEAR(seen.liquidities[0], 5.0, 1e-9);
  // Withdraw has no upper bound; both C++ Signal and the wire struct leave
  // it at zero.
  EXPECT_NEAR(seen.rangeUppers[0], 0.0, 1e-9);
}

TEST(CapiSignalCodesTest, EveryCodeHasAName)
{
  using lrvx::capi::signalTypeName;
  EXPECT_STREQ(signalTypeName(LRVX_SIGNAL_TYPE_ICEBERG), "iceberg");
  EXPECT_STREQ(signalTypeName(LRVX_SIGNAL_TYPE_OCO), "oco");
  EXPECT_STREQ(signalTypeName(LRVX_SIGNAL_TYPE_PROVIDE_LIQUIDITY), "provide_liquidity");
  EXPECT_STREQ(signalTypeName(LRVX_SIGNAL_TYPE_WITHDRAW_LIQUIDITY), "withdraw_liquidity");
  EXPECT_STREQ(signalTypeName(LRVX_SIGNAL_TYPE_WITHDRAW_LIQUIDITY + 1), "unknown");
}
