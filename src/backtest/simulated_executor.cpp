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
  _conditional_orders.reserve(kDefaultOrderCapacity);
  _fills.reserve(kDefaultFillCapacity);
  _compositeLogic.setExecutor(this);
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

  // Handle conditional orders (stop, TP, trailing)
  if (isConditionalOrder(accepted.type))
  {
    _conditional_orders.push_back(accepted);
    emitEvent(OrderEventStatus::PENDING_TRIGGER, accepted);

    // Initialize trailing stop state if needed
    if (accepted.type == OrderType::TRAILING_STOP)
    {
      const MarketState& state = getMarketState(accepted.symbol);
      Price currentPrice =
          state.hasTrade ? Price::fromRaw(state.lastTradeRaw) : Price::fromRaw(state.bestBidRaw);

      TrailingState trailing;
      trailing.activationPrice = currentPrice;

      // Calculate initial trigger based on offset or callback rate
      if (accepted.trailingOffset.raw() > 0)
      {
        trailing.currentTrigger = (accepted.side == Side::SELL)
                                      ? Price::fromRaw(currentPrice.raw() - accepted.trailingOffset.raw())
                                      : Price::fromRaw(currentPrice.raw() + accepted.trailingOffset.raw());
      }
      else if (accepted.trailingCallbackRate > 0)
      {
        int64_t offsetRaw = (currentPrice.raw() * accepted.trailingCallbackRate) / 10000;
        trailing.currentTrigger = (accepted.side == Side::SELL)
                                      ? Price::fromRaw(currentPrice.raw() - offsetRaw)
                                      : Price::fromRaw(currentPrice.raw() + offsetRaw);
      }

      _trailing_states.emplace_back(accepted.id, trailing);
    }
    return;
  }

  if (!tryFillOrder(accepted))
  {
    _pending_orders.push_back(accepted);
  }
}

void SimulatedExecutor::cancelOrder(OrderId orderId)
{
  // Check pending orders first
  for (auto it = _pending_orders.begin(); it != _pending_orders.end(); ++it)
  {
    if (it->id == orderId)
    {
      Order canceled = *it;
      emitEvent(OrderEventStatus::CANCELED, canceled);
      *it = _pending_orders.back();
      _pending_orders.pop_back();
      _compositeLogic.onOrderCanceled(canceled);
      return;
    }
  }

  // Check conditional orders
  for (auto it = _conditional_orders.begin(); it != _conditional_orders.end(); ++it)
  {
    if (it->id == orderId)
    {
      Order canceled = *it;
      emitEvent(OrderEventStatus::CANCELED, canceled);

      // Remove trailing state if exists
      for (auto trailingIt = _trailing_states.begin(); trailingIt != _trailing_states.end();
           ++trailingIt)
      {
        if (trailingIt->first == orderId)
        {
          *trailingIt = _trailing_states.back();
          _trailing_states.pop_back();
          break;
        }
      }

      *it = _conditional_orders.back();
      _conditional_orders.pop_back();
      _compositeLogic.onOrderCanceled(canceled);
      return;
    }
  }
}

