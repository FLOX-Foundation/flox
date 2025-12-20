/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/engine/symbol_registry.h"
#include "flox/strategy/strategy.h"

#include <cmath>
#include <deque>

namespace demo
{

using namespace flox;

struct PairsConfig
{
  SymbolId leg1{0};
  SymbolId leg2{1};
  double entryZScore{2.0};
  double exitZScore{0.5};
  size_t lookbackPeriod{100};
  Quantity orderSize{Quantity::fromDouble(1.0)};
  double hedgeRatio{1.0};
};

class PairsStrategy : public Strategy
{
 public:
  PairsStrategy(SubscriberId id, PairsConfig cfg, const SymbolRegistry& registry)
      : Strategy(id, {cfg.leg1, cfg.leg2}, registry), _leg1(cfg.leg1), _leg2(cfg.leg2), _cfg(cfg)
  {
  }

  void start() override { _running = true; }
  void stop() override { _running = false; }

 protected:
  void onSymbolBook(SymbolContext& c, const BookUpdateEvent& ev) override
  {
    if (!_running)
    {
      return;
    }

    auto spreadOpt = flox::spread(ctx(_leg1), ctx(_leg2));
    if (!spreadOpt)
    {
      return;
    }

    double spreadValue = spreadOpt->toDouble();
    updateSpreadHistory(spreadValue);

    if (_spreadHistory.size() < _cfg.lookbackPeriod)
    {
      return;
    }

    double z = computeZScore(spreadValue);

    if (!ctx(_leg1).position.isZero())
    {
      if (shouldExit(z))
      {
        closeSpread();
      }
    }
    else if (shouldEnter(z))
    {
      openSpread(z > 0 ? Side::SELL : Side::BUY);
    }
  }

 private:
  void updateSpreadHistory(double spreadValue)
  {
    _spreadHistory.push_back(spreadValue);
    if (_spreadHistory.size() > _cfg.lookbackPeriod)
    {
      _spreadHistory.pop_front();
    }
  }

  double computeZScore(double currentSpread) const
  {
    if (_spreadHistory.empty())
    {
      return 0.0;
    }

    double sum = 0.0;
    double sumSq = 0.0;
    for (double s : _spreadHistory)
    {
      sum += s;
      sumSq += s * s;
    }

    double n = static_cast<double>(_spreadHistory.size());
    double mean = sum / n;
    double variance = (sumSq / n) - (mean * mean);
    double stddev = std::sqrt(std::max(variance, 1e-10));

    return (currentSpread - mean) / stddev;
  }

  bool shouldEnter(double z) const { return std::abs(z) > _cfg.entryZScore; }

  bool shouldExit(double z) const
  {
    if (_isLongSpread)
    {
      return z > -_cfg.exitZScore;
    }
    return z < _cfg.exitZScore;
  }

  void openSpread(Side leg1Side)
  {
    Quantity leg2Qty = Quantity::fromDouble(_cfg.orderSize.toDouble() * _cfg.hedgeRatio);

    if (leg1Side == Side::BUY)
    {
      emitMarketBuy(_leg1, _cfg.orderSize);
      emitMarketSell(_leg2, leg2Qty);
      _isLongSpread = true;
    }
    else
    {
      emitMarketSell(_leg1, _cfg.orderSize);
      emitMarketBuy(_leg2, leg2Qty);
      _isLongSpread = false;
    }
  }

  void closeSpread()
  {
    Quantity leg2Qty = Quantity::fromDouble(_cfg.orderSize.toDouble() * _cfg.hedgeRatio);

    if (_isLongSpread)
    {
      emitMarketSell(_leg1, _cfg.orderSize);
      emitMarketBuy(_leg2, leg2Qty);
    }
    else
    {
      emitMarketBuy(_leg1, _cfg.orderSize);
      emitMarketSell(_leg2, leg2Qty);
    }
  }

  SymbolId _leg1;
  SymbolId _leg2;
  PairsConfig _cfg;
  std::deque<double> _spreadHistory;
  bool _running{false};
  bool _isLongSpread{false};
};

}  // namespace demo
