/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Volume Profile Strategy Demo
//
// This demo shows how to use VolumeProfile for trading decisions.
// Strategy logic:
// - Build volume profile from trades
// - Identify Point of Control (POC) and Value Area
// - Trade pullbacks to POC with confirmation from delta

#include "flox/aggregator/custom/volume_profile.h"
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

int main()
{
  std::cout << "=== Volume Profile Strategy Demo ===" << std::endl;

  constexpr SymbolId SYMBOL = 1;

  // Create volume profile with $1 tick size
  VolumeProfile<128> profile;
  profile.setTickSize(Price::fromDouble(1.0));

  std::mt19937 rng(42);
  std::normal_distribution<> priceDist(100.0, 3.0);
  std::uniform_real_distribution<> qtyDist(0.5, 5.0);
  std::bernoulli_distribution sideDist(0.52);  // Slight buy bias

  std::cout << "\nBuilding volume profile from 1000 trades..." << std::endl;

  // Build profile from simulated trades
  for (int i = 0; i < 1000; ++i)
  {
    double price = priceDist(rng);
    double qty = qtyDist(rng);
    bool isBuy = sideDist(rng);

    profile.addTrade(makeTrade(SYMBOL, price, qty, i, isBuy));
  }

  // Analyze profile
  std::cout << "\n=== Volume Profile Analysis ===" << std::endl;
  std::cout << "Total volume: $" << profile.totalVolume().toDouble() << std::endl;
  std::cout << "Total delta:  $" << profile.totalDelta().toDouble() << std::endl;
  std::cout << "POC:          $" << profile.poc().toDouble() << std::endl;
  std::cout << "Value Area:   $" << profile.valueAreaLow().toDouble()
            << " - $" << profile.valueAreaHigh().toDouble() << std::endl;
  std::cout << "Levels:       " << profile.numLevels() << std::endl;

  // Print top 5 volume levels
  std::cout << "\nTop volume levels:" << std::endl;

  struct LevelInfo
  {
    Price price;
    Volume volume;
    Volume delta;
  };

  std::vector<LevelInfo> levels;
  for (size_t i = 0; i < profile.numLevels(); ++i)
  {
    const auto* lvl = profile.level(i);
    if (lvl)
    {
      levels.push_back({lvl->price, lvl->volume, lvl->delta()});
    }
  }

  std::sort(levels.begin(), levels.end(),
            [](const LevelInfo& a, const LevelInfo& b)
            { return a.volume.raw() > b.volume.raw(); });

  for (size_t i = 0; i < std::min<size_t>(5, levels.size()); ++i)
  {
    std::cout << "  $" << levels[i].price.toDouble()
              << " | Vol: $" << levels[i].volume.toDouble()
              << " | Delta: $" << levels[i].delta.toDouble() << std::endl;
  }

  // Trading signals based on profile
  std::cout << "\n=== Trading Signals ===" << std::endl;

  Price poc = profile.poc();
  Price vaLow = profile.valueAreaLow();
  Price vaHigh = profile.valueAreaHigh();
  Volume delta = profile.totalDelta();

  if (delta.raw() > 0)
  {
    std::cout << "[BIAS] Bullish (positive delta)" << std::endl;
    std::cout << "[PLAN] Buy pullbacks to POC at $" << poc.toDouble() << std::endl;
    std::cout << "[PLAN] Buy at Value Area Low $" << vaLow.toDouble() << std::endl;
  }
  else
  {
    std::cout << "[BIAS] Bearish (negative delta)" << std::endl;
    std::cout << "[PLAN] Sell rallies to POC at $" << poc.toDouble() << std::endl;
    std::cout << "[PLAN] Sell at Value Area High $" << vaHigh.toDouble() << std::endl;
  }

  // Simulate new trades and check signals
  std::cout << "\n=== Live Signal Check ===" << std::endl;

  std::vector<double> testPrices = {vaLow.toDouble(), poc.toDouble(), vaHigh.toDouble()};

  for (double testPrice : testPrices)
  {
    Volume volAtPrice = profile.volumeAt(Price::fromDouble(testPrice));

    std::cout << "Price $" << testPrice << ": ";
    if (volAtPrice.raw() > 0)
    {
      std::cout << "HIGH VOLUME zone ($" << volAtPrice.toDouble() << ")";

      if (std::abs(testPrice - poc.toDouble()) < 1.0)
      {
        std::cout << " [POC LEVEL]";
      }
    }
    else
    {
      std::cout << "LOW VOLUME zone";
    }
    std::cout << std::endl;
  }

  return 0;
}
