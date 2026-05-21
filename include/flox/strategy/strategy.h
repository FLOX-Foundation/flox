/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/aggregator/events/bar_event.h"
#include "flox/book/events/book_update_event.h"
#include "flox/book/events/trade_event.h"
#include "flox/engine/symbol_registry.h"
#include "flox/execution/events/order_event.h"
#include "flox/execution/order_tracker.h"
#include "flox/position/abstract_position_manager.h"
#include "flox/strategy/abstract_signal_handler.h"
#include "flox/strategy/abstract_strategy.h"
#include "flox/strategy/signal.h"
#include "flox/strategy/symbol_context.h"
#include "flox/strategy/symbol_state_map.h"

#include <atomic>
#include <deque>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace flox
{

class Strategy : public IStrategy
{
 public:
  // Identifies a single timeframe instance: a `(BarType, param)` pair.
  // For Time bars the param is interval-in-nanoseconds; for Tick /
  // Volume bars it is the tick count or volume threshold the
  // aggregator was configured with.
  struct BarTfKey
  {
    BarType type;
    uint64_t param;
    bool operator==(const BarTfKey& o) const noexcept
    {
      return type == o.type && param == o.param;
    }
  };
  struct BarTfKeyHash
  {
    std::size_t operator()(const BarTfKey& k) const noexcept
    {
      return std::hash<uint8_t>{}(static_cast<uint8_t>(k.type)) ^
             (std::hash<uint64_t>{}(k.param) << 1);
    }
  };

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

  void setSignalHandler(ISignalHandler* handler) override { _signalHandler = handler; }
  void setOrderTracker(OrderTracker* tracker) noexcept { _orderTracker = tracker; }
  void setPositionManager(IPositionManager* pm) noexcept override { _positionManager = pm; }

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
    refreshPosition(c, sym);

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
    refreshPosition(c, sym);

    onSymbolBook(c, ev);
  }

  void onBar(const BarEvent& ev) final
  {
    SymbolId sym = ev.symbol;
    if (!isSubscribed(sym))
    {
      return;
    }

    auto& c = _contexts[sym];
    c.lastTradePrice = ev.bar.close;
    c.lastUpdateNs = ev.bar.endTime.time_since_epoch().count();
    refreshPosition(c, sym);

    // Push into the per-(symbol, timeframe) ring so multi-TF strategies
    // can recall the last N closed bars without bookkeeping by hand.
    auto& ring = _barRings[sym][BarTfKey{ev.barType, ev.barTypeParam}];
    if (ring.size() >= _barRingCapacity)
    {
      ring.pop_front();
    }
    ring.push_back(ev.bar);

    onSymbolBar(c, ev);
  }

  void onOrderEvent(const OrderEvent& ev) override
  {
    SymbolId sym = ev.order.symbol;
    if (!isSubscribed(sym))
    {
      return;
    }
    auto& c = _contexts[sym];
    if (ev.status == OrderEventStatus::FILLED ||
        ev.status == OrderEventStatus::PARTIALLY_FILLED)
    {
      onSymbolFill(c, ev);
    }
    else if (ev.status == OrderEventStatus::QUEUE_POSITION_UPDATED)
    {
      onSymbolQueuePositionChange(c, ev);
    }
    else if (ev.status == OrderEventStatus::MARKET_POSITION_CHANGED)
    {
      onSymbolMarketPositionChange(c, ev);
    }
    else
    {
      onSymbolOrderUpdate(c, ev);
    }
  }

 protected:
  virtual void onSymbolTrade(SymbolContext& ctx, const TradeEvent& ev) {}
  virtual void onSymbolBook(SymbolContext& ctx, const BookUpdateEvent& ev) {}
  virtual void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) {}

  // Order-event hooks. The runner forwards every executor event for an
  // order this strategy emitted (FILLED, PARTIALLY_FILLED, CANCELED,
  // REJECTED, etc) through `onOrderEvent`, which dispatches here.
  // Override `onSymbolFill` for fill notifications (the common case)
  // and `onSymbolOrderUpdate` for everything else (cancels, rejects,
  // pending-trigger transitions, etc). Without these hooks native
  // `stop_market` is unusable — there is no other path for the
  // strategy to learn its stop fired.
  virtual void onSymbolFill(SymbolContext& ctx, const OrderEvent& ev) {}
  virtual void onSymbolOrderUpdate(SymbolContext& ctx, const OrderEvent& ev) {}

  // Resting limit order's queue position moved without any other
  // lifecycle transition. `ev.queueAhead` is the volume in front of
  // the order at its price level at this moment; `ev.queueTotal` is
  // the level's total quantity. Backtest only — live exchanges do
  // not publish queue position.
  virtual void onSymbolQueuePositionChange(SymbolContext& ctx, const OrderEvent& ev) {}

  // Resting limit order's categorical market position transitioned.
  // `ev.marketPosition` is the new state (Best, BehindBest,
  // MidSpread, LevelEmpty, Crossed); `ev.distanceToBestTicks` is
  // signed ticks from best on our side. Backtest only.
  virtual void onSymbolMarketPositionChange(SymbolContext& ctx, const OrderEvent& ev) {}

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

 public:
  // Multi-TF alignment helpers.
  //
  // After a `BarAggregator` of the requested timeframe has emitted at
  // least one bar for the symbol, `lastClosedBar` returns it; before
  // that it returns `std::nullopt`. The ring stores up to
  // `barRingCapacity()` bars per (symbol, tf) and evicts the oldest
  // when full. `lastNClosedBars` returns the most recent `n` in
  // chronological order (oldest first).
  size_t barRingCapacity() const noexcept { return _barRingCapacity; }
  void setBarRingCapacity(size_t n) noexcept { _barRingCapacity = std::max<size_t>(n, 1); }

  std::optional<Bar> lastClosedBar(SymbolId sym, BarType type, uint64_t param) const
  {
    auto symIt = _barRings.find(sym);
    if (symIt == _barRings.end())
    {
      return std::nullopt;
    }
    auto tfIt = symIt->second.find(BarTfKey{type, param});
    if (tfIt == symIt->second.end() || tfIt->second.empty())
    {
      return std::nullopt;
    }
    return tfIt->second.back();
  }

  std::vector<Bar> lastNClosedBars(SymbolId sym, BarType type, uint64_t param, size_t n) const
  {
    std::vector<Bar> out;
    auto symIt = _barRings.find(sym);
    if (symIt == _barRings.end())
    {
      return out;
    }
    auto tfIt = symIt->second.find(BarTfKey{type, param});
    if (tfIt == symIt->second.end())
    {
      return out;
    }
    const auto& ring = tfIt->second;
    size_t take = std::min(n, ring.size());
    out.reserve(take);
    for (size_t i = ring.size() - take; i < ring.size(); ++i)
    {
      out.push_back(ring[i]);
    }
    return out;
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

  // Stop orders
  OrderId emitStopMarket(SymbolId symbol, Side side, Price triggerPrice, Quantity qty)
  {
    OrderId id = nextOrderId();
    emit(Signal::stopMarket(symbol, side, triggerPrice, qty, id));
    return id;
  }

  OrderId emitStopLimit(SymbolId symbol, Side side, Price triggerPrice, Price limitPrice,
                        Quantity qty)
  {
    OrderId id = nextOrderId();
    emit(Signal::stopLimit(symbol, side, triggerPrice, limitPrice, qty, id));
    return id;
  }

  // Take profit orders
  OrderId emitTakeProfitMarket(SymbolId symbol, Side side, Price triggerPrice, Quantity qty)
  {
    OrderId id = nextOrderId();
    emit(Signal::takeProfitMarket(symbol, side, triggerPrice, qty, id));
    return id;
  }

  OrderId emitTakeProfitLimit(SymbolId symbol, Side side, Price triggerPrice, Price limitPrice,
                              Quantity qty)
  {
    OrderId id = nextOrderId();
    emit(Signal::takeProfitLimit(symbol, side, triggerPrice, limitPrice, qty, id));
    return id;
  }

  // Trailing stop
  OrderId emitTrailingStop(SymbolId symbol, Side side, Price offset, Quantity qty)
  {
    OrderId id = nextOrderId();
    emit(Signal::trailingStop(symbol, side, offset, qty, id));
    return id;
  }

  OrderId emitTrailingStopPercent(SymbolId symbol, Side side, int32_t callbackBps, Quantity qty)
  {
    OrderId id = nextOrderId();
    emit(Signal::trailingStopPercent(symbol, side, callbackBps, qty, id));
    return id;
  }

  // Close position (reduce-only market order)
  OrderId emitClosePosition(SymbolId symbol)
  {
    Quantity pos = position(symbol);
    if (pos.raw() == 0)
    {
      return 0;
    }

    OrderId id = nextOrderId();
    Side side = (pos.raw() > 0) ? Side::SELL : Side::BUY;
    Quantity absQty = Quantity::fromRaw(pos.raw() > 0 ? pos.raw() : -pos.raw());

    auto signal = Signal::marketSell(symbol, absQty, id);
    signal.side = side;
    signal.reduceOnly = true;
    emit(signal);
    return id;
  }

  // Limit orders with TimeInForce
  OrderId emitLimitBuy(SymbolId symbol, Price price, Quantity qty, TimeInForce tif)
  {
    OrderId id = nextOrderId();
    auto signal = Signal::limitBuy(symbol, price, qty, id);
    signal.timeInForce = tif;
    emit(signal);
    return id;
  }

  OrderId emitLimitSell(SymbolId symbol, Price price, Quantity qty, TimeInForce tif)
  {
    OrderId id = nextOrderId();
    auto signal = Signal::limitSell(symbol, price, qty, id);
    signal.timeInForce = tif;
    emit(signal);
    return id;
  }

 private:
  OrderId nextOrderId() noexcept
  {
    static std::atomic<OrderId> s_globalOrderId{1};
    return s_globalOrderId++;
  }

  // Pull the latest position from the attached IPositionManager into
  // the per-symbol context so `ctx.position` / `ctx.is_long()` /
  // `ctx.is_flat()` reflect fills the executor has dispatched. Without
  // this hook the SymbolContext.position field is dead — initialised
  // to zero and never updated — which silently produces 0-trade
  // backtests when a strategy guards entries on `ctx.is_flat()` and
  // exits on `ctx.is_long()`.
  void refreshPosition(SymbolContext& c, SymbolId sym) noexcept
  {
    if (_positionManager)
    {
      c.position = _positionManager->getPosition(sym);
    }
  }

  SubscriberId _id;
  ISignalHandler* _signalHandler{nullptr};
  OrderTracker* _orderTracker{nullptr};
  IPositionManager* _positionManager{nullptr};
  std::vector<SymbolId> _symbols;
  std::set<SymbolId> _symbolSet;
  mutable SymbolStateMap<SymbolContext> _contexts;

  // Per-(symbol, timeframe) ring of the most recent closed bars.
  // Capacity is the same for every (symbol, tf) slot; tune with
  // `setBarRingCapacity` when a strategy needs deeper history.
  size_t _barRingCapacity{64};
  std::unordered_map<SymbolId,
                     std::unordered_map<BarTfKey, std::deque<Bar>, BarTfKeyHash>>
      _barRings;
};

}  // namespace flox
