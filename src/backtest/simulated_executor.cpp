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

// FIXED_TICKS uses SlippageProfile::tickSize (raw) as the size of one tick.
// If the caller left tickSize zero, a single raw price unit is used as a
// safe fallback.

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
  _queuePosMinFraction = config.queuePositionMinChangeFraction;
  _cancelAckLatencyNs = config.cancelAckLatencyNs;
  _cancelAckJitterNs = config.cancelAckJitterNs;
  _cancelAckRng.seed(config.cancelAckSeed);
  _replaceAckLatencyNs = config.replaceAckLatencyNs;
  _replaceAckJitterNs = config.replaceAckJitterNs;
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

void SimulatedExecutor::setQueuePositionMinChangeFraction(double fraction)
{
  _queuePosMinFraction = fraction;
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
    {
      const int64_t tickRaw = (prof.tickSize.raw() > 0) ? prof.tickSize.raw() : 1;
      offsetRaw = static_cast<int64_t>(prof.ticks) * tickRaw;
      break;
    }
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

  // POST_ONLY: reject any limit that would cross the book (taker semantics
  // are not allowed for post-only). Real exchanges reject these on submit;
  // the simulator must do the same so backtests don't silently fill them
  // as makers via the queue tracker on subsequent ticks.
  if (accepted.type == OrderType::LIMIT && accepted.timeInForce == TimeInForce::POST_ONLY)
  {
    const MarketState& state = getMarketState(accepted.symbol);
    const int64_t orderPriceRaw = accepted.price.raw();
    const bool crosses =
        (accepted.side == Side::BUY && state.hasAsk && orderPriceRaw >= state.bestAskRaw) ||
        (accepted.side == Side::SELL && state.hasBid && orderPriceRaw <= state.bestBidRaw);
    if (crosses)
    {
      emitEvent(OrderEventStatus::REJECTED, accepted);
      return;
    }
  }

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
      // Seed last-emitted with the arrival ahead so the first
      // queue-position event fires only after a real movement.
      _lastEmittedQueueAheadRaw[accepted.id] = levelQty.raw();
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
  // Async path: emit PENDING_CANCEL and defer CANCELED until the
  // sampled ack deadline. Leave the order in the book so it can
  // still fill in the race window.
  if (_cancelAckLatencyNs > 0)
  {
    for (const auto& o : _pending_orders)
    {
      if (o.id == orderId)
      {
        emitEvent(OrderEventStatus::PENDING_CANCEL, o);
        enqueuePendingCancel(o);
        return;
      }
    }
    for (const auto& o : _conditional_orders)
    {
      if (o.id == orderId)
      {
        emitEvent(OrderEventStatus::PENDING_CANCEL, o);
        enqueuePendingCancel(o);
        return;
      }
    }
    return;
  }

  for (auto it = _pending_orders.begin(); it != _pending_orders.end(); ++it)
  {
    if (it->id == orderId)
    {
      Order canceled = *it;
      emitEvent(OrderEventStatus::CANCELED, canceled);
      *it = _pending_orders.back();
      _pending_orders.pop_back();
      _queueTracker.removeOrder(orderId);
      forgetQueuePosition(orderId);
      forgetTimestamps(orderId);
      forgetMarketPosition(orderId);
      forgetPendingReplace(orderId);
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
      forgetQueuePosition(_pending_orders[i].id);
      forgetTimestamps(_pending_orders[i].id);
      forgetMarketPosition(_pending_orders[i].id);
      forgetPendingReplace(_pending_orders[i].id);
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
  // Async path: emit REPLACE_SUBMITTED immediately and defer the
  // ACCEPTED + REPLACED transition until the ack deadline. The
  // original order stays in the book and can still fill in the
  // race window.
  if (_replaceAckLatencyNs > 0)
  {
    for (const auto& order : _pending_orders)
    {
      if (order.id == oldOrderId)
      {
        OrderEvent ev;
        ev.status = OrderEventStatus::REPLACE_SUBMITTED;
        ev.order = order;
        ev.newOrder = newOrder;
        ev.exchangeTsNs = _clock.nowNs();
        ev.timestamps = timestampsFor(order.id);
        if (_callback)
        {
          _callback(ev);
        }
        enqueuePendingReplace(order, newOrder);
        return;
      }
    }
    return;
  }

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
    maybeEmitQueuePositionChanges();
  }
  maybeEmitMarketPositionChanges();

  finalizePendingCancels();
  finalizePendingReplaces();
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
      // Fill came from queue consumption — our order rested and an
      // aggressive opposite trade walked into it.
      executeFill(*ord, price, fillQty, /*isMaker=*/true);
    }
    // Remove fully-filled queued orders and drop their queue
    // position + timestamp tracking entries.
    for (const auto& o : _pending_orders)
    {
      if (o.filledQuantity.raw() >= o.quantity.raw())
      {
        forgetQueuePosition(o.id);
        forgetTimestamps(o.id);
        forgetMarketPosition(o.id);
        forgetPendingReplace(o.id);
      }
    }
    _pending_orders.erase(
        std::remove_if(_pending_orders.begin(), _pending_orders.end(),
                       [](const Order& o)
                       { return o.filledQuantity.raw() >= o.quantity.raw(); }),
        _pending_orders.end());
  }

  maybeEmitQueuePositionChanges();
  maybeEmitMarketPositionChanges();

  finalizePendingCancels();
  finalizePendingReplaces();
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
  finalizePendingCancels();
  finalizePendingReplaces();
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

