/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/book/events/book_update_event.h"
#include "flox/book/events/trade_event.h"
#include "flox/engine/symbol_registry.h"
#include "flox/execution/order_tracker.h"
#include "flox/position/abstract_position_manager.h"
#include "flox/strategy/abstract_signal_handler.h"
#include "flox/strategy/abstract_strategy.h"
#include "flox/strategy/signal.h"
#include "flox/strategy/symbol_context.h"
#include "flox/strategy/symbol_state_map.h"

#include <atomic>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>

namespace flox
{

class Strategy : public IStrategy
{
 public:
  Strategy(SubscriberId id, std::vector<SymbolId> symbols, const SymbolRegistry& registry)
      : _id(id), _symbols(std::move(symbols))
  {
    _symbolSet.insert(_symbols.begin(), _symbols.end());
    for (SymbolId sym : _symbols)
    {
      auto info = registry.getSymbolInfo(sym);
      if (!info)
      {
        throw std::invalid_argument("Symbol " + std::to_string(sym) + " not found in registry");
      }
      _contexts[sym] = SymbolContext(info->tickSize);
      _contexts[sym].symbolId = sym;
    }
  }

  Strategy(SubscriberId id, SymbolId symbol, const SymbolRegistry& registry)
      : Strategy(id, std::vector<SymbolId>{symbol}, registry)
  {
  }

  SubscriberId id() const override { return _id; }

  void setSignalHandler(ISignalHandler* handler) noexcept { _signalHandler = handler; }
  void setOrderTracker(OrderTracker* tracker) noexcept { _orderTracker = tracker; }
  void setPositionManager(IPositionManager* pm) noexcept { _positionManager = pm; }

  void onTrade(const TradeEvent& ev) final
  {
    SymbolId sym = ev.trade.symbol;
    if (!isSubscribed(sym))
    {
      return;
    }

    auto& c = _contexts[sym];
    c.lastTradePrice = ev.trade.price;
    c.lastUpdateNs = ev.trade.exchangeTsNs;

    onSymbolTrade(c, ev);
  }

  void onBookUpdate(const BookUpdateEvent& ev) final
  {
    SymbolId sym = ev.update.symbol;
    if (!isSubscribed(sym))
    {
      return;
    }

    auto& c = _contexts[sym];
    c.book.applyBookUpdate(ev);
    c.lastUpdateNs = ev.update.exchangeTsNs;

    onSymbolBook(c, ev);
  }

 protected:
  virtual void onSymbolTrade(SymbolContext& ctx, const TradeEvent& ev) {}
  virtual void onSymbolBook(SymbolContext& ctx, const BookUpdateEvent& ev) {}

  SymbolContext& ctx(SymbolId sym) noexcept { return _contexts[sym]; }
  const SymbolContext& ctx(SymbolId sym) const noexcept { return _contexts[sym]; }

  SymbolContext& ctx() noexcept { return _contexts[_symbols[0]]; }
  const SymbolContext& ctx() const noexcept { return _contexts[_symbols[0]]; }

  SymbolId symbol() const noexcept { return _symbols[0]; }

  bool isSubscribed(SymbolId sym) const noexcept { return _symbolSet.count(sym) > 0; }

  const std::vector<SymbolId>& symbols() const noexcept { return _symbols; }

  // Position and order status queries
  Quantity position(SymbolId sym) const
  {
    return _positionManager ? _positionManager->getPosition(sym) : Quantity{};
  }

  Quantity position() const { return position(_symbols[0]); }

  std::optional<OrderEventStatus> getOrderStatus(OrderId orderId) const
  {
    return _orderTracker ? _orderTracker->getStatus(orderId) : std::nullopt;
  }

  std::optional<OrderState> getOrder(OrderId orderId) const
  {
    return _orderTracker ? _orderTracker->get(orderId) : std::nullopt;
  }

  // Signal emission
  void emit(const Signal& signal)
  {
    if (_signalHandler)
    {
      _signalHandler->onSignal(signal);
    }
  }

  OrderId emitMarketBuy(SymbolId symbol, Quantity qty)
  {
    OrderId id = nextOrderId();
    emit(Signal::marketBuy(symbol, qty, id));
    return id;
  }

  OrderId emitMarketSell(SymbolId symbol, Quantity qty)
  {
    OrderId id = nextOrderId();
    emit(Signal::marketSell(symbol, qty, id));
    return id;
  }

  OrderId emitLimitBuy(SymbolId symbol, Price price, Quantity qty)
  {
    OrderId id = nextOrderId();
    emit(Signal::limitBuy(symbol, price, qty, id));
    return id;
  }

  OrderId emitLimitSell(SymbolId symbol, Price price, Quantity qty)
  {
    OrderId id = nextOrderId();
    emit(Signal::limitSell(symbol, price, qty, id));
    return id;
  }

  void emitCancel(OrderId orderId) { emit(Signal::cancel(orderId)); }
  void emitCancelAll(SymbolId symbol) { emit(Signal::cancelAll(symbol)); }

  void emitModify(OrderId orderId, Price newPrice, Quantity newQty)
  {
    emit(Signal::modify(orderId, newPrice, newQty));
  }

 private:
  OrderId nextOrderId() noexcept
  {
    static std::atomic<OrderId> s_globalOrderId{1};
    return s_globalOrderId++;
  }

  SubscriberId _id;
  ISignalHandler* _signalHandler{nullptr};
  OrderTracker* _orderTracker{nullptr};
  IPositionManager* _positionManager{nullptr};
  std::vector<SymbolId> _symbols;
  std::set<SymbolId> _symbolSet;
  mutable SymbolStateMap<SymbolContext> _contexts;
};

}  // namespace flox
