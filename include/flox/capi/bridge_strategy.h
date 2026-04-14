/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/capi/flox_capi.h"
#include "flox/strategy/strategy.h"

namespace flox
{

class BridgeStrategy : public Strategy
{
 public:
  BridgeStrategy(SubscriberId id, std::vector<SymbolId> symbols, const SymbolRegistry& registry,
                 FloxStrategyCallbacks callbacks)
      : Strategy(id, std::move(symbols), registry), _cb(callbacks)
  {
  }

  void start() override
  {
    if (_cb.on_start)
    {
      _cb.on_start(_cb.user_data);
    }
  }

  void stop() override
  {
    if (_cb.on_stop)
    {
      _cb.on_stop(_cb.user_data);
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
    if (!_cb.on_trade)
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

    _cb.on_trade(_cb.user_data, &fctx, &ftrade);
  }

  void onSymbolBook(SymbolContext& c, const BookUpdateEvent& ev) override
  {
    if (!_cb.on_book)
    {
      return;
    }

    FloxSymbolContext fctx = toFloxContext(c);
    FloxBookData fbook{};
    fbook.symbol = ev.update.symbol;
    fbook.exchange_ts_ns = static_cast<int64_t>(ev.update.exchangeTsNs);
    fbook.snapshot = toBookSnapshot(c);

    _cb.on_book(_cb.user_data, &fctx, &fbook);
  }

 private:
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

  FloxStrategyCallbacks _cb;
};

}  // namespace flox