void SimulatedExecutor::executeFill(Order& order, Price price, Quantity qty, bool isMaker)
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
  ev.isMaker = isMaker;
  ev.exchangeTsNs = now;
  ev.status =
      (order.filledQuantity >= order.quantity) ? OrderEventStatus::FILLED : OrderEventStatus::PARTIALLY_FILLED;
  if (auto snap = _queueTracker.snapshot(order.id))
  {
    ev.queueAhead = snap->ahead;
    ev.queueTotal = snap->total;
  }
  auto& ts = timestampsFor(order.id);
  if (ts.firstFillAtNs == 0)
  {
    ts.firstFillAtNs = static_cast<int64_t>(now);
  }
  ts.lastFillAtNs = static_cast<int64_t>(now);
  ev.timestamps = ts;

  if (_callback)
  {
    _callback(ev);
  }

  if (ev.status == OrderEventStatus::FILLED)
  {
    resolveLateCancelOnFill(order);
    resolveLateReplaceOnFill(order);
    _compositeLogic.onOrderFilled(order);
  }
}

void SimulatedExecutor::emitEvent(OrderEventStatus status, const Order& order)
{
  if (!_callback)
  {
    return;
  }
  const int64_t nowNs = static_cast<int64_t>(_clock.nowNs());
  auto& ts = timestampsFor(order.id);
  switch (status)
  {
    case OrderEventStatus::SUBMITTED:
      ts.submittedAtNs = nowNs;
      break;
    case OrderEventStatus::ACCEPTED:
      ts.acceptedAtNs = nowNs;
      break;
    case OrderEventStatus::CANCELED:
      ts.canceledAtNs = nowNs;
      break;
    case OrderEventStatus::REJECTED:
      ts.rejectedAtNs = nowNs;
      break;
    case OrderEventStatus::TRIGGERED:
      ts.triggeredAtNs = nowNs;
      break;
    case OrderEventStatus::EXPIRED:
      ts.expiredAtNs = nowNs;
      break;
    default:
      break;
  }
  OrderEvent ev;
  ev.status = status;
  ev.order = order;
  ev.exchangeTsNs = nowNs;
  ev.timestamps = ts;
  _callback(ev);
}

