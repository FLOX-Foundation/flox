/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/backtest_runner.h"

#include "flox/book/events/book_update_event.h"
#include "flox/book/events/trade_event.h"
#include "flox/strategy/strategy.h"

#include <memory_resource>

namespace flox
{

BacktestRunner::BacktestRunner(const BacktestConfig& config) : _config(config), _executor(_clock)
{
  _executor.setOrderEventCallback([this](const OrderEvent& ev)
                                  { onOrderEvent(ev); });
}

void BacktestRunner::setStrategy(IStrategy* strategy)
{
  _strategy = strategy;
}

void BacktestRunner::setStrategy(Strategy* strategy)
{
  _strategy = strategy;
  strategy->setSignalHandler(this);
}

void BacktestRunner::addExecutionListener(IOrderExecutionListener* listener)
{
  _execution_listeners.push_back(listener);
}

BacktestResult BacktestRunner::run(replay::IMultiSegmentReader& reader)
{
  if (_strategy)
  {
    _strategy->start();
  }

  std::pmr::monotonic_buffer_resource pool(1024 * 1024);

  reader.forEach([&](const replay::ReplayEvent& event) -> bool
                 {
    _clock.advanceTo(event.timestamp_ns);

    if (event.type == replay::EventType::Trade)
    {
      TradeEvent trade_ev;
      trade_ev.trade.symbol = event.trade.symbol_id;
      trade_ev.trade.price = Price::fromRaw(event.trade.price_raw);
      trade_ev.trade.quantity = Quantity::fromRaw(event.trade.qty_raw);
      trade_ev.trade.isBuy = (event.trade.side == 1);
      trade_ev.trade.exchangeTsNs = event.trade.exchange_ts_ns;
      trade_ev.trade.instrument = static_cast<InstrumentType>(event.trade.instrument);
      trade_ev.exchangeMsgTsNs = event.trade.exchange_ts_ns;

      _executor.onTrade(trade_ev.trade.symbol, trade_ev.trade.price, trade_ev.trade.isBuy);

      if (_strategy)
      {
        _strategy->onTrade(trade_ev);
      }
    }
    else if (event.type == replay::EventType::BookSnapshot ||
             event.type == replay::EventType::BookDelta)
    {
      BookUpdateEvent book_ev(&pool);
      book_ev.update.symbol = event.book_header.symbol_id;
      book_ev.update.type = (event.type == replay::EventType::BookSnapshot) ? BookUpdateType::SNAPSHOT
                                                                            : BookUpdateType::DELTA;
      book_ev.update.instrument = static_cast<InstrumentType>(event.book_header.instrument);
      book_ev.update.exchangeTsNs = event.book_header.exchange_ts_ns;
      book_ev.seq = event.book_header.seq;

      book_ev.update.bids.reserve(event.bids.size());
      book_ev.update.asks.reserve(event.asks.size());

      for (const auto& bid : event.bids)
      {
        book_ev.update.bids.emplace_back(Price::fromRaw(bid.price_raw), Quantity::fromRaw(bid.qty_raw));
      }
      for (const auto& ask : event.asks)
      {
        book_ev.update.asks.emplace_back(Price::fromRaw(ask.price_raw), Quantity::fromRaw(ask.qty_raw));
      }

      _executor.onBookUpdate(book_ev.update.symbol, book_ev.update.bids, book_ev.update.asks);

      if (_strategy)
      {
        _strategy->onBookUpdate(book_ev);
      }

      pool.release();
    }

    return true; });

  if (_strategy)
  {
    _strategy->stop();
  }

  BacktestResult result(_config);
  for (const auto& fill : _executor.fills())
  {
    result.recordFill(fill);
  }

  return result;
}

void BacktestRunner::onOrderEvent(const OrderEvent& ev)
{
  for (auto* listener : _execution_listeners)
  {
    ev.dispatchTo(*listener);
  }
}

void BacktestRunner::onSignal(const Signal& signal)
{
  switch (signal.type)
  {
    case SignalType::Market:
    case SignalType::Limit:
      _executor.submitOrder(signalToOrder(signal));
      break;
    case SignalType::Cancel:
      _executor.cancelOrder(signal.orderId);
      break;
    case SignalType::CancelAll:
      _executor.cancelAllOrders(signal.symbol);
      break;
    case SignalType::Modify:
    {
      Order newOrder{.id = _nextOrderId++,
                     .price = signal.newPrice,
                     .quantity = signal.newQuantity};
      _executor.replaceOrder(signal.orderId, newOrder);
      break;
    }
  }
}

Order BacktestRunner::signalToOrder(const Signal& sig)
{
  OrderId id = (sig.orderId != 0) ? sig.orderId : _nextOrderId++;
  return Order{.id = id,
               .side = sig.side,
               .price = sig.price,
               .quantity = sig.quantity,
               .type = (sig.type == SignalType::Market) ? OrderType::MARKET : OrderType::LIMIT,
               .symbol = sig.symbol};
}

}  // namespace flox
