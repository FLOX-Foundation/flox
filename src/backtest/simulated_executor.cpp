/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/simulated_executor.h"

#include "flox/backtest/latency_profiles.h"

#include <algorithm>
#include <cmath>
#include <string>

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
  setCancelAckLatency(config.cancelAckLatencyNs, config.cancelAckJitterNs);
  _cancelAckRng.seed(config.cancelAckSeed);
  setReplaceAckLatency(config.replaceAckLatencyNs, config.replaceAckJitterNs);
  setSubmitAckLatency(config.submitAckLatencyNs, config.submitAckJitterNs);
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

void SimulatedExecutor::setQueueFifoTopN(size_t topN)
{
  _queueTracker.setFifoTopN(topN);
}

void SimulatedExecutor::setQueuePositionMinChangeFraction(double fraction)
{
  _queuePosMinFraction = fraction;
}

static LatencyDistribution makeLegacyDist(int64_t latencyNs, int64_t jitterNs)
{
  if (jitterNs <= 0)
  {
    return LatencyDistribution::constant(latencyNs);
  }
  return LatencyDistribution::uniform(std::max<int64_t>(0, latencyNs - jitterNs),
                                      latencyNs + jitterNs);
}

void SimulatedExecutor::setSubmitAckLatency(int64_t latencyNs, int64_t jitterNs)
{
  _submitAckDist = makeLegacyDist(latencyNs, jitterNs);
}

void SimulatedExecutor::setCancelAckLatency(int64_t latencyNs, int64_t jitterNs)
{
  _cancelAckDist = makeLegacyDist(latencyNs, jitterNs);
}

void SimulatedExecutor::setReplaceAckLatency(int64_t latencyNs, int64_t jitterNs)
{
  _replaceAckDist = makeLegacyDist(latencyNs, jitterNs);
}

void SimulatedExecutor::setSubmitAckLatencyDistribution(const LatencyDistribution& dist)
{
  _submitAckDist = dist;
}

void SimulatedExecutor::setCancelAckLatencyDistribution(const LatencyDistribution& dist)
{
  _cancelAckDist = dist;
}

void SimulatedExecutor::setReplaceAckLatencyDistribution(const LatencyDistribution& dist)
{
  _replaceAckDist = dist;
}

void SimulatedExecutor::applyLatencyProfile(const char* name)
{
  if (name == nullptr)
  {
    return;
  }
  std::string n(name);
  BacktestConfig cfg{};
  if (n == "binance_um_futures")
  {
    LatencyProfiles::binance_um_futures(cfg);
  }
  else if (n == "bybit_linear")
  {
    LatencyProfiles::bybit_linear(cfg);
  }
  else if (n == "okx_swap")
  {
    LatencyProfiles::okx_swap(cfg);
  }
  else if (n == "deribit")
  {
    LatencyProfiles::deribit(cfg);
  }
  else if (n == "idealized")
  {
    LatencyProfiles::idealized(cfg);
  }
  else if (n == "adversarial")
  {
    LatencyProfiles::adversarial(cfg);
  }
  else
  {
    return;
  }
  setSubmitAckLatency(cfg.submitAckLatencyNs, cfg.submitAckJitterNs);
  setCancelAckLatency(cfg.cancelAckLatencyNs, cfg.cancelAckJitterNs);
  setReplaceAckLatency(cfg.replaceAckLatencyNs, cfg.replaceAckJitterNs);
}

void SimulatedExecutor::setRateLimitPolicy(const RateLimitPolicy& policy)
{
  _rateLimit = policy;
  _hasRateLimit = _rateLimit.bucketCount() > 0;
}