void SimulatedExecutor::maybeEmitQueuePositionChanges()
{
  if (!_queueTracker.enabled() || !_callback || _queuePosMinFraction >= 1.0)
  {
    return;
  }
  _queueTracker.snapshotAll(_queueSnapshotBuffer);
  for (const auto& snap : _queueSnapshotBuffer)
  {
    auto it = _lastEmittedQueueAheadRaw.find(snap.orderId);
    const int64_t lastRaw = (it == _lastEmittedQueueAheadRaw.end())
                                ? snap.aheadAtArrival.raw()
                                : it->second;
    const int64_t deltaRaw =
        (snap.ahead.raw() > lastRaw) ? (snap.ahead.raw() - lastRaw) : (lastRaw - snap.ahead.raw());
    const int64_t denomRaw = (snap.aheadAtArrival.raw() > 0) ? snap.aheadAtArrival.raw() : 1;
    if (_queuePosMinFraction > 0.0 &&
        static_cast<double>(deltaRaw) <
            _queuePosMinFraction * static_cast<double>(denomRaw))
    {
      continue;
    }
    Order* ord = findPendingOrder(snap.orderId);
    if (!ord)
    {
      continue;
    }
    OrderEvent ev;
    ev.status = OrderEventStatus::QUEUE_POSITION_UPDATED;
    ev.order = *ord;
    ev.queueAhead = snap.ahead;
    ev.queueTotal = snap.total;
    ev.exchangeTsNs = _clock.nowNs();
    ev.timestamps = timestampsFor(snap.orderId);
    _callback(ev);
    _lastEmittedQueueAheadRaw[snap.orderId] = snap.ahead.raw();
  }
}

void SimulatedExecutor::forgetQueuePosition(OrderId orderId)
{
  _lastEmittedQueueAheadRaw.erase(orderId);
}

void SimulatedExecutor::forgetMarketPosition(OrderId orderId)
{
  _lastEmittedMarketPosition.erase(orderId);
}

int64_t SimulatedExecutor::sampleReplaceAckLatency()
{
  if (_replaceAckJitterNs <= 0)
  {
    return _replaceAckLatencyNs;
  }
  std::uniform_int_distribution<int64_t> dist(-_replaceAckJitterNs,
                                              _replaceAckJitterNs);
  return std::max<int64_t>(0, _replaceAckLatencyNs + dist(_cancelAckRng));
}

void SimulatedExecutor::enqueuePendingReplace(const Order& oldOrder,
                                              const Order& newOrder)
{
  const int64_t now = static_cast<int64_t>(_clock.nowNs());
  _pendingReplaces.push_back(
      PendingReplace{.orderId = oldOrder.id,
                     .ackAtNs = now + sampleReplaceAckLatency(),
                     .oldSnapshot = oldOrder,
                     .newOrder = newOrder});
}

void SimulatedExecutor::forgetPendingReplace(OrderId orderId)
{
  for (auto it = _pendingReplaces.begin(); it != _pendingReplaces.end();)
  {
    if (it->orderId == orderId)
    {
      it = _pendingReplaces.erase(it);
    }
    else
    {
      ++it;
    }
  }
}

void SimulatedExecutor::finalizePendingReplaces()
{
  if (_pendingReplaces.empty() || !_callback)
  {
    return;
  }
  const int64_t now = static_cast<int64_t>(_clock.nowNs());
  size_t i = 0;
  while (i < _pendingReplaces.size())
  {
    if (_pendingReplaces[i].ackAtNs > now)
    {
      ++i;
      continue;
    }
    const OrderId orderId = _pendingReplaces[i].orderId;
    Order oldSnapshot = _pendingReplaces[i].oldSnapshot;
    Order newOrder = _pendingReplaces[i].newOrder;
    _pendingReplaces[i] = _pendingReplaces.back();
    _pendingReplaces.pop_back();

    Order* live = findPendingOrder(orderId);
    if (!live)
    {
      // Original order is gone (typically filled mid-window); the
      // fill path already emitted REPLACE_REJECTED.
      continue;
    }

    OrderEvent accepted;
    accepted.status = OrderEventStatus::REPLACE_ACCEPTED;
    accepted.order = oldSnapshot;
    accepted.newOrder = newOrder;
    accepted.exchangeTsNs = now;
    accepted.timestamps = timestampsFor(orderId);
    _callback(accepted);

    *live = newOrder;

    OrderEvent replaced;
    replaced.status = OrderEventStatus::REPLACED;
    replaced.order = oldSnapshot;
    replaced.newOrder = newOrder;
    replaced.exchangeTsNs = now;
    replaced.timestamps = timestampsFor(orderId);
    _callback(replaced);
  }
}

