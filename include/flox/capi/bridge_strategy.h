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
#include "flox/capi/flox_capi.h"
#include "flox/strategy/strategy.h"

#include <atomic>
#include <memory>

namespace flox
{

class BridgeStrategy : public Strategy
{
 public:
  BridgeStrategy(SubscriberId id, std::vector<SymbolId> symbols, const SymbolRegistry& registry,
                 FloxStrategyCallbacks callbacks)
      : Strategy(id, std::move(symbols), registry),
        _cb(new FloxStrategyCallbacks(callbacks)),
        _registry(&registry)
  {
  }

  ~BridgeStrategy()
  {
    delete _cb.load(std::memory_order_acquire);
  }

  // Atomically replace the callback set. In-flight dispatches see
  // either the old or the new callback set; the struct is never
  // torn. State carry-over happens through the on_stop / on_start
  // contract: on_stop fires on the old user_data before the swap,
  // on_start on the new user_data after.
  //
  // Old callback structs are intentionally leaked. Hot-reload is
  // rare; ~56 bytes per swap is preferable to wiring an
  // epoch-based reclaimer for a feature that fires once an hour.
  void replaceCallbacks(FloxStrategyCallbacks newCb)
  {
    FloxStrategyCallbacks* old = _cb.load(std::memory_order_acquire);
    if (old && old->on_stop)
    {
      old->on_stop(old->user_data);
    }
    auto* next = new FloxStrategyCallbacks(newCb);
    _cb.store(next, std::memory_order_release);
    if (newCb.on_start)
    {
      newCb.on_start(newCb.user_data);
    }
  }

  const SymbolRegistry& registry() const { return *_registry; }

  void start() override
  {
    auto cb = _cb.load(std::memory_order_acquire);
    if (cb && cb->on_start)
    {
      cb->on_start(cb->user_data);
    }
  }

  void stop() override
  {
    auto cb = _cb.load(std::memory_order_acquire);
    if (cb && cb->on_stop)
    {
      cb->on_stop(cb->user_data);
    }
  }

  OrderId publicEmitMarketBuy(SymbolId symbol, Quantity qty)
  {
    return emitMarketBuy(symbol, qty);
  }

  OrderId publicEmitMarketSell(SymbolId symbol, Quantity qty)
  {
    return emitMarketSell(symbol, qty);
  }

  OrderId publicEmitLimitBuy(SymbolId symbol, Price price, Quantity qty)
  {
    return emitLimitBuy(symbol, price, qty);
  }

  OrderId publicEmitLimitSell(SymbolId symbol, Price price, Quantity qty)
  {
    return emitLimitSell(symbol, price, qty);
  }

  void publicEmitCancel(OrderId orderId) { emitCancel(orderId); }

  void publicEmitCancelAll(SymbolId symbol) { emitCancelAll(symbol); }

  void publicEmitModify(OrderId orderId, Price newPrice, Quantity newQty)
  {
    emitModify(orderId, newPrice, newQty);
  }

  OrderId publicEmitStopMarket(SymbolId symbol, Side side, Price triggerPrice, Quantity qty)
  {
    return emitStopMarket(symbol, side, triggerPrice, qty);
  }

  OrderId publicEmitStopLimit(SymbolId symbol, Side side, Price triggerPrice, Price limitPrice,
                              Quantity qty)
  {
    return emitStopLimit(symbol, side, triggerPrice, limitPrice, qty);
  }

  OrderId publicEmitTakeProfitMarket(SymbolId symbol, Side side, Price triggerPrice, Quantity qty)
  {
    return emitTakeProfitMarket(symbol, side, triggerPrice, qty);
  }

  OrderId publicEmitTakeProfitLimit(SymbolId symbol, Side side, Price triggerPrice,
                                    Price limitPrice, Quantity qty)
  {
    return emitTakeProfitLimit(symbol, side, triggerPrice, limitPrice, qty);
  }

  OrderId publicEmitTrailingStop(SymbolId symbol, Side side, Price offset, Quantity qty)
  {
    return emitTrailingStop(symbol, side, offset, qty);
  }

  OrderId publicEmitTrailingStopPercent(SymbolId symbol, Side side, int32_t callbackBps,
                                        Quantity qty)
  {
    return emitTrailingStopPercent(symbol, side, callbackBps, qty);
  }

