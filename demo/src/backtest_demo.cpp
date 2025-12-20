/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/backtest_runner.h"
#include "flox/book/events/trade_event.h"
#include "flox/engine/symbol_registry.h"
#include "flox/position/position_tracker.h"
#include "flox/replay/abstract_event_reader.h"
#include "flox/strategy/strategy.h"

#include <deque>
#include <iostream>

using namespace flox;

struct SmaCrossoverConfig
{
  SymbolId symbol{0};
  size_t fast_period{10};
  size_t slow_period{20};
  Quantity order_size{Quantity::fromDouble(1.0)};
};

class SmaCrossoverStrategy : public Strategy
{
 public:
  SmaCrossoverStrategy(SubscriberId id, SmaCrossoverConfig config, const SymbolRegistry& registry)
      : Strategy(id, config.symbol, registry), _config(config)
  {
  }

  void start() override { _running = true; }
  void stop() override { _running = false; }

  size_t tradeCount() const { return _trade_count; }

 protected:
  void onSymbolTrade(SymbolContext& c, const TradeEvent& ev) override
  {
    if (!_running)
    {
      return;
    }

    _prices.push_back(ev.trade.price.toDouble());
    if (_prices.size() > _config.slow_period)
    {
      _prices.pop_front();
    }

    if (_prices.size() < _config.slow_period)
    {
      return;
    }

    double fast_sma = computeSma(_config.fast_period);
    double slow_sma = computeSma(_config.slow_period);
    bool fast_above_slow = fast_sma > slow_sma;

    if (fast_above_slow && !_prev_fast_above_slow && !_long_position)
    {
      if (_short_position)
      {
        closeShort();
      }
      openLong();
    }
    else if (!fast_above_slow && _prev_fast_above_slow && !_short_position)
    {
      if (_long_position)
      {
        closeLong();
      }
      openShort();
    }

    _prev_fast_above_slow = fast_above_slow;
  }

 private:
  double computeSma(size_t period) const
  {
    if (_prices.size() < period)
    {
      return 0.0;
    }
    double sum = 0.0;
    auto it = _prices.end();
    for (size_t i = 0; i < period; ++i)
    {
      sum += *--it;
    }
    return sum / static_cast<double>(period);
  }

  void openLong()
  {
    emitMarketBuy(symbol(), _config.order_size);
    _long_position = true;
    _trade_count++;
  }

  void closeLong()
  {
    emitMarketSell(symbol(), _config.order_size);
    _long_position = false;
    _trade_count++;
  }

  void openShort()
  {
    emitMarketSell(symbol(), _config.order_size);
    _short_position = true;
    _trade_count++;
  }

  void closeShort()
  {
    emitMarketBuy(symbol(), _config.order_size);
    _short_position = false;
    _trade_count++;
  }

  SmaCrossoverConfig _config;
  std::deque<double> _prices;
  bool _running{false};
  bool _prev_fast_above_slow{false};
  bool _long_position{false};
  bool _short_position{false};
  size_t _trade_count{0};
};

int main(int argc, char* argv[])
{
  if (argc < 2)
  {
    std::cerr << "Usage: " << argv[0] << " <data_dir> [symbol_id]\n";
    return 1;
  }

  std::filesystem::path data_dir = argv[1];
  uint32_t symbol_id = (argc > 2) ? std::stoul(argv[2]) : 1;

  replay::ReaderFilter filter;
  filter.symbols = {symbol_id};

  auto reader = replay::createMultiSegmentReader(data_dir, filter);

  BacktestConfig backtest_config;
  backtest_config.initialCapital = 10000.0;
  backtest_config.feeRate = 0.0004;  // 0.04% taker fee

  BacktestRunner runner(backtest_config);

  SmaCrossoverConfig strategy_config;
  strategy_config.symbol = symbol_id;
  strategy_config.fast_period = 10;
  strategy_config.slow_period = 20;
  strategy_config.order_size = Quantity::fromDouble(1.0);

  SymbolRegistry registry;
  SymbolInfo info;
  info.exchange = "BACKTEST";
  info.symbol = "SYM" + std::to_string(symbol_id);
  info.tickSize = Price::fromDouble(0.01);
  registry.registerSymbol(info);

  SmaCrossoverStrategy strategy(1, strategy_config, registry);
  runner.setStrategy(&strategy);

  PositionTracker positions(2);
  runner.addExecutionListener(&positions);

  std::cout << "Running backtest on " << data_dir << " for symbol " << symbol_id << "...\n";

  BacktestResult result = runner.run(*reader);
  auto stats = result.computeStats();

  std::cout << "\n=== Backtest Results ===\n";
  std::cout << "Initial capital: " << stats.initialCapital << "\n";
  std::cout << "Final capital: " << stats.finalCapital << "\n";
  std::cout << "Return: " << stats.returnPct << "%\n";
  std::cout << "\nTotal trades: " << stats.totalTrades << "\n";
  std::cout << "Winning: " << stats.winningTrades << " | Losing: " << stats.losingTrades << "\n";
  std::cout << "Win rate: " << (stats.winRate * 100) << "%\n";
  std::cout << "\nGross PnL: " << stats.totalPnl << "\n";
  std::cout << "Total fees: " << stats.totalFees << "\n";
  std::cout << "Net PnL: " << stats.netPnl << "\n";
  std::cout << "Gross profit: " << stats.grossProfit << " | Gross loss: " << stats.grossLoss << "\n";
  std::cout << "Profit factor: " << stats.profitFactor << "\n";
  std::cout << "\nMax drawdown: " << stats.maxDrawdown << " (" << stats.maxDrawdownPct << "%)\n";
  std::cout << "Sharpe ratio: " << stats.sharpeRatio << "\n";

  std::cout << "\n=== Position Tracker ===\n";
  std::cout << "Final position: " << positions.getPosition(symbol_id).toDouble() << "\n";
  std::cout << "Avg entry price: " << positions.getAvgEntryPrice(symbol_id).toDouble() << "\n";
  std::cout << "Realized PnL: " << positions.getRealizedPnl(symbol_id) << "\n";

  return 0;
}
