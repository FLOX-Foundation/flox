/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Footprint Chart Demo
//
// This demo shows how to use FootprintBar for order flow analysis.
// Strategy logic:
// - Track bid/ask volume at each price level
// - Identify imbalances (aggressive buyers vs sellers)
// - Use delta and imbalance for trade decisions

#include "flox/aggregator/custom/footprint_bar.h"
#include "flox/book/events/trade_event.h"
#include "flox/common.h"

#include <iostream>
#include <random>

using namespace flox;

TradeEvent makeTrade(SymbolId symbol, double price, double qty, int sec, bool isBuy)
{
  TradeEvent ev;
  ev.trade.symbol = symbol;
  ev.trade.price = Price::fromDouble(price);
  ev.trade.quantity = Quantity::fromDouble(qty);
  ev.trade.isBuy = isBuy;
  ev.trade.exchangeTsNs = static_cast<uint64_t>(sec) * 1'000'000'000ULL;
  return ev;
}

void printFootprint(const FootprintBar<32>& footprint)
{
  std::cout << "\n+--------+--------+--------+--------+" << std::endl;
  std::cout << "| Price  |  Bid   |  Ask   | Delta  |" << std::endl;
  std::cout << "+--------+--------+--------+--------+" << std::endl;

  // Collect and sort levels by price
  struct LevelData
  {
    Price price;
    Quantity bid;
    Quantity ask;
    Quantity delta;
  };

  std::vector<LevelData> levels;
  for (size_t i = 0; i < footprint.numLevels(); ++i)
  {
    const auto* lvl = footprint.level(i);
    if (lvl)
    {
      levels.push_back({lvl->price, lvl->bidVolume, lvl->askVolume, lvl->delta()});
    }
  }

  std::sort(levels.begin(), levels.end(),
            [](const LevelData& a, const LevelData& b)
            { return a.price.raw() > b.price.raw(); });

  for (const auto& lvl : levels)
  {
    char deltaSign = lvl.delta.raw() > 0 ? '+' : (lvl.delta.raw() < 0 ? '-' : ' ');

    printf("| %6.1f | %6.1f | %6.1f | %c%5.1f |\n",
           lvl.price.toDouble(), lvl.bid.toDouble(), lvl.ask.toDouble(),
           deltaSign, std::abs(lvl.delta.toDouble()));
  }

  std::cout << "+--------+--------+--------+--------+" << std::endl;
}

int main()
{
  std::cout << "=== Footprint Chart Demo ===" << std::endl;

  constexpr SymbolId SYMBOL = 1;

  // Create footprint bar with $0.50 tick size
  FootprintBar<32> footprint;
  footprint.setTickSize(Price::fromDouble(0.5));

  std::cout << "\nSimulating order flow..." << std::endl;

  // Simulate realistic order flow with some imbalances
  // Scenario: Price moves up with heavy buying at certain levels

  // Heavy buying at 100.0 (breakout level)
  footprint.addTrade(makeTrade(SYMBOL, 100.0, 15.0, 1, true));
  footprint.addTrade(makeTrade(SYMBOL, 100.0, 12.0, 2, true));
  footprint.addTrade(makeTrade(SYMBOL, 100.0, 3.0, 3, false));

  // Mixed activity at 100.5
  footprint.addTrade(makeTrade(SYMBOL, 100.5, 5.0, 4, true));
  footprint.addTrade(makeTrade(SYMBOL, 100.5, 4.0, 5, false));

  // Heavy selling at 101.0 (resistance)
  footprint.addTrade(makeTrade(SYMBOL, 101.0, 2.0, 6, true));
  footprint.addTrade(makeTrade(SYMBOL, 101.0, 10.0, 7, false));
  footprint.addTrade(makeTrade(SYMBOL, 101.0, 8.0, 8, false));

  // Some activity at lower levels
  footprint.addTrade(makeTrade(SYMBOL, 99.5, 4.0, 9, true));
  footprint.addTrade(makeTrade(SYMBOL, 99.5, 6.0, 10, false));

  footprint.addTrade(makeTrade(SYMBOL, 99.0, 8.0, 11, false));
  footprint.addTrade(makeTrade(SYMBOL, 99.0, 3.0, 12, true));

  // Print footprint visualization
  printFootprint(footprint);

  // Analysis
  std::cout << "\n=== Order Flow Analysis ===" << std::endl;
  std::cout << "Total Volume: " << footprint.totalVolume().toDouble() << std::endl;
  std::cout << "Total Delta:  " << footprint.totalDelta().toDouble() << std::endl;

  Price buyPressure = footprint.highestBuyingPressure();
  Price sellPressure = footprint.highestSellingPressure();

  std::cout << "\nHighest buying pressure:  $" << buyPressure.toDouble() << std::endl;
  std::cout << "Highest selling pressure: $" << sellPressure.toDouble() << std::endl;

  // Check imbalances
  Price strongImbalance = footprint.strongestImbalance(0.7);
  if (strongImbalance.raw() != 0)
  {
    const auto* lvl = footprint.levelAt(strongImbalance);
    if (lvl)
    {
      double ratio = lvl->imbalanceRatio();
      std::cout << "\nStrong imbalance at $" << strongImbalance.toDouble()
                << " (ratio: " << (ratio * 100) << "% buy)" << std::endl;
    }
  }

  // Trading signals
  std::cout << "\n=== Trading Signals ===" << std::endl;

  if (footprint.totalDelta().raw() > 0)
  {
    std::cout << "[BIAS] Bullish - Net buying pressure" << std::endl;
  }
  else
  {
    std::cout << "[BIAS] Bearish - Net selling pressure" << std::endl;
  }

  // Check specific levels
  const auto* lvl100 = footprint.levelAt(Price::fromDouble(100.0));
  if (lvl100 && lvl100->imbalanceRatio() > 0.7)
  {
    std::cout << "[SUPPORT] Strong buying at $100.0 - potential support" << std::endl;
  }

  const auto* lvl101 = footprint.levelAt(Price::fromDouble(101.0));
  if (lvl101 && lvl101->imbalanceRatio() < 0.3)
  {
    std::cout << "[RESISTANCE] Strong selling at $101.0 - potential resistance" << std::endl;
  }

  return 0;
}
