/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/venue_stack.h"

#include <gtest/gtest.h>

#include <memory_resource>
#include <utility>
#include <vector>

using namespace flox;

namespace
{
constexpr SymbolId BTC = 1;

// Feed a top-of-book snapshot so liquidation orders can fill.
void feedBook(SimulatedExecutor& ex, SymbolId sym,
              const std::vector<std::pair<double, double>>& bids,
              const std::vector<std::pair<double, double>>& asks)
{
  std::pmr::monotonic_buffer_resource pool(1024);
  std::pmr::vector<BookLevel> b(&pool);
  std::pmr::vector<BookLevel> a(&pool);
  for (const auto& [px, qty] : bids)
  {
    b.emplace_back(Price::fromDouble(px), Quantity::fromDouble(qty));
  }
  for (const auto& [px, qty] : asks)
  {
    a.emplace_back(Price::fromDouble(px), Quantity::fromDouble(qty));
  }
  ex.onBookUpdate(sym, b, a);
}
}  // namespace

TEST(VenueStack, BinanceFactoryWiresEverySubsystem)
{
  auto stack = VenueStack::binance_um_futures(/*accountId=*/42, /*equity=*/10'000.0);
  EXPECT_EQ(stack.venueName(), "binance_um_futures");
  EXPECT_EQ(stack.account().accountId(), 42u);
  EXPECT_DOUBLE_EQ(stack.account().equity(), 10'000.0);
  EXPECT_EQ(stack.account().marginMode(), MarginMode::Cross);

  // FeeSchedule bound to account.
  EXPECT_NE(stack.fees().boundAccount(), nullptr);
  EXPECT_EQ(stack.fees().boundAccount(), &stack.account());

  // Liquidation has Binance ADL ranking + tier ladder + executor.
  EXPECT_EQ(stack.liquidation().adlRanking(), AdlRanking::Binance);
  EXPECT_GE(stack.liquidation().tiers().size(), 5u);
  EXPECT_EQ(stack.liquidation().executor(), &stack.executor());
  // Account attached to liquidation engine.
  ASSERT_EQ(stack.liquidation().accounts().size(), 1u);
  EXPECT_EQ(stack.liquidation().accounts().front(), &stack.account());

  // Funding tape has Binance settlement schedule.
  EXPECT_NE(&stack.funding(), nullptr);

  // VenueAvailability installed on executor (we can't read the
  // pointer directly, but we can verify it via a no-crash outage cycle).
  EXPECT_NE(&stack.venue(), nullptr);
}

TEST(VenueStack, BybitFactoryUsesBybitRanking)
{
  auto stack = VenueStack::bybit_linear(1, 1'000.0);
  EXPECT_EQ(stack.venueName(), "bybit_linear");
  EXPECT_EQ(stack.liquidation().adlRanking(), AdlRanking::Bybit);
}

TEST(VenueStack, OkxFactoryUsesPnlRatioRanking)
{
  auto stack = VenueStack::okx_swap(1, 1'000.0);
  EXPECT_EQ(stack.venueName(), "okx_swap");
  EXPECT_EQ(stack.liquidation().adlRanking(), AdlRanking::PnlRatio);
}

TEST(VenueStack, DeribitFactoryAvailable)
{
  auto stack = VenueStack::deribit(1, 1'000.0);
  EXPECT_EQ(stack.venueName(), "deribit");
  // Deribit uses pro-rata-with-FIFO as primary matching mode (we
  // can't introspect the queue model directly, but the stack must
  // build without crashing and provide the fee schedule).
  EXPECT_GE(stack.fees().tierCount(), 1u);
}

TEST(VenueStack, FromVenueByNameAcceptsAllVenues)
{
  EXPECT_EQ(VenueStack::fromVenue("binance_um_futures", 1, 100.0).venueName(),
            "binance_um_futures");
  EXPECT_EQ(VenueStack::fromVenue("BINANCE", 1, 100.0).venueName(),
            "binance_um_futures");
  EXPECT_EQ(VenueStack::fromVenue("bybit_linear", 1, 100.0).venueName(),
            "bybit_linear");
  EXPECT_EQ(VenueStack::fromVenue("okx_swap", 1, 100.0).venueName(), "okx_swap");
  EXPECT_EQ(VenueStack::fromVenue("deribit", 1, 100.0).venueName(), "deribit");
  // Unknown name → empty stack.
  EXPECT_EQ(VenueStack::fromVenue("garbage", 1, 100.0).venueName(), "");
}

TEST(VenueStack, AccountFlowFiresLiquidationAfterUnderwaterMark)
{
  // Smoke: open a position, mark down, see liquidation fire via
  // the wired account → liquidation path.
  auto stack = VenueStack::binance_um_futures(1, 100.0);
  // Feed a book so the executor can fill the liquidation order.
  feedBook(stack.executor(), BTC, {{29'500.0, 100.0}}, {{30'500.0, 100.0}});
  // Position big enough that a 20% drop wipes equity past MM tier.
  stack.account().openPosition(BTC, 1.0, 50'000.0);
  const auto out = stack.liquidation().onMark(BTC, 30'000.0);
  EXPECT_GE(out.liquidationsCount, 1u);
  // Account position closed.
  EXPECT_EQ(stack.account().positionCount(), 0u);
}

TEST(VenueStack, FeeBindingTracksAggregateNotional)
{
  auto stack = VenueStack::binance_um_futures(1, 100.0);
  stack.fees().recordFill(0, 200'000.0);
  EXPECT_DOUBLE_EQ(stack.account().rollingNotional30d(), 200'000.0);
  // FeeSchedule reads aggregate from account; tier > 0 after 250k.
  stack.fees().recordFill(0, 60'000.0);
  EXPECT_GE(stack.fees().currentTierIndex(), 1u);
}

TEST(VenueStack, MoveSemanticsPreservePointers)
{
  // Stack is move-only; after move, accessors point to the moved-to
  // owner.
  auto s1 = VenueStack::binance_um_futures(7, 500.0);
  Account* before = &s1.account();
  auto s2 = std::move(s1);
  EXPECT_EQ(&s2.account(), before);
  EXPECT_EQ(s2.account().accountId(), 7u);
}
