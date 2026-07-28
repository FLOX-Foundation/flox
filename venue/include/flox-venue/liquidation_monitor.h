/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/messages.h"

#include "flox/backtest/liquidation_engine.h"

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace flox::venue
{

class LiquidationMonitor
{
 public:
  LiquidationMonitor(SymbolId symbol, double mmFraction, double insuranceCapital = 0.0)
      : symbol_(symbol)
  {
    engine_.addTier(0.0, mmFraction);
    engine_.setInsuranceFundCapital(insuranceCapital);
  }

  void openPosition(uint64_t account, double signedQty, double entry, double equity,
                    double multiplier = 1.0)
  {
    flox::LeveragedPosition p;
    p.accountId = account;
    p.symbol = symbol_;
    p.quantity = signedQty;  // + long, - short
    p.entryPrice = entry;
    p.equity = equity;
    p.contractMultiplier = multiplier;
    engine_.openPosition(p);
    pos_[account] = p;
  }

  // Feed a new mark price; returns closing MARKET orders for every account the
  // flox engine liquidated (already removed from the monitor's book).
  std::vector<NewOrder> onMark(double mark)
  {
    const flox::LiquidationOutcome outcome = engine_.onMark(symbol_, mark);
    std::vector<NewOrder> orders;
    for (uint64_t acct : outcome.liquidated)
    {
      auto it = pos_.find(acct);
      if (it == pos_.end())
      {
        continue;
      }
      const flox::LeveragedPosition p = it->second;
      NewOrder o;
      o.id = (++liqSeq_) | kLiqBit;  // synthetic liquidation order id
      o.symbol = symbol_;
      o.side = p.quantity > 0 ? Side::SELL : Side::BUY;  // close long -> sell
      o.type = OrderType::MARKET;
      o.quantity = Quantity::fromDouble(std::abs(p.quantity));
      o.accountId = acct;
      orders.push_back(o);
      engine_.closePosition(acct, symbol_);
      pos_.erase(it);
    }
    return orders;
  }

  double insuranceFundBalance() const { return engine_.insuranceFundBalance(); }
  size_t openPositions() const { return pos_.size(); }

 private:
  static constexpr OrderId kLiqBit = 0x8000000000000000ULL;

  SymbolId symbol_;
  flox::LiquidationEngine engine_;
  std::unordered_map<uint64_t, flox::LeveragedPosition> pos_;
  uint64_t liqSeq_{0};
};

}  // namespace flox::venue
