/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Multi-Timeframe Momentum Strategy Demo
//
// This demo shows how to use the bar aggregator system with multiple timeframes.
// Strategy logic:
// - H1 determines trend direction
// - M5 identifies pullbacks in trend direction
// - M1 provides entry signals
//
// Entry: H1 bullish + M5 pullback + M1 reversal candle

#include "flox/aggregator/bar_aggregator.h"
#include "flox/aggregator/bar_matrix.h"
#include "flox/aggregator/bus/bar_bus.h"
#include "flox/aggregator/multi_timeframe_aggregator.h"
#include "flox/aggregator/timeframe.h"
#include "flox/book/events/trade_event.h"
#include "flox/common.h"
#include "flox/engine/abstract_market_data_subscriber.h"

#include <chrono>
#include <iostream>
#include <random>

using namespace flox;

// Multi-timeframe momentum strategy
template <size_t MaxSymbols = 256, size_t MaxTimeframes = 4, size_t Depth = 64>
class MTFMomentumStrategy : public IMarketDataSubscriber
{
 public:
  explicit MTFMomentumStrategy(SymbolId symbol, BarMatrix<MaxSymbols, MaxTimeframes, Depth>* matrix)
      : _symbol(symbol), _matrix(matrix)
  {
  }

  SubscriberId id() const override { return reinterpret_cast<SubscriberId>(this); }

  void onBar(const BarEvent& ev) override
  {
    if (ev.symbol != _symbol)
    {
      return;
    }

    // Only act on M1 bars (fastest timeframe)
    if (ev.barType != BarType::Time || ev.barTypeParam != 60)
    {
      return;
    }

    // Get bars from different timeframes
    const Bar* h1 = _matrix->bar(_symbol, timeframe::H1, 0);
    const Bar* h1_prev = _matrix->bar(_symbol, timeframe::H1, 1);
    const Bar* m5 = _matrix->bar(_symbol, timeframe::M5, 0);
    const Bar* m5_prev = _matrix->bar(_symbol, timeframe::M5, 1);
    const Bar* m1 = _matrix->bar(_symbol, timeframe::M1, 0);

    if (!h1 || !h1_prev || !m5 || !m5_prev || !m1)
    {
      return;  // Not enough data yet
    }

    // Check H1 trend
    bool h1Bullish = h1->close.raw() > h1_prev->close.raw();
    bool h1Bearish = h1->close.raw() < h1_prev->close.raw();

    // Check M5 pullback
    bool m5Pullback = m5->close.raw() < m5->open.raw();  // bearish candle
    bool m5Breakout = m5->close.raw() > m5->open.raw();  // bullish candle

    // Check M1 reversal
    bool m1BullishReversal = m1->close.raw() > m1->open.raw() &&
                             m1->low.raw() < m5_prev->low.raw();
    bool m1BearishReversal = m1->close.raw() < m1->open.raw() &&
                             m1->high.raw() > m5_prev->high.raw();

    // Generate signals
    if (h1Bullish && m5Pullback && m1BullishReversal)
    {
      std::cout << "[SIGNAL] BUY @ " << m1->close.toDouble()
                << " | H1 bullish, M5 pullback, M1 reversal" << std::endl;
      ++_buySignals;
    }
    else if (h1Bearish && m5Breakout && m1BearishReversal)
    {
      std::cout << "[SIGNAL] SELL @ " << m1->close.toDouble()
                << " | H1 bearish, M5 breakout, M1 reversal" << std::endl;
      ++_sellSignals;
    }
  }

  void printStats() const
  {
    std::cout << "\n=== MTF Strategy Stats ===" << std::endl;
    std::cout << "Buy signals:  " << _buySignals << std::endl;
    std::cout << "Sell signals: " << _sellSignals << std::endl;
  }

 private:
  SymbolId _symbol;
  BarMatrix<MaxSymbols, MaxTimeframes, Depth>* _matrix;
  int _buySignals = 0;
  int _sellSignals = 0;
};

// Simulate price data
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

int main()
{
  std::cout << "=== Multi-Timeframe Momentum Strategy Demo ===" << std::endl;

  constexpr SymbolId SYMBOL = 1;

  // Create bar bus
  BarBus bus;

  // Create multi-timeframe aggregator
  MultiTimeframeAggregator<4> aggregator(&bus);
  aggregator.addTimeInterval(std::chrono::seconds(60));    // M1
  aggregator.addTimeInterval(std::chrono::seconds(300));   // M5
  aggregator.addTimeInterval(std::chrono::seconds(3600));  // H1

  // Create bar matrix to store history
  BarMatrix<256, 4, 64> matrix;
  std::array<TimeframeId, 3> timeframes = {timeframe::M1, timeframe::M5, timeframe::H1};
  matrix.configure(timeframes);

  // Create strategy
  MTFMomentumStrategy<256, 4, 64> strategy(SYMBOL, &matrix);

  // Subscribe to bar events
  bus.subscribe(&matrix);
  bus.subscribe(&strategy);

  // Start components
  bus.start();
  aggregator.start();

  // Simulate trending market data
  std::mt19937 rng(42);
  std::normal_distribution<> noise(0.0, 0.5);

  double price = 100.0;
  double trend = 0.01;  // Slight uptrend

  // Simulate 4 hours of data (enough for H1 bars)
  constexpr uint64_t SECOND_NS = 1'000'000'000ULL;
  constexpr uint64_t DURATION = 4 * 3600 * SECOND_NS;

  std::cout << "\nSimulating 4 hours of market data..." << std::endl;

  for (uint64_t ts = 0; ts < DURATION; ts += 5 * SECOND_NS)
  {
    // Add some price movement
    price += trend + noise(rng);

    // Occasional trend reversals
    if (ts % (30 * 60 * SECOND_NS) == 0)
    {
      trend = -trend * 0.8 + noise(rng) * 0.01;
    }

    aggregator.onTrade(makeTrade(SYMBOL, price, 1.0, ts));
  }

  // Stop and flush
  aggregator.stop();
  bus.stop();

  // Print stats
  strategy.printStats();

  std::cout << "\nBar matrix status:" << std::endl;
  std::cout << "  M1 bars: " << (matrix.bar(SYMBOL, timeframe::M1, 0) ? "available" : "none")
            << std::endl;
  std::cout << "  M5 bars: " << (matrix.bar(SYMBOL, timeframe::M5, 0) ? "available" : "none")
            << std::endl;
  std::cout << "  H1 bars: " << (matrix.bar(SYMBOL, timeframe::H1, 0) ? "available" : "none")
            << std::endl;

  return 0;
}
