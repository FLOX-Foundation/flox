/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/simulated_executor.h"

namespace flox
{

SimulatedExecutor::SimulatedExecutor(IClock& clock) : _clock(clock)
{
  _pending_orders.reserve(kDefaultOrderCapacity);
  _fills.reserve(kDefaultFillCapacity);
}

void SimulatedExecutor::setOrderEventCallback(OrderEventCallback cb)
{
  _callback = std::move(cb);
}

void SimulatedExecutor::submitOrder(const Order& order)
{
  Order accepted = order;
  accepted.createdAt = fromUnixNs(_clock.nowNs());

  emitEvent(OrderEventStatus::SUBMITTED, accepted);
  emitEvent(OrderEventStatus::ACCEPTED, accepted);

  if (!tryFillOrder(accepted))
  {
    _pending_orders.push_back(accepted);
  }
}

void SimulatedExecutor::cancelOrder(OrderId orderId)
{
  for (auto it = _pending_orders.begin(); it != _pending_orders.end(); ++it)
  {
    if (it->id == orderId)
    {
      emitEvent(OrderEventStatus::CANCELED, *it);
      *it = _pending_orders.back();
      _pending_orders.pop_back();
      return;
    }
  }
}

void SimulatedExecutor::cancelAllOrders(SymbolId symbol)
{
  size_t i = 0;
  while (i < _pending_orders.size())
  {
    if (_pending_orders[i].symbol == symbol)
    {
      emitEvent(OrderEventStatus::CANCELED, _pending_orders[i]);
      _pending_orders[i] = _pending_orders.back();
      _pending_orders.pop_back();
    }
    else
    {
      ++i;
    }
  }
}

void SimulatedExecutor::replaceOrder(OrderId oldOrderId, const Order& newOrder)
{
  for (auto& order : _pending_orders)
  {
    if (order.id == oldOrderId)
    {
      Order oldOrder = order;
      order = newOrder;

      OrderEvent ev;
      ev.status = OrderEventStatus::REPLACED;
      ev.order = oldOrder;
      ev.newOrder = newOrder;
      ev.exchangeTsNs = _clock.nowNs();
      if (_callback)
      {
        _callback(ev);
      }
      return;
    }
  }
}

void SimulatedExecutor::onBookUpdate(SymbolId symbol, const std::pmr::vector<BookLevel>& bids,
                                     const std::pmr::vector<BookLevel>& asks)
{
  MarketState& state = getMarketState(symbol);

  state.hasBid = !bids.empty();
  state.hasAsk = !asks.empty();
  if (state.hasBid)
  {
    state.bestBidRaw = bids[0].price.raw();
  }
  if (state.hasAsk)
  {
    state.bestAskRaw = asks[0].price.raw();
  }

  processPendingOrders(symbol, state);
}

void SimulatedExecutor::onTrade(SymbolId symbol, Price price, bool /* isBuy */)
{
  MarketState& state = getMarketState(symbol);
  state.lastTradeRaw = price.raw();
  state.hasTrade = true;
  processPendingOrders(symbol, state);
}

SimulatedExecutor::MarketState& SimulatedExecutor::getMarketState(SymbolId symbol) noexcept
{
  if (symbol < kMaxSymbols) [[likely]]
  {
    return _marketStatesFlat[symbol];
  }

  for (auto& [id, state] : _marketStatesOverflow)
  {
    if (id == symbol)
    {
      return state;
    }
  }
  _marketStatesOverflow.emplace_back(symbol, MarketState{});
  return _marketStatesOverflow.back().second;
}

bool SimulatedExecutor::tryFillOrder(Order& order)
{
  const MarketState& state = getMarketState(order.symbol);
  int64_t fillPriceRaw = 0;
  bool canFill = false;

  if (order.type == OrderType::MARKET)
  {
    if (order.side == Side::BUY && state.hasAsk)
    {
      fillPriceRaw = state.bestAskRaw;
      canFill = true;
    }
    else if (order.side == Side::SELL && state.hasBid)
    {
      fillPriceRaw = state.bestBidRaw;
      canFill = true;
    }
    else if (state.hasTrade)
    {
      // Fallback: use last trade price when book data unavailable
      fillPriceRaw = state.lastTradeRaw;
      canFill = true;
    }
  }
  else
  {
    const int64_t orderPriceRaw = order.price.raw();
    if (order.side == Side::BUY && state.hasAsk && orderPriceRaw >= state.bestAskRaw)
    {
      fillPriceRaw = state.bestAskRaw;
      canFill = true;
    }
    else if (order.side == Side::SELL && state.hasBid && orderPriceRaw <= state.bestBidRaw)
    {
      fillPriceRaw = state.bestBidRaw;
      canFill = true;
    }
  }

  if (canFill)
  {
    executeFill(order, Price::fromRaw(fillPriceRaw), order.quantity - order.filledQuantity);
    return true;
  }
  return false;
}

void SimulatedExecutor::processPendingOrders(SymbolId symbol, const MarketState& state)
{
  size_t i = 0;
  while (i < _pending_orders.size())
  {
    Order& order = _pending_orders[i];
    if (order.symbol != symbol)
    {
      ++i;
      continue;
    }

    int64_t fillPriceRaw = 0;
    bool canFill = false;

    if (order.type == OrderType::MARKET)
    {
      if (order.side == Side::BUY && state.hasAsk)
      {
        fillPriceRaw = state.bestAskRaw;
        canFill = true;
      }
      else if (order.side == Side::SELL && state.hasBid)
      {
        fillPriceRaw = state.bestBidRaw;
        canFill = true;
      }
      else if (state.hasTrade)
      {
        // Fallback: use last trade price when book data unavailable
        fillPriceRaw = state.lastTradeRaw;
        canFill = true;
      }
    }
    else
    {
      const int64_t orderPriceRaw = order.price.raw();
      if (order.side == Side::BUY && state.hasAsk && orderPriceRaw >= state.bestAskRaw)
      {
        fillPriceRaw = state.bestAskRaw;
        canFill = true;
      }
      else if (order.side == Side::SELL && state.hasBid && orderPriceRaw <= state.bestBidRaw)
      {
        fillPriceRaw = state.bestBidRaw;
        canFill = true;
      }
    }

    if (canFill)
    {
      executeFill(order, Price::fromRaw(fillPriceRaw), order.quantity - order.filledQuantity);
      _pending_orders[i] = _pending_orders.back();
      _pending_orders.pop_back();
    }
    else
    {
      ++i;
    }
  }
}

void SimulatedExecutor::executeFill(Order& order, Price price, Quantity qty)
{
  const UnixNanos now = _clock.nowNs();

  _fills.push_back({.orderId = order.id,
                    .symbol = order.symbol,
                    .side = order.side,
                    .price = price,
                    .quantity = qty,
                    .timestampNs = now});

  order.filledQuantity = order.filledQuantity + qty;

  OrderEvent ev;
  ev.order = order;
  ev.fillQty = qty;
  ev.exchangeTsNs = now;
  ev.status =
      (order.filledQuantity >= order.quantity) ? OrderEventStatus::FILLED : OrderEventStatus::PARTIALLY_FILLED;

  if (_callback)
  {
    _callback(ev);
  }
}

void SimulatedExecutor::emitEvent(OrderEventStatus status, const Order& order)
{
  if (_callback)
  {
    OrderEvent ev;
    ev.status = status;
    ev.order = order;
    ev.exchangeTsNs = _clock.nowNs();
    _callback(ev);
  }
}

}  // namespace flox