void SimulatedExecutor::cancelAllOrders(SymbolId symbol)
{
  // Cancel pending orders
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

  // Cancel conditional orders
  i = 0;
  while (i < _conditional_orders.size())
  {
    if (_conditional_orders[i].symbol == symbol)
    {
      OrderId orderId = _conditional_orders[i].id;
      emitEvent(OrderEventStatus::CANCELED, _conditional_orders[i]);

      // Remove trailing state if exists
      for (auto trailingIt = _trailing_states.begin(); trailingIt != _trailing_states.end();
           ++trailingIt)
      {
        if (trailingIt->first == orderId)
        {
          *trailingIt = _trailing_states.back();
          _trailing_states.pop_back();
          break;
        }
      }

      _conditional_orders[i] = _conditional_orders.back();
      _conditional_orders.pop_back();
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

void SimulatedExecutor::submitOCO(const OCOParams& params)
{
  // Register OCO link
  _compositeLogic.registerOCO(params.order1.id, params.order2.id);

  // Submit both orders
  submitOrder(params.order1);
  submitOrder(params.order2);
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
  processConditionalOrders(symbol, state);
}

void SimulatedExecutor::onTrade(SymbolId symbol, Price price, bool /* isBuy */)
{
  MarketState& state = getMarketState(symbol);
  state.lastTradeRaw = price.raw();
  state.hasTrade = true;
  processPendingOrders(symbol, state);
  processConditionalOrders(symbol, state);
  updateTrailingStops(symbol, price);
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
  ev.fillPrice = price;
  ev.exchangeTsNs = now;
  ev.status =
      (order.filledQuantity >= order.quantity) ? OrderEventStatus::FILLED : OrderEventStatus::PARTIALLY_FILLED;

  if (_callback)
  {
    _callback(ev);
  }

  // Notify composite logic for OCO handling
  if (ev.status == OrderEventStatus::FILLED)
  {
    _compositeLogic.onOrderFilled(order);
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

void SimulatedExecutor::emitTrailingUpdate(const Order& order, Price newTrigger)
{
  if (_callback)
  {
    OrderEvent ev;
    ev.status = OrderEventStatus::TRAILING_UPDATED;
    ev.order = order;
    ev.newTrailingPrice = newTrigger;
    ev.exchangeTsNs = _clock.nowNs();
    _callback(ev);
  }
}

bool SimulatedExecutor::isConditionalOrder(OrderType type) const
{
  return type == OrderType::STOP_MARKET || type == OrderType::STOP_LIMIT ||
         type == OrderType::TAKE_PROFIT_MARKET || type == OrderType::TAKE_PROFIT_LIMIT ||
         type == OrderType::TRAILING_STOP;
}

bool SimulatedExecutor::checkStopTrigger(const Order& order, const MarketState& state) const
{
  if (!state.hasTrade)
  {
    return false;
  }

  const int64_t triggerRaw = order.triggerPrice.raw();
  const int64_t priceRaw = state.lastTradeRaw;

  // SELL stop: triggers when price <= triggerPrice (falling)
  // BUY stop: triggers when price >= triggerPrice (rising)
  if (order.side == Side::SELL)
  {
    return priceRaw <= triggerRaw;
  }
  else
  {
    return priceRaw >= triggerRaw;
  }
}

bool SimulatedExecutor::checkTakeProfitTrigger(const Order& order, const MarketState& state) const
{
  if (!state.hasTrade)
  {
    return false;
  }

  const int64_t triggerRaw = order.triggerPrice.raw();
  const int64_t priceRaw = state.lastTradeRaw;

  // SELL TP: triggers when price >= triggerPrice (rising, lock profit on long)
  // BUY TP: triggers when price <= triggerPrice (falling, lock profit on short)
  if (order.side == Side::SELL)
  {
    return priceRaw >= triggerRaw;
  }
  else
  {
    return priceRaw <= triggerRaw;
  }
}

bool SimulatedExecutor::checkTrailingStopTrigger(const Order& order, const TrailingState& trailing,
                                                 const MarketState& state) const
{
  if (!state.hasTrade)
  {
    return false;
  }

  const int64_t priceRaw = state.lastTradeRaw;
  const int64_t triggerRaw = trailing.currentTrigger.raw();

  // SELL trailing: triggers when price drops to/below trigger
  // BUY trailing: triggers when price rises to/above trigger
  if (order.side == Side::SELL)
  {
    return priceRaw <= triggerRaw;
  }
  else
  {
    return priceRaw >= triggerRaw;
  }
}

void SimulatedExecutor::updateTrailingStops(SymbolId symbol, Price currentPrice)
{
  for (auto& [orderId, trailing] : _trailing_states)
  {
    // Find the order
    for (auto& order : _conditional_orders)
    {
      if (order.id != orderId || order.symbol != symbol)
      {
        continue;
      }

      const int64_t priceRaw = currentPrice.raw();
      int64_t newTriggerRaw = trailing.currentTrigger.raw();
      bool updated = false;

      if (order.side == Side::SELL)
      {
        // For SELL trailing: trigger follows price up, but never down
        int64_t offsetRaw = 0;
        if (order.trailingOffset.raw() > 0)
        {
          offsetRaw = order.trailingOffset.raw();
        }
        else if (order.trailingCallbackRate > 0)
        {
          offsetRaw = (priceRaw * order.trailingCallbackRate) / 10000;
        }

        int64_t newCandidate = priceRaw - offsetRaw;
        if (newCandidate > newTriggerRaw)
        {
          newTriggerRaw = newCandidate;
          updated = true;
        }
      }
      else
      {
        // For BUY trailing: trigger follows price down, but never up
        int64_t offsetRaw = 0;
        if (order.trailingOffset.raw() > 0)
        {
          offsetRaw = order.trailingOffset.raw();
        }
        else if (order.trailingCallbackRate > 0)
        {
          offsetRaw = (priceRaw * order.trailingCallbackRate) / 10000;
        }

        int64_t newCandidate = priceRaw + offsetRaw;
        if (newCandidate < newTriggerRaw)
        {
          newTriggerRaw = newCandidate;
          updated = true;
        }
      }

      if (updated)
      {
        trailing.currentTrigger = Price::fromRaw(newTriggerRaw);
        emitTrailingUpdate(order, trailing.currentTrigger);
      }

      break;
    }
  }
}

void SimulatedExecutor::triggerConditionalOrder(Order& order)
{
  emitEvent(OrderEventStatus::TRIGGERED, order);

  // Convert to market or limit order and try to fill
  if (order.type == OrderType::STOP_MARKET || order.type == OrderType::TAKE_PROFIT_MARKET ||
      order.type == OrderType::TRAILING_STOP)
  {
    order.type = OrderType::MARKET;
  }
  else if (order.type == OrderType::STOP_LIMIT || order.type == OrderType::TAKE_PROFIT_LIMIT)
  {
    order.type = OrderType::LIMIT;
  }

  // Try immediate fill, otherwise add to pending
  if (!tryFillOrder(order))
  {
    _pending_orders.push_back(order);
  }
}

void SimulatedExecutor::processConditionalOrders(SymbolId symbol, const MarketState& state)
{
  size_t i = 0;
  while (i < _conditional_orders.size())
  {
    Order& order = _conditional_orders[i];
    if (order.symbol != symbol)
    {
      ++i;
      continue;
    }

    bool triggered = false;

    switch (order.type)
    {
      case OrderType::STOP_MARKET:
      case OrderType::STOP_LIMIT:
        triggered = checkStopTrigger(order, state);
        break;

      case OrderType::TAKE_PROFIT_MARKET:
      case OrderType::TAKE_PROFIT_LIMIT:
        triggered = checkTakeProfitTrigger(order, state);
        break;

      case OrderType::TRAILING_STOP:
      {
        // Find trailing state
        for (const auto& [id, trailing] : _trailing_states)
        {
          if (id == order.id)
          {
            triggered = checkTrailingStopTrigger(order, trailing, state);
            break;
          }
        }
        break;
      }

      default:
        break;
    }

    if (triggered)
    {
      Order triggeredOrder = order;

      // Remove from conditional orders
      _conditional_orders[i] = _conditional_orders.back();
      _conditional_orders.pop_back();

      // Remove trailing state if exists
      for (auto trailingIt = _trailing_states.begin(); trailingIt != _trailing_states.end();
           ++trailingIt)
      {
        if (trailingIt->first == triggeredOrder.id)
        {
          *trailingIt = _trailing_states.back();
          _trailing_states.pop_back();
          break;
        }
      }

      triggerConditionalOrder(triggeredOrder);
      // Don't increment i, as we swapped from back
    }
    else
    {
      ++i;
    }
  }
}

}  // namespace flox