  OrderId publicEmitProvideLiquidity(SymbolId pool, Price priceLower, Price priceUpper,
                                     Quantity liquidity)
  {
    return emitProvideLiquidity(pool, priceLower, priceUpper, liquidity);
  }

  OrderId publicEmitWithdrawLiquidity(SymbolId pool, Quantity liquidity)
  {
    return emitWithdrawLiquidity(pool, liquidity);
  }

  OrderId publicEmitLimitBuyTif(SymbolId symbol, Price price, Quantity qty, TimeInForce tif)
  {
    return emitLimitBuy(symbol, price, qty, tif);
  }

  OrderId publicEmitLimitSellTif(SymbolId symbol, Price price, Quantity qty, TimeInForce tif)
  {
    return emitLimitSell(symbol, price, qty, tif);
  }

  OrderId publicEmitClosePosition(SymbolId symbol) { return emitClosePosition(symbol); }

  // Expose protected context and order accessors
  using Strategy::ctx;
  using Strategy::getOrder;
  using Strategy::getOrderStatus;
  using Strategy::position;

 protected:
  void onSymbolTrade(SymbolContext& c, const TradeEvent& ev) override
  {
    auto* cb = _cb.load(std::memory_order_acquire);
    if (!cb || !cb->on_trade)
    {
      return;
    }

    FloxSymbolContext fctx = toFloxContext(c);
    FloxTradeData ftrade{};
    ftrade.symbol = ev.trade.symbol;
    ftrade.price_raw = ev.trade.price.raw();
    ftrade.quantity_raw = ev.trade.quantity.raw();
    ftrade.is_buy = ev.trade.isBuy ? 1 : 0;
    ftrade.exchange_ts_ns = static_cast<int64_t>(ev.trade.exchangeTsNs);

    cb->on_trade(cb->user_data, &fctx, &ftrade);
  }

  void onSymbolBook(SymbolContext& c, const BookUpdateEvent& ev) override
  {
    auto* cb = _cb.load(std::memory_order_acquire);
    if (!cb || !cb->on_book)
    {
      return;
    }

    FloxSymbolContext fctx = toFloxContext(c);
    FloxBookData fbook{};
    fbook.symbol = ev.update.symbol;
    fbook.exchange_ts_ns = static_cast<int64_t>(ev.update.exchangeTsNs);
    fbook.snapshot = toBookSnapshot(c);

    cb->on_book(cb->user_data, &fctx, &fbook);
  }

  void onSymbolBar(SymbolContext& c, const BarEvent& ev) override
  {
    auto* cb = _cb.load(std::memory_order_acquire);
    if (!cb || !cb->on_bar)
    {
      return;
    }

    FloxSymbolContext fctx = toFloxContext(c);
    FloxBarData fbar{};
    fbar.symbol = ev.symbol;
    fbar.bar_type = static_cast<uint8_t>(ev.barType);
    fbar.close_reason = static_cast<uint8_t>(ev.bar.reason);
    fbar.bar_type_param = ev.barTypeParam;
    fbar.open_raw = ev.bar.open.raw();
    fbar.high_raw = ev.bar.high.raw();
    fbar.low_raw = ev.bar.low.raw();
    fbar.close_raw = ev.bar.close.raw();
    fbar.volume_raw = ev.bar.volume.raw();
    fbar.buy_volume_raw = ev.bar.buyVolume.raw();
    fbar.trade_count_raw = ev.bar.tradeCount.raw();
    fbar.start_time_ns = ev.bar.startTime.time_since_epoch().count();
    fbar.end_time_ns = ev.bar.endTime.time_since_epoch().count();

    cb->on_bar(cb->user_data, &fctx, &fbar);
  }

  void onSymbolFill(SymbolContext& c, const OrderEvent& ev) override
  {
    auto* cb = _cb.load(std::memory_order_acquire);
    if (!cb || !cb->on_fill)
    {
      return;
    }
    FloxSymbolContext fctx = toFloxContext(c);
    FloxOrderEventData fev = toFloxOrderEvent(ev);
    cb->on_fill(cb->user_data, &fctx, &fev);
  }