void SimulatedExecutor::resolveLateReplaceOnFill(const Order& order)
{
  if (_pendingReplaces.empty() || !_callback)
  {
    return;
  }
  for (auto it = _pendingReplaces.begin(); it != _pendingReplaces.end(); ++it)
  {
    if (it->orderId != order.id)
    {
      continue;
    }
    OrderEvent ev;
    ev.status = OrderEventStatus::REPLACE_REJECTED;
    ev.order = it->oldSnapshot;
    ev.newOrder = it->newOrder;
    ev.rejectReason = "late_replace_after_fill";
    ev.exchangeTsNs = _clock.nowNs();
    ev.timestamps = timestampsFor(order.id);
    _callback(ev);
    *it = _pendingReplaces.back();
    _pendingReplaces.pop_back();
    return;
  }
}

MarketPosition SimulatedExecutor::computeMarketPosition(
    const Order& order, const MarketState& state, Quantity ourRemaining,
    Quantity queueTotal) const
{
  const int64_t p = order.price.raw();
  const bool isBuy = (order.side == Side::BUY);
  const bool hasBid = state.hasBid;
  const bool hasAsk = state.hasAsk;
  const int64_t bid = state.bestBidRaw;
  const int64_t ask = state.bestAskRaw;

  if (isBuy)
  {
    if (hasAsk && p >= ask)
    {
      return MarketPosition::Crossed;
    }
    if (hasBid && p == bid)
    {
      if (queueTotal.raw() > 0 && queueTotal.raw() <= ourRemaining.raw())
      {
        return MarketPosition::LevelEmpty;
      }
      return MarketPosition::Best;
    }
    if (hasBid && p < bid)
    {
      return MarketPosition::BehindBest;
    }
    // p > bid (or no bid) and p < ask → mid-spread.
    if (hasAsk && (!hasBid || p > bid) && p < ask)
    {
      return MarketPosition::MidSpread;
    }
    return MarketPosition::Unknown;
  }
  // SELL side.
  if (hasBid && p <= bid)
  {
    return MarketPosition::Crossed;
  }
  if (hasAsk && p == ask)
  {
    if (queueTotal.raw() > 0 && queueTotal.raw() <= ourRemaining.raw())
    {
      return MarketPosition::LevelEmpty;
    }
    return MarketPosition::Best;
  }
  if (hasAsk && p > ask)
  {
    return MarketPosition::BehindBest;
  }
  if (hasBid && (!hasAsk || p < ask) && p > bid)
  {
    return MarketPosition::MidSpread;
  }
  return MarketPosition::Unknown;
}

int32_t SimulatedExecutor::computeDistanceToBestTicks(
    const Order& order, const MarketState& state) const
{
  // One raw price unit per "tick" is the safe fallback if the engine
  // has no symbol tick-size handy. Strategies that care about this
  // value typically know their own tick size.
  const int64_t p = order.price.raw();
  if (order.side == Side::BUY)
  {
    if (!state.hasBid)
    {
      return 0;
    }
    return static_cast<int32_t>(state.bestBidRaw - p);  // positive = behind
  }
  if (!state.hasAsk)
  {
    return 0;
  }
  return static_cast<int32_t>(p - state.bestAskRaw);
}

void SimulatedExecutor::maybeEmitMarketPositionChanges()
{
  if (!_callback || _pending_orders.empty())
  {
    return;
  }
  for (const Order& o : _pending_orders)
  {
    if (o.type != OrderType::LIMIT)
    {
      continue;
    }
    const MarketState& state = getMarketState(o.symbol);
    Quantity queueTotal{0};
    if (auto snap = _queueTracker.snapshot(o.id))
    {
      queueTotal = snap->total;
    }
    const Quantity remaining =
        Quantity::fromRaw(o.quantity.raw() - o.filledQuantity.raw());
    const MarketPosition pos = computeMarketPosition(o, state, remaining, queueTotal);
    const int32_t dist = computeDistanceToBestTicks(o, state);

    auto it = _lastEmittedMarketPosition.find(o.id);
    if (it != _lastEmittedMarketPosition.end() && it->second == pos)
    {
      continue;
    }
    _lastEmittedMarketPosition[o.id] = pos;

    OrderEvent ev;
    ev.status = OrderEventStatus::MARKET_POSITION_CHANGED;
    ev.order = o;
    ev.marketPosition = pos;
    ev.distanceToBestTicks = dist;
    ev.exchangeTsNs = _clock.nowNs();
    ev.timestamps = timestampsFor(o.id);
    _callback(ev);
  }
}