void SimulatedExecutor::clearRateLimitPolicy()
{
  _rateLimit = RateLimitPolicy{};
  _hasRateLimit = false;
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

  if (_venue && !_venue->isUp(static_cast<int64_t>(_clock.nowNs())))
  {
    _outageBuffer.push_back(BufferedRequest{.action = BufferedAction::SUBMIT,
                                            .order = accepted});
    return;
  }

  if (_hasRateLimit &&
      !_rateLimit.tryConsume(RateLimitPolicy::ActionKind::Submit,
                             static_cast<int64_t>(_clock.nowNs())))
  {
    emitEvent(OrderEventStatus::REJECTED_RATE_LIMIT, accepted);
    return;
  }

  // Self-trade prevention. Walk pending orders for an opposite-side
  // crossing match on the same symbol; apply the configured mode.
  // Empty / None mode is a no-op.
  if (_stpMode != STPMode::None && !_pending_orders.empty())
  {
    for (size_t i = 0; i < _pending_orders.size(); ++i)
    {
      Order& existing = _pending_orders[i];
      if (existing.symbol != accepted.symbol || existing.side == accepted.side)
      {
        continue;
      }
      const int64_t aPrice = accepted.price.raw();
      const int64_t ePrice = existing.price.raw();
      const bool crosses =
          (accepted.side == Side::BUY && aPrice >= ePrice) ||
          (accepted.side == Side::SELL && aPrice <= ePrice);
      if (!crosses)
      {
        continue;
      }

      auto rejectAccepted = [&](const char* reason)
      {
        OrderEvent ev;
        ev.status = OrderEventStatus::REJECTED;
        ev.order = accepted;
        ev.exchangeTsNs = _clock.nowNs();
        ev.timestamps = timestampsFor(accepted.id);
        ev.timestamps.rejectedAtNs = ev.exchangeTsNs;
        ev.rejectReason = reason;
        if (_callback)
        {
          _callback(ev);
        }
      };

      auto cancelExisting = [&]()
      {
        Order cancelled = existing;
        _queueTracker.removeOrder(cancelled.id);
        forgetQueuePosition(cancelled.id);
        forgetMarketPosition(cancelled.id);
        _pending_orders.erase(_pending_orders.begin() + static_cast<long>(i));
        emitEvent(OrderEventStatus::CANCELED, cancelled);
      };

      switch (_stpMode)
      {
        case STPMode::CancelNewest:
          rejectAccepted("stp_cancel_newest");
          return;
        case STPMode::CancelOldest:
          cancelExisting();
          // Fall through to normal submit path for the accepted order.
          goto stp_done;
        case STPMode::CancelBoth:
          cancelExisting();
          rejectAccepted("stp_cancel_both");
          return;
        case STPMode::Decrement:
        {
          const int64_t aQty = accepted.quantity.raw();
          const int64_t eQty = existing.quantity.raw() - existing.filledQuantity.raw();
          if (aQty <= eQty)
          {
            // Smaller side is the new order. Cancel it; shrink existing by aQty.
            existing.quantity = Quantity::fromRaw(existing.quantity.raw() - aQty);
            rejectAccepted("stp_decrement_newest");
            return;
          }
          else
          {
            // Smaller side is the existing order. Cancel it; shrink new by eQty.
            cancelExisting();
            accepted.quantity = Quantity::fromRaw(aQty - eQty);
            goto stp_done;
          }
        }
        case STPMode::None:
          break;
      }
    }
  }
stp_done:

  // reduce_only: order may only reduce existing position. Computed
  // against the simulator-side net position (updated in executeFill).
  // If the order would open or grow the position it is rejected; if
  // it would overshoot zero, the size is truncated to flat.
  if (accepted.flags.reduceOnly)
  {
    const int64_t netRaw = netPositionRaw(accepted.symbol);
    const int64_t qtyRaw = accepted.quantity.raw();
    const int64_t signedQtyRaw = (accepted.side == Side::BUY) ? qtyRaw : -qtyRaw;

    // No open position → reduce-only can't reduce anything.
    if (netRaw == 0 || (netRaw > 0 && signedQtyRaw > 0) ||
        (netRaw < 0 && signedQtyRaw < 0))
    {
      OrderEvent ev;
      ev.status = OrderEventStatus::REJECTED;
      ev.order = accepted;
      ev.exchangeTsNs = _clock.nowNs();
      ev.timestamps = timestampsFor(accepted.id);
      ev.timestamps.rejectedAtNs = ev.exchangeTsNs;
      ev.rejectReason = "reduce_only";
      if (_callback)
      {
        _callback(ev);
      }
      return;
    }

    // Truncate so we don't flip sign.
    const int64_t maxReductionRaw = (netRaw > 0) ? netRaw : -netRaw;
    if (qtyRaw > maxReductionRaw)
    {
      accepted.quantity = Quantity::fromRaw(maxReductionRaw);
    }
  }

  emitEvent(OrderEventStatus::SUBMITTED, accepted);

  if (_submitAckDist.medianNs() > 0)
  {
    // Async path: ACCEPTED defers until the sampled deadline. The
    // order is held aside; finishSubmission runs the existing
    // book-add / queue-tracker / try-fill logic when ack arrives.
    enqueuePendingSubmission(accepted);
    return;
  }

  finishSubmission(std::move(accepted), /*fromAck=*/false);
}

