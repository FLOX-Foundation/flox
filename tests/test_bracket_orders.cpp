/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/bracket_order.h"
#include "flox/backtest/simulated_clock.h"
#include "flox/backtest/simulated_executor.h"

#include <gtest/gtest.h>

#include <unordered_set>

using namespace flox;

namespace
{
constexpr SymbolId BTC = 1;

BracketOrder makeBracket(uint64_t id, double entryPx, double tpPx, double stopTriggerPx,
                         double qty)
{
  BracketOrder b;
  b.bracketId = id;
  b.symbol = BTC;
  b.entry.side = Side::BUY;
  b.entry.type = OrderType::LIMIT;
  b.entry.price = Price::fromDouble(entryPx);
  b.entry.quantity = Quantity::fromDouble(qty);

  b.takeProfit.side = Side::SELL;
  b.takeProfit.type = OrderType::LIMIT;
  b.takeProfit.price = Price::fromDouble(tpPx);
  b.takeProfit.quantity = Quantity::fromDouble(qty);

  b.stop.side = Side::SELL;
  b.stop.type = OrderType::STOP_MARKET;
  b.stop.triggerPrice = Price::fromDouble(stopTriggerPx);
  b.stop.quantity = Quantity::fromDouble(qty);
  return b;
}

struct Capture
{
  std::vector<OrderEvent> events;
  std::vector<OrderId> filledIds;
};

void wire(SimulatedExecutor& ex, Capture& cap)
{
  ex.setOrderEventCallback([&](const OrderEvent& ev)
                           {
                             cap.events.push_back(ev);
                             if (ev.status == OrderEventStatus::FILLED)
                             {
                               cap.filledIds.push_back(ev.order.id);
                             } });
}
}  // namespace

TEST(BracketOrders, EntryFillsThenTpFillsCancelsStop)
{
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  Capture cap;
  wire(ex, cap);

  BracketOrder b = makeBracket(1, 100.0, 110.0, 90.0, 1.0);
  ex.submitBracket(b);
  EXPECT_EQ(ex.bracketStatus(1).state, BracketState::PENDING_ENTRY);

  // Drop price to 100; entry buy-limit fills.
  ex.onBar(BTC, Price::fromDouble(100.0));
  EXPECT_EQ(ex.bracketStatus(1).state, BracketState::ENTRY_FILLED);

  // Rally to 110; TP sell-limit fills.
  ex.onBar(BTC, Price::fromDouble(110.0));
  EXPECT_EQ(ex.bracketStatus(1).state, BracketState::TP_FILLED);
}

TEST(BracketOrders, EntryFillsThenStopFillsCancelsTp)
{
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  Capture cap;
  wire(ex, cap);

  BracketOrder b = makeBracket(2, 100.0, 110.0, 90.0, 1.0);
  ex.submitBracket(b);

  ex.onBar(BTC, Price::fromDouble(100.0));
  EXPECT_EQ(ex.bracketStatus(2).state, BracketState::ENTRY_FILLED);

  // Drop to 90 triggers stop.
  ex.onBar(BTC, Price::fromDouble(90.0));
  // Stop-market triggers and consumes the bar close; the simulator
  // should have advanced the bracket to STOP_FILLED.
  EXPECT_EQ(ex.bracketStatus(2).state, BracketState::STOP_FILLED);
}

TEST(BracketOrders, CancelBeforeEntryFillCancelsEverything)
{
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  Capture cap;
  wire(ex, cap);

  BracketOrder b = makeBracket(3, 100.0, 110.0, 90.0, 1.0);
  ex.submitBracket(b);
  EXPECT_EQ(ex.bracketStatus(3).state, BracketState::PENDING_ENTRY);

  ex.cancelBracket(3);
  EXPECT_EQ(ex.bracketStatus(3).state, BracketState::CANCELED);
}

TEST(BracketOrders, CancelAfterEntryFillCancelsBothChildren)
{
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  Capture cap;
  wire(ex, cap);

  BracketOrder b = makeBracket(4, 100.0, 110.0, 90.0, 1.0);
  ex.submitBracket(b);
  ex.onBar(BTC, Price::fromDouble(100.0));
  EXPECT_EQ(ex.bracketStatus(4).state, BracketState::ENTRY_FILLED);

  size_t cancelsBefore = 0;
  for (const auto& ev : cap.events)
  {
    if (ev.status == OrderEventStatus::CANCELED)
    {
      ++cancelsBefore;
    }
  }
  ex.cancelBracket(4);
  size_t cancelsAfter = 0;
  for (const auto& ev : cap.events)
  {
    if (ev.status == OrderEventStatus::CANCELED)
    {
      ++cancelsAfter;
    }
  }
  EXPECT_GE(cancelsAfter - cancelsBefore, 2u);
  EXPECT_EQ(ex.bracketStatus(4).state, BracketState::CANCELED);
}

TEST(BracketOrders, BracketStatusReturnsEmptyForUnknownId)
{
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  auto st = ex.bracketStatus(999);
  EXPECT_EQ(st.bracketId, 0u);
  EXPECT_EQ(st.state, BracketState::PENDING_ENTRY);
}

// === T040: child-arm modes ===

TEST(BracketOrders, OnFullFillModeIsDefault)
{
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  EXPECT_EQ(ex.bracketChildArmMode(),
            SimulatedExecutor::BracketArmMode::OnFullFill);
}

TEST(BracketOrders, ArmModeSwitchesAtRuntime)
{
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  ex.setBracketChildArmMode(SimulatedExecutor::BracketArmMode::OnPartialFill);
  EXPECT_EQ(ex.bracketChildArmMode(),
            SimulatedExecutor::BracketArmMode::OnPartialFill);
  ex.setBracketChildArmMode(SimulatedExecutor::BracketArmMode::OnFullFill);
  EXPECT_EQ(ex.bracketChildArmMode(),
            SimulatedExecutor::BracketArmMode::OnFullFill);
}

TEST(BracketOrders, OnPartialFillPreservesHappyPath)
{
  // Mode toggle must not break full-fill happy path.
  SimulatedClock clock;
  SimulatedExecutor ex(clock);
  Capture cap;
  wire(ex, cap);
  ex.setBracketChildArmMode(SimulatedExecutor::BracketArmMode::OnPartialFill);

  BracketOrder b = makeBracket(10, 100.0, 110.0, 90.0, 1.0);
  ex.submitBracket(b);
  ex.onBar(BTC, Price::fromDouble(100.0));
  EXPECT_EQ(ex.bracketStatus(10).state, BracketState::ENTRY_FILLED);

  ex.onBar(BTC, Price::fromDouble(110.0));
  EXPECT_EQ(ex.bracketStatus(10).state, BracketState::TP_FILLED);
}
