/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/position/abstract_position_manager.h"
#include "flox/strategy/symbol_state_map.h"

#include <deque>

namespace flox
{

enum class CostBasisMethod
{
  FIFO,
  LIFO,
  AVERAGE
};

struct Lot
{
  double quantity;
  double price;
};

struct PositionState
{
  std::deque<Lot> lots;
  double realizedPnl{0.0};

  double position() const
  {
    double total = 0.0;
    for (const auto& lot : lots)
    {
      total += lot.quantity;
    }
    return total;
  }

  double avgEntryPrice() const
  {
    double totalQty = 0.0;
    double totalNotional = 0.0;
    for (const auto& lot : lots)
    {
      totalQty += std::abs(lot.quantity);
      totalNotional += std::abs(lot.quantity) * lot.price;
    }
    return (totalQty > 1e-9) ? (totalNotional / totalQty) : 0.0;
  }
};

class PositionTracker : public IPositionManager
{
 public:
  PositionTracker(SubscriberId id, CostBasisMethod method = CostBasisMethod::FIFO)
      : IPositionManager(id), _method(method)
  {
  }

  void start() override {}
  void stop() override {}

  CostBasisMethod method() const { return _method; }

  Quantity getPosition(SymbolId symbol) const override
  {
    return Quantity::fromDouble(_states[symbol].position());
  }

  Price getAvgEntryPrice(SymbolId symbol) const
  {
    return Price::fromDouble(_states[symbol].avgEntryPrice());
  }

  double getRealizedPnl(SymbolId symbol) const { return _states[symbol].realizedPnl; }

  double getTotalRealizedPnl() const
  {
    double total = 0.0;
    _states.forEach([&total](SymbolId, const PositionState& s)
                    { total += s.realizedPnl; });
    return total;
  }

  void onOrderSubmitted(const Order&) override {}
  void onOrderAccepted(const Order&) override {}
  void onOrderCanceled(const Order&) override {}
  void onOrderExpired(const Order&) override {}
  void onOrderRejected(const Order&, const std::string&) override {}
  void onOrderReplaced(const Order&, const Order&) override {}

  void onOrderPartiallyFilled(const Order& order, Quantity fillQty) override
  {
    updatePosition(order.symbol, order.side, order.price.toDouble(), fillQty.toDouble());
  }

  void onOrderFilled(const Order& order) override
  {
    updatePosition(order.symbol, order.side, order.price.toDouble(), order.quantity.toDouble());
  }

 private:
  void updatePosition(SymbolId symbol, Side side, double price, double qty)
  {
    auto& s = _states[symbol];
    double signedQty = (side == Side::BUY) ? qty : -qty;
    double currentPos = s.position();

    bool isOpening = (currentPos >= 0 && signedQty > 0) || (currentPos <= 0 && signedQty < 0);
    bool isClosing = (currentPos > 0 && signedQty < 0) || (currentPos < 0 && signedQty > 0);

    if (isClosing)
    {
      double qtyToClose = std::min(std::abs(signedQty), std::abs(currentPos));
      double pnl = closePosition(s, qtyToClose, price, currentPos > 0);
      s.realizedPnl += pnl;

      double remaining = std::abs(signedQty) - qtyToClose;
      if (remaining > 1e-9)
      {
        addLot(s, signedQty > 0 ? remaining : -remaining, price);
      }
    }
    else if (isOpening)
    {
      addLot(s, signedQty, price);
    }
  }

  void addLot(PositionState& s, double signedQty, double price)
  {
    if (_method == CostBasisMethod::AVERAGE && !s.lots.empty())
    {
      double oldQty = s.lots.front().quantity;
      double oldPrice = s.lots.front().price;
      double newQty = oldQty + signedQty;
      if (std::abs(newQty) > 1e-9)
      {
        double newPrice =
            (std::abs(oldQty) * oldPrice + std::abs(signedQty) * price) / std::abs(newQty);
        s.lots.front().quantity = newQty;
        s.lots.front().price = newPrice;
      }
      else
      {
        s.lots.clear();
      }
    }
    else
    {
      s.lots.push_back({signedQty, price});
    }
  }

  double closePosition(PositionState& s, double qtyToClose, double closePrice, bool wasLong)
  {
    double pnl = 0.0;
    double remaining = qtyToClose;

    while (remaining > 1e-9 && !s.lots.empty())
    {
      Lot& lot = (_method == CostBasisMethod::LIFO) ? s.lots.back() : s.lots.front();

      double lotQty = std::abs(lot.quantity);
      double closeQty = std::min(remaining, lotQty);

      double lotPnl = (closePrice - lot.price) * closeQty;
      if (!wasLong)
      {
        lotPnl = -lotPnl;
      }
      pnl += lotPnl;

      remaining -= closeQty;

      if (closeQty >= lotQty - 1e-9)
      {
        if (_method == CostBasisMethod::LIFO)
        {
          s.lots.pop_back();
        }
        else
        {
          s.lots.pop_front();
        }
      }
      else
      {
        lot.quantity = (lot.quantity > 0) ? (lotQty - closeQty) : -(lotQty - closeQty);
      }
    }

    return pnl;
  }

  CostBasisMethod _method;
  mutable SymbolStateMap<PositionState> _states;
};

}  // namespace flox