void SimulatedExecutor::finishSubmission(Order accepted, bool fromAck)
{
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
      if (_callback)
      {
        OrderEvent ev;
        ev.status = OrderEventStatus::REJECTED;
        ev.order = accepted;
        ev.exchangeTsNs = _clock.nowNs();
        ev.timestamps = timestampsFor(accepted.id);
        ev.timestamps.rejectedAtNs = ev.exchangeTsNs;
        if (fromAck)
        {
          ev.rejectReason = "late_post_only_crossed";
        }
        _callback(ev);
      }
      return;
    }
  }

  // FOK: must fully fill or reject. Available crossing liquidity is
  // approximated by the best-level qty (TOB). The simulator does not
  // walk multi-level liquidity for a single submit, so this is a
  // best-effort check; deeper-walk FOK is on the spec follow-up list.
  if (accepted.type == OrderType::LIMIT &&
      accepted.timeInForce == TimeInForce::FOK)
  {
    const MarketState& state = getMarketState(accepted.symbol);
    int64_t availRaw = 0;
    bool canCross = false;
    if (accepted.side == Side::BUY && state.hasAsk &&
        accepted.price.raw() >= state.bestAskRaw)
    {
      availRaw = state.bestAskQtyRaw;
      canCross = true;
    }
    else if (accepted.side == Side::SELL && state.hasBid &&
             accepted.price.raw() <= state.bestBidRaw)
    {
      availRaw = state.bestBidQtyRaw;
      canCross = true;
    }
    if (!canCross || availRaw < accepted.quantity.raw())
    {
      OrderEvent ev;
      ev.status = OrderEventStatus::REJECTED;
      ev.order = accepted;
      ev.exchangeTsNs = _clock.nowNs();
      ev.timestamps = timestampsFor(accepted.id);
      ev.timestamps.rejectedAtNs = ev.exchangeTsNs;
      ev.rejectReason = "fok_not_fillable";
      if (_callback)
      {
        _callback(ev);
      }
      return;
    }
    // Fall through to tryFillOrder which will take the crossing
    // liquidity at the limit price (or better).
  }

  // IOC: take whatever crosses now; cancel any remainder. We tag it
  // so the post-fill path skips resting the order.
  bool isIOC = (accepted.type == OrderType::LIMIT &&
                accepted.timeInForce == TimeInForce::IOC);

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
      if (isIOC)
      {
        // IOC limit that does not cross — nothing to fill now and
        // nothing to rest. Cancel immediately.
        emitEvent(OrderEventStatus::CANCELED, accepted);
        return;
      }
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

  const bool filled = tryFillOrder(accepted);
  if (!filled && isIOC)
  {
    // IOC: cancel the unfilled remainder instead of resting. The
    // CANCELED event carries the IOC tag on the order itself; no
    // free-text reason field on cancel today (rejectReason would
    // be misleading).
    emitEvent(OrderEventStatus::CANCELED, accepted);
    return;
  }
  if (!filled)
  {
    _pending_orders.push_back(accepted);
  }
}

