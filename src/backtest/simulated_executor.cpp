/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/simulated_executor.h"

#include <algorithm>
#include <cmath>

namespace flox
{

namespace
{
// 1 tick in raw units. Price has a fixed scale (Price::Scale in common.h);
// callers that want per-instrument tick size would pass ticks via their own
// config. Here FIXED_TICKS treats one "tick" as 1/Price::Scale times 1 integer
// price unit, matching how the rest of the engine stores prices.
constexpr int64_t kTickRaw = 1;
}  // namespace

SimulatedExecutor::SimulatedExecutor(IClock& clock) : _clock(clock)
{
  _pending_orders.reserve(kDefaultOrderCapacity);
  _conditional_orders.reserve(kDefaultOrderCapacity);
  _fills.reserve(kDefaultFillCapacity);
  _compositeLogic.setExecutor(this);
  _slippageSetFlat.fill(false);
}

void SimulatedExecutor::setOrderEventCallback(OrderEventCallback cb)
{
  _callback = std::move(cb);
}

void SimulatedExecutor::applyConfig(const BacktestConfig& config)
{
  _defaultSlippage = config.defaultSlippage;
  _slippageSetFlat.fill(false);
  _slippageOverflow.clear();
  for (const auto& [sym, prof] : config.perSymbolSlippage)
  {
    setSymbolSlippage(sym, prof);
  }
  setQueueModel(config.queueModel, config.queueDepth);
}

void SimulatedExecutor::setDefaultSlippage(const SlippageProfile& profile)
{
  _defaultSlippage = profile;
}

void SimulatedExecutor::setSymbolSlippage(SymbolId symbol, const SlippageProfile& profile)
{
  if (symbol < kMaxSymbols)
  {
    _slippageFlat[symbol] = profile;
    _slippageSetFlat[symbol] = true;
    return;
  }
  for (auto& [id, prof] : _slippageOverflow)
  {
    if (id == symbol)
    {
      prof = profile;
      return;
    }
  }
  _slippageOverflow.emplace_back(symbol, profile);
}

void SimulatedExecutor::setQueueModel(QueueModel model, size_t depth)
{
  _queueModel = model;
  _queueTracker.setModel(model, depth);
}

const SlippageProfile& SimulatedExecutor::slippageFor(SymbolId symbol) const
{
  if (symbol < kMaxSymbols && _slippageSetFlat[symbol])
  {
    return _slippageFlat[symbol];
  }
  for (const auto& [id, prof] : _slippageOverflow)
  {
    if (id == symbol)
    {
      return prof;
    }
  }
  return _defaultSlippage;
}

int64_t SimulatedExecutor::applySlippage(int64_t priceRaw, Side side, SymbolId symbol,
                                         Quantity qty, int64_t levelQtyRaw) const
{
  const SlippageProfile& prof = slippageFor(symbol);
  if (prof.model == SlippageModel::NONE)
  {
    return priceRaw;
  }

  int64_t offsetRaw = 0;
  switch (prof.model)
  {
    case SlippageModel::FIXED_TICKS:
      offsetRaw = static_cast<int64_t>(prof.ticks) * kTickRaw;
      break;
    case SlippageModel::FIXED_BPS:
      offsetRaw = static_cast<int64_t>(std::llround(
          static_cast<double>(priceRaw) * (prof.bps * 1e-4)));
      break;
    case SlippageModel::VOLUME_IMPACT:
    {
      const double levelQty = (levelQtyRaw > 0)
                                  ? static_cast<double>(levelQtyRaw)
                                  : 1.0;
      const double ratio = static_cast<double>(qty.raw()) / levelQty;
      offsetRaw = static_cast<int64_t>(
          std::llround(static_cast<double>(priceRaw) * prof.impactCoeff * ratio));
      break;
    }
    case SlippageModel::NONE:
      break;
  }

  return (side == Side::BUY) ? priceRaw + offsetRaw : priceRaw - offsetRaw;
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

    if (accepted.type == OrderType::TRAILING_STOP)
    {
      const MarketState& state = getMarketState(accepted.symbol);
      Price currentPrice =
          state.hasTrade ? Price::fromRaw(state.lastTradeRaw) : Price::fromRaw(state.bestBidRaw);

      TrailingState trailing;
      trailing.activationPrice = currentPrice;

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

  // When queue simulation is enabled, LIMIT orders that do not cross the book
  // get registered in the queue tracker. Orders that cross fill immediately.
  if (accepted.type == OrderType::LIMIT && _queueTracker.enabled())
  {
    const MarketState& state = getMarketState(accepted.symbol);
    const int64_t orderPriceRaw = accepted.price.raw();
    const bool crosses =
        (accepted.side == Side::BUY && state.hasAsk && orderPriceRaw >= state.bestAskRaw) ||
        (accepted.side == Side::SELL && state.hasBid && orderPriceRaw <= state.bestBidRaw);

    if (!crosses)
    {
      Quantity levelQty = Quantity::fromRaw(0);
      if (accepted.side == Side::BUY && state.hasBid && orderPriceRaw == state.bestBidRaw)
      {
        levelQty = Quantity::fromRaw(state.bestBidQtyRaw);
      }
      else if (accepted.side == Side::SELL && state.hasAsk && orderPriceRaw == state.bestAskRaw)
      {
        levelQty = Quantity::fromRaw(state.bestAskQtyRaw);
      }
      _queueTracker.addOrder(accepted.symbol, accepted.side, accepted.price,
                             accepted.id, accepted.quantity, levelQty);
      _pending_orders.push_back(accepted);
      return;
    }
  }

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
      Order canceled = *it;
      emitEvent(OrderEventStatus::CANCELED, canceled);
      *it = _pending_orders.back();
      _pending_orders.pop_back();
      _queueTracker.removeOrder(orderId);
      _compositeLogic.onOrderCanceled(canceled);
      return;
    }
  }

