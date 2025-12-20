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
#include <mutex>

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
  Quantity quantity;
  Price price;
};

struct PositionState
{
  std::deque<Lot> lots;
  Price realizedPnl{};

  Quantity position() const
  {
    int64_t total = 0;
    for (const auto& lot : lots)
    {
      total += lot.quantity.raw();
    }
    return Quantity::fromRaw(total);
  }

  Price avgEntryPrice() const
  {
    double totalQty = 0.0;
    double totalNotional = 0.0;
    for (const auto& lot : lots)
    {
      double absQty = std::abs(lot.quantity.toDouble());
      totalQty += absQty;
      totalNotional += absQty * lot.price.toDouble();
    }
    if (totalQty == 0.0)
    {
      return Price{};
    }
    return Price::fromDouble(totalNotional / totalQty);
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
    std::lock_guard<std::mutex> lock(_mutex);
    return _states[symbol].position();
  }

  Price getAvgEntryPrice(SymbolId symbol) const
  {
    std::lock_guard<std::mutex> lock(_mutex);
    return _states[symbol].avgEntryPrice();
  }

  Price getRealizedPnl(SymbolId symbol) const
  {
    std::lock_guard<std::mutex> lock(_mutex);
    return _states[symbol].realizedPnl;
  }

  Price getTotalRealizedPnl() const
  {
    std::lock_guard<std::mutex> lock(_mutex);
    int64_t total = 0;
    _states.forEach([&total](SymbolId, const PositionState& s)
                    { total += s.realizedPnl.raw(); });
    return Price::fromRaw(total);
  }

  void onOrderPartiallyFilled(const Order& order, Quantity fillQty) override
  {
    std::lock_guard<std::mutex> lock(_mutex);
    updatePosition(order.symbol, order.side, order.price, fillQty);
  }

  void onOrderFilled(const Order& order) override
  {
    std::lock_guard<std::mutex> lock(_mutex);
    updatePosition(order.symbol, order.side, order.price, order.quantity);
  }

 private:
  void updatePosition(SymbolId symbol, Side side, Price price, Quantity qty)
  {
    auto& s = _states[symbol];
    int64_t signedQty = (side == Side::BUY) ? qty.raw() : -qty.raw();
    int64_t currentPos = s.position().raw();

    bool isOpening = (currentPos >= 0 && signedQty > 0) || (currentPos <= 0 && signedQty < 0);
    bool isClosing = (currentPos > 0 && signedQty < 0) || (currentPos < 0 && signedQty > 0);

    if (isClosing)
    {
      int64_t absSignedQty = std::abs(signedQty);
      int64_t absCurrentPos = std::abs(currentPos);
      int64_t qtyToClose = std::min(absSignedQty, absCurrentPos);
      Price pnl = closePosition(s, Quantity::fromRaw(qtyToClose), price, currentPos > 0);
      s.realizedPnl = Price::fromRaw(s.realizedPnl.raw() + pnl.raw());

      int64_t remaining = absSignedQty - qtyToClose;
      if (remaining > 0)
      {
        Quantity remQty = Quantity::fromRaw(signedQty > 0 ? remaining : -remaining);
        addLot(s, remQty, price);
      }
    }
    else if (isOpening)
    {
      addLot(s, Quantity::fromRaw(signedQty), price);
    }
  }

  void addLot(PositionState& s, Quantity signedQty, Price price)
  {
    if (_method == CostBasisMethod::AVERAGE && !s.lots.empty())
    {
      int64_t newQty = s.lots.front().quantity.raw() + signedQty.raw();
      if (newQty != 0)
      {
        double absOld = std::abs(s.lots.front().quantity.toDouble());
        double absNew = std::abs(signedQty.toDouble());
        double absTotal = std::abs(Quantity::fromRaw(newQty).toDouble());
        double notional = absOld * s.lots.front().price.toDouble() +
                          absNew * price.toDouble();
        s.lots.front().quantity = Quantity::fromRaw(newQty);
        s.lots.front().price = Price::fromDouble(notional / absTotal);
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

  Price closePosition(PositionState& s, Quantity qtyToClose, Price closePrice, bool wasLong)
  {
    int64_t pnl = 0;
    int64_t remaining = qtyToClose.raw();

    while (remaining > 0 && !s.lots.empty())
    {
      Lot& lot = (_method == CostBasisMethod::LIFO) ? s.lots.back() : s.lots.front();

      int64_t lotQty = std::abs(lot.quantity.raw());
      int64_t closeQty = std::min(remaining, lotQty);

      double priceDiff = closePrice.toDouble() - lot.price.toDouble();
      double lotPnlD = priceDiff * Quantity::fromRaw(closeQty).toDouble();
      if (!wasLong)
      {
        lotPnlD = -lotPnlD;
      }
      pnl += Price::fromDouble(lotPnlD).raw();

      remaining -= closeQty;

      if (closeQty >= lotQty)
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
        int64_t newQty = (lot.quantity.raw() > 0) ? (lotQty - closeQty) : -(lotQty - closeQty);
        lot.quantity = Quantity::fromRaw(newQty);
      }
    }

    return Price::fromRaw(pnl);
  }

  CostBasisMethod _method;
  mutable std::mutex _mutex;
  mutable SymbolStateMap<PositionState> _states;
};

}  // namespace flox