void SimulatedExecutor::cancelOrder(OrderId orderId)
{
  if (_venue && !_venue->isUp(static_cast<int64_t>(_clock.nowNs())))
  {
    _outageBuffer.push_back(BufferedRequest{.action = BufferedAction::CANCEL,
                                            .oldOrderId = orderId});
    return;
  }
  if (_hasRateLimit &&
      !_rateLimit.tryConsume(RateLimitPolicy::ActionKind::Cancel,
                             static_cast<int64_t>(_clock.nowNs())))
  {
    for (const auto& o : _pending_orders)
    {
      if (o.id == orderId)
      {
        emitEvent(OrderEventStatus::REJECTED_RATE_LIMIT, o);
        return;
      }
    }
    return;
  }

  // Async path: emit PENDING_CANCEL and defer CANCELED until the
  // sampled ack deadline. Leave the order in the book so it can
  // still fill in the race window.
  if (_cancelAckDist.medianNs() > 0)
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
  if (_venue && !_venue->isUp(static_cast<int64_t>(_clock.nowNs())))
  {
    _outageBuffer.push_back(BufferedRequest{.action = BufferedAction::CANCEL_ALL_SYMBOL,
                                            .cancelAllSymbol = symbol});
    return;
  }
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
  if (_venue && !_venue->isUp(static_cast<int64_t>(_clock.nowNs())))
  {
    _outageBuffer.push_back(BufferedRequest{.action = BufferedAction::REPLACE,
                                            .order = newOrder,
                                            .oldOrderId = oldOrderId});
    return;
  }
  if (_hasRateLimit &&
      !_rateLimit.tryConsume(RateLimitPolicy::ActionKind::Replace,
                             static_cast<int64_t>(_clock.nowNs())))
  {
    for (const auto& o : _pending_orders)
    {
      if (o.id == oldOrderId)
      {
        emitEvent(OrderEventStatus::REJECTED_RATE_LIMIT, o);
        return;
      }
    }
    return;
  }

  // Async path: emit REPLACE_SUBMITTED immediately and defer the
  // ACCEPTED + REPLACED transition until the ack deadline. The
  // original order stays in the book and can still fill in the
  // race window.
  if (_replaceAckDist.medianNs() > 0)
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

namespace
{
Order legToOrder(const BracketLeg& leg, OrderId id, SymbolId symbol, Quantity qty)
{
  Order o;
  o.id = id;
  o.side = leg.side;
  o.type = leg.type;
  o.price = leg.price;
  o.triggerPrice = leg.triggerPrice;
  o.quantity = qty;
  o.symbol = symbol;
  return o;
}
}  // namespace

void SimulatedExecutor::submitBracket(const BracketOrder& bracket)
{
  const OrderId entryId = bracket.bracketId * 3 + 0;
  const OrderId tpId = bracket.bracketId * 3 + 1;
  const OrderId stopId = bracket.bracketId * 3 + 2;

  BracketStatus st;
  st.bracketId = bracket.bracketId;
  st.state = BracketState::PENDING_ENTRY;
  st.entryOrderId = entryId;
  st.tpOrderId = tpId;
  st.stopOrderId = stopId;
  _brackets[bracket.bracketId] = st;
  _bracketTemplates[bracket.bracketId] = bracket;
  _legToBracket[entryId] = bracket.bracketId;

  submitOrder(legToOrder(bracket.entry, entryId, bracket.symbol,
                         bracket.entry.quantity));
}

void SimulatedExecutor::cancelBracket(uint64_t bracketId)
{
  auto it = _brackets.find(bracketId);
  if (it == _brackets.end())
  {
    return;
  }
  const auto& st = it->second;
  // Cancel every leg that is still resting in the book.
  if (st.state == BracketState::PENDING_ENTRY)
  {
    cancelOrder(st.entryOrderId);
  }
  if (st.state == BracketState::ENTRY_FILLED)
  {
    cancelOrder(st.tpOrderId);
    cancelOrder(st.stopOrderId);
  }
  // ENTRY_FILLED → TP_FILLED/STOP_FILLED transitions already cancel the sibling.
  it->second.state = BracketState::CANCELED;
  _legToBracket.erase(st.entryOrderId);
  _legToBracket.erase(st.tpOrderId);
  _legToBracket.erase(st.stopOrderId);
  _bracketTemplates.erase(bracketId);
}

BracketStatus SimulatedExecutor::bracketStatus(uint64_t bracketId) const
{
  auto it = _brackets.find(bracketId);
  return it == _brackets.end() ? BracketStatus{} : it->second;
}

void SimulatedExecutor::onBracketFillEvent(const Order& filledOrder)
{
  auto legIt = _legToBracket.find(filledOrder.id);
  if (legIt == _legToBracket.end())
  {
    return;
  }
  const uint64_t bracketId = legIt->second;
  auto stIt = _brackets.find(bracketId);
  auto tplIt = _bracketTemplates.find(bracketId);
  if (stIt == _brackets.end() || tplIt == _bracketTemplates.end())
  {
    return;
  }
  auto& st = stIt->second;
  const BracketOrder& tpl = tplIt->second;

  if (filledOrder.id == st.entryOrderId &&
      st.state == BracketState::PENDING_ENTRY &&
      filledOrder.filledQuantity.raw() >= filledOrder.quantity.raw())
  {
    // Entry fully filled: submit TP + stop scaled to the actual
    // entry fill (matches partial-fill semantics in real venues).
    st.entryFilled = filledOrder.filledQuantity;
    st.state = BracketState::ENTRY_FILLED;
    _legToBracket[st.tpOrderId] = bracketId;
    _legToBracket[st.stopOrderId] = bracketId;
    submitOrder(legToOrder(tpl.takeProfit, st.tpOrderId, tpl.symbol,
                           st.entryFilled));
    submitOrder(legToOrder(tpl.stop, st.stopOrderId, tpl.symbol,
                           st.entryFilled));
    return;
  }

  if (filledOrder.id == st.tpOrderId && st.state == BracketState::ENTRY_FILLED)
  {
    st.state = BracketState::TP_FILLED;
    cancelOrder(st.stopOrderId);
    return;
  }
  if (filledOrder.id == st.stopOrderId && st.state == BracketState::ENTRY_FILLED)
  {
    st.state = BracketState::STOP_FILLED;
    cancelOrder(st.tpOrderId);
    return;
  }
}

void SimulatedExecutor::onBookUpdate(SymbolId symbol, const std::pmr::vector<BookLevel>& bids,
                                     const std::pmr::vector<BookLevel>& asks)
{
  if (_venue)
  {
    const int64_t now = static_cast<int64_t>(_clock.nowNs());
    const bool up = _venue->isUp(now);
    if (!up && _venueWasUp)
    {
      applyOutagePolicy();
      _venueWasUp = false;
    }
    else if (up && !_venueWasUp)
    {
      _venueWasUp = true;
      flushOutageBuffer();
    }
    if (!up)
    {
      return;  // feed gap
    }
  }
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
    // First update every level present in the snapshot.
    for (const auto& lvl : bids)
    {
      _queueTracker.onLevelUpdate(symbol, Side::BUY, lvl.price, lvl.quantity);
    }
    for (const auto& lvl : asks)
    {
      _queueTracker.onLevelUpdate(symbol, Side::SELL, lvl.price, lvl.quantity);
    }
    // Then zero-out any tracked level that disappeared from the snapshot.
    // Book providers usually drop empty levels rather than reporting qty=0;
    // without this pass, our queue tracker would keep stale `level.totalQty`
    // values for those levels and LevelEmpty would never fire.
    std::vector<Price> trackedBids;
    _queueTracker.trackedPrices(symbol, Side::BUY, trackedBids);
    for (const Price& p : trackedBids)
    {
      bool present = false;
      for (const auto& lvl : bids)
      {
        if (lvl.price.raw() == p.raw())
        {
          present = true;
          break;
        }
      }
      if (!present)
      {
        _queueTracker.onLevelUpdate(symbol, Side::BUY, p, Quantity::fromRaw(0));
      }
    }
    std::vector<Price> trackedAsks;
    _queueTracker.trackedPrices(symbol, Side::SELL, trackedAsks);
    for (const Price& p : trackedAsks)
    {
      bool present = false;
      for (const auto& lvl : asks)
      {
        if (lvl.price.raw() == p.raw())
        {
          present = true;
          break;
        }
      }
      if (!present)
      {
        _queueTracker.onLevelUpdate(symbol, Side::SELL, p, Quantity::fromRaw(0));
      }
    }
    maybeEmitQueuePositionChanges();
  }
  maybeEmitMarketPositionChanges();

  finalizePendingSubmissions();
  finalizePendingCancels();
  finalizePendingReplaces();
  processExpiredOrders();
  processPendingOrders(symbol, state);
  processConditionalOrders(symbol, state);
}