  for (auto it = _conditional_orders.begin(); it != _conditional_orders.end(); ++it)
  {
    if (it->id == orderId)
    {
      Order canceled = *it;
      emitEvent(OrderEventStatus::CANCELED, canceled);

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
  size_t i = 0;
  while (i < _pending_orders.size())
  {
    if (_pending_orders[i].symbol == symbol)
    {
      emitEvent(OrderEventStatus::CANCELED, _pending_orders[i]);
      _queueTracker.removeOrder(_pending_orders[i].id);
      _pending_orders[i] = _pending_orders.back();
      _pending_orders.pop_back();
    }
    else
    {
      ++i;
    }
  }

  i = 0;
  while (i < _conditional_orders.size())
  {
    if (_conditional_orders[i].symbol == symbol)
    {
      OrderId orderId = _conditional_orders[i].id;
      emitEvent(OrderEventStatus::CANCELED, _conditional_orders[i]);

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
  _compositeLogic.registerOCO(params.order1.id, params.order2.id);
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
    state.bestBidQtyRaw = bids[0].quantity.raw();
  }
  if (state.hasAsk)
  {
    state.bestAskRaw = asks[0].price.raw();
    state.bestAskQtyRaw = asks[0].quantity.raw();
  }

  if (_queueTracker.enabled())
  {
    for (const auto& lvl : bids)
    {
      _queueTracker.onLevelUpdate(symbol, Side::BUY, lvl.price, lvl.quantity);
    }
    for (const auto& lvl : asks)
    {
      _queueTracker.onLevelUpdate(symbol, Side::SELL, lvl.price, lvl.quantity);
    }
  }

  processPendingOrders(symbol, state);
  processConditionalOrders(symbol, state);
}

void SimulatedExecutor::onTrade(SymbolId symbol, Price price, bool isBuy)
{
  onTrade(symbol, price, Quantity::fromRaw(0), isBuy);
}

void SimulatedExecutor::onTrade(SymbolId symbol, Price price, Quantity qty, bool /*isBuy*/)
{
  MarketState& state = getMarketState(symbol);
  state.lastTradeRaw = price.raw();
  state.hasTrade = true;

  if (_queueTracker.enabled() && qty.raw() > 0)
  {
    _queueFillBuffer.clear();
    _queueTracker.onTrade(symbol, price, qty, _queueFillBuffer);
    for (const auto& [orderId, fillQty] : _queueFillBuffer)
    {
      Order* ord = findPendingOrder(orderId);
      if (!ord)
      {
        continue;
      }
      executeFill(*ord, price, fillQty);
    }
    // Remove fully-filled queued orders
    _pending_orders.erase(
        std::remove_if(_pending_orders.begin(), _pending_orders.end(),
                       [](const Order& o)
                       { return o.filledQuantity.raw() >= o.quantity.raw(); }),
        _pending_orders.end());
  }

  processPendingOrders(symbol, state);
  processConditionalOrders(symbol, state);
  updateTrailingStops(symbol, price);
}

void SimulatedExecutor::onBar(SymbolId symbol, Price price)
{
  MarketState& state = getMarketState(symbol);
  const int64_t priceRaw = price.raw();
  state.bestBidRaw = priceRaw;
  state.bestAskRaw = priceRaw;
  state.lastTradeRaw = priceRaw;
  state.hasBid = true;
  state.hasAsk = true;
  state.hasTrade = true;
  processPendingOrders(symbol, state);
  processConditionalOrders(symbol, state);
  updateTrailingStops(symbol, price);
}

SimulatedExecutor::MarketState& SimulatedExecutor::getMarketState(SymbolId symbol)
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

Order* SimulatedExecutor::findPendingOrder(OrderId orderId)
{
  for (auto& order : _pending_orders)
  {
    if (order.id == orderId)
    {
      return &order;
    }
  }
  return nullptr;
}

bool SimulatedExecutor::tryFillOrder(Order& order)
{
  const MarketState& state = getMarketState(order.symbol);
  int64_t fillPriceRaw = 0;
  int64_t levelQtyRaw = 0;
  bool canFill = false;

  if (order.type == OrderType::MARKET)
  {
    if (order.side == Side::BUY && state.hasAsk)
    {
      fillPriceRaw = state.bestAskRaw;
      levelQtyRaw = state.bestAskQtyRaw;
      canFill = true;
    }
    else if (order.side == Side::SELL && state.hasBid)
    {
      fillPriceRaw = state.bestBidRaw;
      levelQtyRaw = state.bestBidQtyRaw;
      canFill = true;
    }
    else if (state.hasTrade)
    {
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
      levelQtyRaw = state.bestAskQtyRaw;
      canFill = true;
    }
    else if (order.side == Side::SELL && state.hasBid && orderPriceRaw <= state.bestBidRaw)
    {
      fillPriceRaw = state.bestBidRaw;
      levelQtyRaw = state.bestBidQtyRaw;
      canFill = true;
    }
  }

  if (!canFill)
  {
    return false;
  }

  const Quantity remainingQty =
      Quantity::fromRaw(order.quantity.raw() - order.filledQuantity.raw());
  // Apply slippage only to market-style fills (limit makers trade at posted price).
  if (order.type == OrderType::MARKET)
  {
    fillPriceRaw = applySlippage(fillPriceRaw, order.side, order.symbol,
                                 remainingQty, levelQtyRaw);
  }
  executeFill(order, Price::fromRaw(fillPriceRaw), remainingQty);
  return true;
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

    // Queue-registered limit orders are handled by the queue tracker on trades.
    if (order.type == OrderType::LIMIT && _queueTracker.enabled())
    {
      ++i;
      continue;
    }

    if (tryFillOrder(order))
    {
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

  order.filledQuantity = Quantity::fromRaw(order.filledQuantity.raw() + qty.raw());

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

  if (order.type == OrderType::STOP_MARKET || order.type == OrderType::TAKE_PROFIT_MARKET ||
      order.type == OrderType::TRAILING_STOP)
  {
    order.type = OrderType::MARKET;
  }
  else if (order.type == OrderType::STOP_LIMIT || order.type == OrderType::TAKE_PROFIT_LIMIT)
  {
    order.type = OrderType::LIMIT;
  }

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

      _conditional_orders[i] = _conditional_orders.back();
      _conditional_orders.pop_back();

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
    }
    else
    {
      ++i;
    }
  }
}

void SimulatedExecutor::drainQueueFills(SymbolId /*symbol*/)
{
  // Reserved for future heartbeat-driven draining. Queue fills currently drain
  // inside onTrade(symbol, price, qty, isBuy).
}

}  // namespace flox
