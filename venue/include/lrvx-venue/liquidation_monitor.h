/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "lrvx-venue/messages.h"

#include "lrvx/backtest/liquidation_engine.h"

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace lrvx::venue
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
    lrvx::LeveragedPosition p;
    p.accountId = account;
    p.symbol = symbol_;
    // The monitor's own surface is double-facing; LeveragedPosition is fixed
    // point, so the quantisation happens here, once, on the way in.
    p.quantity = lrvx::Quantity::fromDouble(signedQty);  // + long, - short
    p.entryPrice = lrvx::Price::fromDouble(entry);
    p.equity = lrvx::Volume::fromDouble(equity);
    p.contractMultiplier = lrvx::Quantity::fromDouble(multiplier);
    engine_.openPosition(p);
    pos_[account] = p;
  }

  // Feed a new mark price; returns closing MARKET orders for every account the
  // lrvx engine liquidated (already removed from the monitor's book).
  std::vector<NewOrder> onMark(double mark)
  {
    const lrvx::LiquidationOutcome outcome = engine_.onMark(symbol_, mark);
    std::vector<NewOrder> orders;
    for (uint64_t acct : outcome.liquidated)
    {
      auto it = pos_.find(acct);
      if (it == pos_.end())
      {
        continue;
      }
      const lrvx::LeveragedPosition p = it->second;
      NewOrder o;
      o.id = (++liqSeq_) | kLiqBit;  // synthetic liquidation order id
      o.symbol = symbol_;
      o.side = p.quantity.raw() > 0 ? Side::SELL : Side::BUY;  // close long -> sell
      o.type = OrderType::MARKET;
      o.quantity = Quantity::fromRaw(std::abs(p.quantity.raw()));
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
  lrvx::LiquidationEngine engine_;
  std::unordered_map<uint64_t, lrvx::LeveragedPosition> pos_;
  uint64_t liqSeq_{0};
};

}  // namespace lrvx::venue
