/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Market Profile (TPO) Demo
//
// This demo shows how to use MarketProfile for session analysis.
// Key concepts:
// - TPO (Time Price Opportunity) - price + time period letter
// - POC (Point of Control) - price with most TPOs
// - Value Area - 70% of TPOs concentrated
// - Initial Balance - first hour of trading (periods A + B)
// - Single Prints - prices visited only once (potential S/R)

#include "flox/aggregator/custom/market_profile.h"
#include "flox/book/events/trade_event.h"
#include "flox/common.h"

#include <iostream>
#include <random>

using namespace flox;

TradeEvent makeTrade(SymbolId symbol, double price, double qty, uint64_t tsNs)
{
  TradeEvent ev;
  ev.trade.symbol = symbol;
  ev.trade.price = Price::fromDouble(price);
  ev.trade.quantity = Quantity::fromDouble(qty);
  ev.trade.isBuy = true;
  ev.trade.exchangeTsNs = tsNs;
  return ev;
}

void printMarketProfile(const MarketProfile<64, 26>& profile)
{
  std::cout << "\n=== Market Profile Visualization ===" << std::endl;

  // Collect and sort levels
  struct LevelInfo
  {
    Price price;
    std::string tpos;
    uint32_t count;
  };

  std::vector<LevelInfo> levels;
  for (size_t i = 0; i < profile.numLevels(); ++i)
  {
    const auto* lvl = profile.level(i);
    if (lvl)
    {
      std::string tpos;
      for (size_t p = 0; p <= profile.currentPeriod(); ++p)
      {
        if (lvl->hasPeriod(p))
        {
          tpos += MarketProfile<>::periodLetter(p);
        }
      }
      levels.push_back({lvl->price, tpos, lvl->tpoCount});
    }
  }

  std::sort(levels.begin(), levels.end(),
            [](const LevelInfo& a, const LevelInfo& b)
            { return a.price.raw() > b.price.raw(); });

  // Find POC for marking
  Price poc = profile.poc();
  Price vaLow = profile.valueAreaLow();
  Price vaHigh = profile.valueAreaHigh();
  Price ibLow = profile.initialBalanceLow();
  Price ibHigh = profile.initialBalanceHigh();

  std::cout << "\n Price  | TPOs              | Markers" << std::endl;
  std::cout << "--------+-------------------+---------" << std::endl;

  for (const auto& lvl : levels)
  {
    std::string markers;
    if (lvl.price == poc)
    {
      markers += " <POC>";
    }
    if (lvl.price == vaHigh)
    {
      markers += " <VAH>";
    }
    if (lvl.price == vaLow)
    {
      markers += " <VAL>";
    }
    if (lvl.price == ibHigh)
    {
      markers += " <IBH>";
    }
    if (lvl.price == ibLow)
    {
      markers += " <IBL>";
    }
    if (lvl.price == profile.highPrice())
    {
      markers += " <HIGH>";
    }
    if (lvl.price == profile.lowPrice())
    {
      markers += " <LOW>";
    }

    printf(" %5.1f  | %-17s |%s\n", lvl.price.toDouble(), lvl.tpos.c_str(), markers.c_str());
  }
}