int64_t SimulatedExecutor::sampleCancelAckLatency()
{
  if (_cancelAckJitterNs <= 0)
  {
    return _cancelAckLatencyNs;
  }
  std::uniform_int_distribution<int64_t> dist(-_cancelAckJitterNs,
                                              _cancelAckJitterNs);
  return std::max<int64_t>(0, _cancelAckLatencyNs + dist(_cancelAckRng));
}

void SimulatedExecutor::enqueuePendingCancel(const Order& order)
{
  const int64_t now = static_cast<int64_t>(_clock.nowNs());
  _pendingCancels.push_back(
      PendingCancel{.orderId = order.id,
                    .ackAtNs = now + sampleCancelAckLatency(),
                    .orderSnapshot = order});
}

void SimulatedExecutor::finalizePendingCancels()
{
  if (_pendingCancels.empty())
  {
    return;
  }
  const int64_t now = static_cast<int64_t>(_clock.nowNs());
  size_t i = 0;
  while (i < _pendingCancels.size())
  {
    if (_pendingCancels[i].ackAtNs > now)
    {
      ++i;
      continue;
    }
    const OrderId orderId = _pendingCancels[i].orderId;
    Order* live = findPendingOrder(orderId);
    Order snapshot = _pendingCancels[i].orderSnapshot;
    _pendingCancels[i] = _pendingCancels.back();
    _pendingCancels.pop_back();

    if (live)
    {
      Order canceled = *live;
      emitEvent(OrderEventStatus::CANCELED, canceled);
      for (auto it = _pending_orders.begin(); it != _pending_orders.end(); ++it)
      {
        if (it->id == orderId)
        {
          *it = _pending_orders.back();
          _pending_orders.pop_back();
          break;
        }
      }
      _queueTracker.removeOrder(orderId);
      forgetQueuePosition(orderId);
      forgetTimestamps(orderId);
      forgetMarketPosition(orderId);
      forgetPendingReplace(orderId);
      _compositeLogic.onOrderCanceled(canceled);
      continue;
    }
    // Order not in book any more — typically it filled mid-window.
    // The fill path already emitted REJECTED on the cancel attempt.
    // Conditional orders cancel synchronously below if still present.
    for (auto it = _conditional_orders.begin(); it != _conditional_orders.end();
         ++it)
    {
      if (it->id == orderId)
      {
        Order canceled = *it;
        emitEvent(OrderEventStatus::CANCELED, canceled);
        *it = _conditional_orders.back();
        _conditional_orders.pop_back();
        _compositeLogic.onOrderCanceled(canceled);
        break;
      }
    }
  }
}

void SimulatedExecutor::resolveLateCancelOnFill(const Order& order)
{
  if (_pendingCancels.empty() || !_callback)
  {
    return;
  }
  for (auto it = _pendingCancels.begin(); it != _pendingCancels.end(); ++it)
  {
    if (it->orderId != order.id)
    {
      continue;
    }
    OrderEvent ev;
    ev.status = OrderEventStatus::REJECTED;
    ev.order = order;
    ev.rejectReason = "late_cancel_after_fill";
    ev.exchangeTsNs = _clock.nowNs();
    ev.timestamps = timestampsFor(order.id);
    _callback(ev);
    *it = _pendingCancels.back();
    _pendingCancels.pop_back();
    return;
  }
}

OrderTimestamps& SimulatedExecutor::timestampsFor(OrderId id)
{
  return _orderTimestamps[id];
}

void SimulatedExecutor::forgetTimestamps(OrderId id)
{
  _orderTimestamps.erase(id);
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