  void onSymbolOrderUpdate(SymbolContext& c, const OrderEvent& ev) override
  {
    auto* cb = _cb.load(std::memory_order_acquire);
    if (!cb || !cb->on_order_update)
    {
      return;
    }
    FloxSymbolContext fctx = toFloxContext(c);
    FloxOrderEventData fev = toFloxOrderEvent(ev);
    cb->on_order_update(cb->user_data, &fctx, &fev);
  }

  void onSymbolQueuePositionChange(SymbolContext& c, const OrderEvent& ev) override
  {
    auto* cb = _cb.load(std::memory_order_acquire);
    if (!cb || !cb->on_queue_position_change)
    {
      return;
    }
    FloxSymbolContext fctx = toFloxContext(c);
    FloxOrderEventData fev = toFloxOrderEvent(ev);
    cb->on_queue_position_change(cb->user_data, &fctx, &fev);
  }

  void onSymbolMarketPositionChange(SymbolContext& c, const OrderEvent& ev) override
  {
    auto* cb = _cb.load(std::memory_order_acquire);
    if (!cb || !cb->on_market_position_change)
    {
      return;
    }
    FloxSymbolContext fctx = toFloxContext(c);
    FloxOrderEventData fev = toFloxOrderEvent(ev);
    cb->on_market_position_change(cb->user_data, &fctx, &fev);
  }

 private:
  static FloxOrderEventData toFloxOrderEvent(const OrderEvent& ev)
  {
    FloxOrderEventData fev{};
    fev.order_id = ev.order.id;
    fev.symbol_id = ev.order.symbol;
    fev.side = (ev.order.side == Side::BUY) ? 0 : 1;
    fev.order_type = static_cast<uint8_t>(ev.order.type);
    fev.status = static_cast<uint8_t>(ev.status);
    fev.fill_qty_raw = ev.fillQty.raw();
    fev.fill_price_raw = ev.fillPrice.raw();
    fev.exchange_ts_ns = ev.exchangeTsNs;
    fev.reject_reason = ev.rejectReason.empty() ? nullptr : ev.rejectReason.c_str();
    fev.queue_ahead_raw = ev.queueAhead.raw();
    fev.queue_total_raw = ev.queueTotal.raw();
    fev.submitted_at_ns = ev.timestamps.submittedAtNs;
    fev.accepted_at_ns = ev.timestamps.acceptedAtNs;
    fev.first_fill_at_ns = ev.timestamps.firstFillAtNs;
    fev.last_fill_at_ns = ev.timestamps.lastFillAtNs;
    fev.canceled_at_ns = ev.timestamps.canceledAtNs;
    fev.rejected_at_ns = ev.timestamps.rejectedAtNs;
    fev.triggered_at_ns = ev.timestamps.triggeredAtNs;
    fev.expired_at_ns = ev.timestamps.expiredAtNs;
    fev.is_maker = ev.isMaker ? 1 : 0;
    fev.market_position = static_cast<uint8_t>(ev.marketPosition);
    fev.distance_to_best_ticks = ev.distanceToBestTicks;
    return fev;
  }
  static FloxBookSnapshot toBookSnapshot(const SymbolContext& c)
  {
    FloxBookSnapshot snap{};
    auto bid = c.book.bestBid();
    auto ask = c.book.bestAsk();
    if (bid)
    {
      snap.bid_price_raw = bid->raw();
      // Quantity at best bid not directly available from bestBid()
      snap.bid_qty_raw = 0;
    }
    if (ask)
    {
      snap.ask_price_raw = ask->raw();
      snap.ask_qty_raw = 0;
    }
    auto mid = c.mid();
    if (mid)
    {
      snap.mid_raw = mid->raw();
    }
    auto spread = c.bookSpread();
    if (spread)
    {
      snap.spread_raw = spread->raw();
    }
    return snap;
  }

  static FloxSymbolContext toFloxContext(const SymbolContext& c)
  {
    FloxSymbolContext fctx{};
    fctx.symbol_id = c.symbolId;
    fctx.position_raw = c.position.raw();
    fctx.avg_entry_price_raw = c.avgEntryPrice.raw();
    fctx.last_trade_price_raw = c.lastTradePrice.raw();
    fctx.last_update_ns = c.lastUpdateNs;
    fctx.book = toBookSnapshot(c);
    return fctx;
  }

  std::atomic<FloxStrategyCallbacks*> _cb;
  const SymbolRegistry* _registry;
};

}  // namespace flox