void SimulatedExecutor::onTrade(SymbolId symbol, Price price, bool isBuy)
{
  onTrade(symbol, price, Quantity::fromRaw(0), isBuy);
}

void SimulatedExecutor::onTrade(SymbolId symbol, Price price, Quantity qty, bool /*isBuy*/)
{
  if (_venue)
  {
    const int64_t now = static_cast<int64_t>(_clock.nowNs());
    const bool up = _venue->isUp(now);
    if (!up && _venueWasUp)
    {
      applyOutagePolicy();
      _venueWasUp = false;
    }
    else if (up && !_venueWasUp)
    {
      _venueWasUp = true;
      flushOutageBuffer();
    }
    if (!up)
    {
      return;
    }
  }
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

  finalizePendingSubmissions();
  finalizePendingCancels();
  finalizePendingReplaces();
  processExpiredOrders();
  processPendingOrders(symbol, state);
  processConditionalOrders(symbol, state);
  updateTrailingStops(symbol, price);
}

void SimulatedExecutor::onBar(SymbolId symbol, Price price)
{
  if (_venue)
  {
    const int64_t now = static_cast<int64_t>(_clock.nowNs());
    const bool up = _venue->isUp(now);
    if (!up && _venueWasUp)
    {
      applyOutagePolicy();
      _venueWasUp = false;
    }
    else if (up && !_venueWasUp)
    {
      _venueWasUp = true;
      flushOutageBuffer();
    }
    if (!up)
    {
      return;
    }
  }
  MarketState& state = getMarketState(symbol);
  const int64_t priceRaw = price.raw();
  state.bestBidRaw = priceRaw;
  state.bestAskRaw = priceRaw;
  state.lastTradeRaw = priceRaw;
  state.hasBid = true;
  state.hasAsk = true;
  state.hasTrade = true;
  finalizePendingSubmissions();
  finalizePendingCancels();
  finalizePendingReplaces();
  processExpiredOrders();
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

  // Update simulator-side net position for reduce_only enforcement.
  const int64_t signedRaw = (order.side == Side::BUY) ? qty.raw() : -qty.raw();
  _netPositionRaw[order.symbol] += signedRaw;

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
    onBracketFillEvent(order);
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
  return _replaceAckDist.sample(_cancelAckRng);
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

int64_t SimulatedExecutor::sampleSubmitAckLatency()
{
  return _submitAckDist.sample(_cancelAckRng);
}

void SimulatedExecutor::enqueuePendingSubmission(const Order& order)
{
  const int64_t now = static_cast<int64_t>(_clock.nowNs());
  _pendingSubmissions.push_back(
      PendingSubmission{.ackAtNs = now + sampleSubmitAckLatency(), .order = order});
}

void SimulatedExecutor::finalizePendingSubmissions()
{
  if (_pendingSubmissions.empty())
  {
    return;
  }
  const int64_t now = static_cast<int64_t>(_clock.nowNs());
  size_t i = 0;
  while (i < _pendingSubmissions.size())
  {
    if (_pendingSubmissions[i].ackAtNs > now)
    {
      ++i;
      continue;
    }
    Order accepted = _pendingSubmissions[i].order;
    _pendingSubmissions[i] = _pendingSubmissions.back();
    _pendingSubmissions.pop_back();
    finishSubmission(std::move(accepted), /*fromAck=*/true);
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
    const Order& order, const MarketState& state, Quantity /*ourRemaining*/,
    Quantity nonOurLevelQty) const
{
  const int64_t p = order.price.raw();
  const bool isBuy = (order.side == Side::BUY);
  const bool hasBid = state.hasBid;
  const bool hasAsk = state.hasAsk;
  const int64_t bid = state.bestBidRaw;
  const int64_t ask = state.bestAskRaw;
  // nonOurLevelQty is the queue tracker's view of "others' quantity at this
  // order's price level". Meaningful only when the queue tracker is enabled
  // and our order is registered with it. Otherwise it is zero and cannot
  // distinguish "no others at level" from "no tracker info".
  const bool queueInfo = _queueTracker.enabled();

  if (isBuy)
  {
    if (hasAsk && p >= ask)
    {
      return MarketPosition::Crossed;
    }
    // No bid level on the book: we are the only resting buy at our
    // price (or a worse one). If the queue tracker confirms no
    // others share our exact level, we are LevelEmpty.
    if (!hasBid)
    {
      if (queueInfo && nonOurLevelQty.raw() == 0)
      {
        return MarketPosition::LevelEmpty;
      }
      if (hasAsk && p < ask)
      {
        return MarketPosition::MidSpread;
      }
      return MarketPosition::Unknown;
    }
    if (p == bid)
    {
      if (queueInfo && nonOurLevelQty.raw() == 0)
      {
        return MarketPosition::LevelEmpty;
      }
      return MarketPosition::Best;
    }
    if (p < bid)
    {
      return MarketPosition::BehindBest;
    }
    // p > bid and p < ask → mid-spread (our order is bid-side but
    // above the current best bid yet below best ask — improving the
    // book without crossing).
    if (hasAsk && p < ask)
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
  if (!hasAsk)
  {
    if (queueInfo && nonOurLevelQty.raw() == 0)
    {
      return MarketPosition::LevelEmpty;
    }
    if (hasBid && p > bid)
    {
      return MarketPosition::MidSpread;
    }
    return MarketPosition::Unknown;
  }
  if (p == ask)
  {
    if (queueInfo && nonOurLevelQty.raw() == 0)
    {
      return MarketPosition::LevelEmpty;
    }
    return MarketPosition::Best;
  }
  if (p > ask)
  {
    return MarketPosition::BehindBest;
  }
  if (hasBid && p > bid)
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
  return _cancelAckDist.sample(_cancelAckRng);
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

int64_t SimulatedExecutor::netPositionRaw(SymbolId symbol) const
{
  auto it = _netPositionRaw.find(symbol);
  return it == _netPositionRaw.end() ? 0 : it->second;
}

void SimulatedExecutor::processExpiredOrders()
{
  const int64_t nowNs = static_cast<int64_t>(_clock.nowNs());
  for (auto it = _pending_orders.begin(); it != _pending_orders.end();)
  {
    if (it->timeInForce == TimeInForce::GTD && it->expiresAfter.has_value())
    {
      // expiresAfter is a TimePoint (nanoseconds since epoch). Compare
      // against the simulator clock; expired orders are dropped with
      // EXPIRED, removed from the book, and forgotten from trackers.
      const int64_t expiresNs = it->expiresAfter->time_since_epoch().count();
      if (nowNs >= expiresNs)
      {
        Order expired = *it;
        _queueTracker.removeOrder(expired.id);
        forgetQueuePosition(expired.id);
        forgetMarketPosition(expired.id);
        emitEvent(OrderEventStatus::EXPIRED, expired);
        it = _pending_orders.erase(it);
        continue;
      }
    }
    ++it;
  }
}

void SimulatedExecutor::applyOutagePolicy()
{
  if (!_venue)
  {
    return;
  }
  const int64_t now = static_cast<int64_t>(_clock.nowNs());
  const auto* outage = _venue->activeOutage(now);
  if (!outage)
  {
    return;
  }
  switch (outage->policy)
  {
    case OnOutage::HOLD:
      // Orders stay; nothing to do.
      break;
    case OnOutage::CANCEL_ALL:
    {
      // Cancel every resting order at outage start.
      for (auto& o : _pending_orders)
      {
        emitEvent(OrderEventStatus::CANCELED, o);
        _queueTracker.removeOrder(o.id);
        forgetQueuePosition(o.id);
        forgetTimestamps(o.id);
        forgetMarketPosition(o.id);
        forgetPendingReplace(o.id);
      }
      _pending_orders.clear();
      break;
    }
    case OnOutage::EXPIRE_GTC_AFTER:
    {
      // Drop orders whose age in flight exceeds the venue TTL.
      const int64_t ttlNs = outage->gtcTtlNs;
      if (ttlNs <= 0)
      {
        return;
      }
      size_t i = 0;
      while (i < _pending_orders.size())
      {
        auto& o = _pending_orders[i];
        const int64_t createdNs = o.createdAt.time_since_epoch().count();
        const int64_t ageNs = now - createdNs;
        if (ageNs >= ttlNs)
        {
          emitEvent(OrderEventStatus::CANCELED, o);
          _queueTracker.removeOrder(o.id);
          forgetQueuePosition(o.id);
          forgetTimestamps(o.id);
          forgetMarketPosition(o.id);
          forgetPendingReplace(o.id);
          _pending_orders[i] = _pending_orders.back();
          _pending_orders.pop_back();
          continue;
        }
        ++i;
      }
      break;
    }
  }
}

void SimulatedExecutor::flushOutageBuffer()
{
  if (_outageBuffer.empty())
  {
    return;
  }
  std::vector<BufferedRequest> drained;
  drained.swap(_outageBuffer);
  for (const auto& req : drained)
  {
    switch (req.action)
    {
      case BufferedAction::SUBMIT:
        submitOrder(req.order);
        break;
      case BufferedAction::CANCEL:
        cancelOrder(req.oldOrderId);
        break;
      case BufferedAction::REPLACE:
        replaceOrder(req.oldOrderId, req.order);
        break;
      case BufferedAction::CANCEL_ALL_SYMBOL:
        cancelAllOrders(req.cancelAllSymbol);
        break;
    }
  }
}

}  // namespace flox