int main()
{
  std::cout << "=== Market Profile (TPO) Demo ===" << std::endl;

  constexpr SymbolId SYMBOL = 1;
  constexpr uint64_t MINUTE_NS = 60ULL * 1'000'000'000ULL;

  // Create market profile with $1 tick size, 30-min periods
  MarketProfile<64, 26> profile;
  profile.setTickSize(Price::fromDouble(1.0));
  profile.setPeriodDuration(std::chrono::minutes(30));
  profile.setSessionStart(0);

  std::cout << "\nSimulating 3-hour trading session..." << std::endl;

  std::mt19937 rng(42);

  // Period A (0-30 min): Opening range, exploration
  std::cout << "Period A: Opening exploration" << std::endl;
  for (int i = 0; i < 100; ++i)
  {
    double price = 100.0 + (rng() % 6) - 2;  // 98-103
    profile.addTrade(makeTrade(SYMBOL, price, 1.0, i * MINUTE_NS / 4));
  }

  // Period B (30-60 min): IB extension up
  std::cout << "Period B: IB extension up" << std::endl;
  for (int i = 0; i < 100; ++i)
  {
    double price = 101.0 + (rng() % 5) - 1;  // 100-104
    profile.addTrade(makeTrade(SYMBOL, price, 1.0, 30 * MINUTE_NS + i * MINUTE_NS / 4));
  }

  // Period C (60-90 min): Balance at POC
  std::cout << "Period C: Balance around POC" << std::endl;
  for (int i = 0; i < 100; ++i)
  {
    double price = 101.0 + (rng() % 4) - 1;  // 100-103
    profile.addTrade(makeTrade(SYMBOL, price, 1.0, 60 * MINUTE_NS + i * MINUTE_NS / 4));
  }

  // Period D (90-120 min): Breakout attempt
  std::cout << "Period D: Breakout attempt" << std::endl;
  for (int i = 0; i < 80; ++i)
  {
    double price = 102.0 + (rng() % 4);  // 102-105
    profile.addTrade(makeTrade(SYMBOL, price, 1.0, 90 * MINUTE_NS + i * MINUTE_NS / 4));
  }

  // Period E (120-150 min): Rejection, back to value
  std::cout << "Period E: Rejection back to value" << std::endl;
  for (int i = 0; i < 100; ++i)
  {
    double price = 101.0 + (rng() % 3);  // 101-103
    profile.addTrade(makeTrade(SYMBOL, price, 1.0, 120 * MINUTE_NS + i * MINUTE_NS / 4));
  }

  // Period F (150-180 min): Continuation or reversal
  std::cout << "Period F: Late session" << std::endl;
  for (int i = 0; i < 80; ++i)
  {
    double price = 100.0 + (rng() % 4);  // 100-103
    profile.addTrade(makeTrade(SYMBOL, price, 1.0, 150 * MINUTE_NS + i * MINUTE_NS / 4));
  }

  // Print profile
  printMarketProfile(profile);

  // Analysis
  std::cout << "\n=== Profile Analysis ===" << std::endl;
  std::cout << "Session High: $" << profile.highPrice().toDouble() << std::endl;
  std::cout << "Session Low:  $" << profile.lowPrice().toDouble() << std::endl;
  std::cout << "POC:          $" << profile.poc().toDouble() << std::endl;
  std::cout << "Value Area:   $" << profile.valueAreaLow().toDouble()
            << " - $" << profile.valueAreaHigh().toDouble() << std::endl;
  std::cout << "Initial Balance: $" << profile.initialBalanceLow().toDouble()
            << " - $" << profile.initialBalanceHigh().toDouble() << std::endl;

  // Check for poor highs/lows
  std::cout << "\n=== Structure Analysis ===" << std::endl;
  if (profile.isPoorHigh())
  {
    std::cout << "[WARNING] Poor high detected - weak resistance, potential continuation"
              << std::endl;
  }
  else
  {
    std::cout << "[OK] Strong high - good resistance" << std::endl;
  }

  if (profile.isPoorLow())
  {
    std::cout << "[WARNING] Poor low detected - weak support, potential breakdown" << std::endl;
  }
  else
  {
    std::cout << "[OK] Strong low - good support" << std::endl;
  }

  // Single prints analysis
  auto [singleCount, singles] = profile.singlePrints();
  if (singleCount > 0)
  {
    std::cout << "\n=== Single Prints (potential S/R) ===" << std::endl;
    for (size_t i = 0; i < singleCount; ++i)
    {
      std::cout << "  $" << singles[i].toDouble() << std::endl;
    }
  }

  // Trading signals
  std::cout << "\n=== Trading Signals ===" << std::endl;
  Price poc = profile.poc();
  Price vaLow = profile.valueAreaLow();
  Price vaHigh = profile.valueAreaHigh();

  std::cout << "[STRATEGY] Fade extremes, trade to value" << std::endl;
  std::cout << "  - If price above VAH ($" << vaHigh.toDouble() << "): Look for shorts to POC"
            << std::endl;
  std::cout << "  - If price below VAL ($" << vaLow.toDouble() << "): Look for longs to POC"
            << std::endl;
  std::cout << "  - POC ($" << poc.toDouble() << ") is fair value - expect rotation around it"
            << std::endl;

  return 0;
}
