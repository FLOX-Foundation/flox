/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/aggregator/events/candle_event.h"
#include "flox/book/events/book_update_event.h"
#include "flox/book/events/trade_event.h"
#include "flox/engine/abstract_market_data_subscriber.h"
#include "flox/execution/events/order_event.h"

namespace flox
{

template <typename T>
struct EventDispatcher;

template <typename T>
struct EventDispatcher<pool::Handle<T>>
{
  static void dispatch(const pool::Handle<T>& ev, typename T::Listener& sub)
  {
    EventDispatcher<T>::dispatch(*ev, sub);
  }
};

template <>
struct EventDispatcher<BookUpdateEvent>
{
  static void dispatch(const BookUpdateEvent& ev, IMarketDataSubscriber& sub)
  {
    sub.onBookUpdate(ev);
  }
};

template <>
struct EventDispatcher<TradeEvent>
{
  static void dispatch(const TradeEvent& ev, IMarketDataSubscriber& sub)
  {
    sub.onTrade(ev);
  }
};

template <>
struct EventDispatcher<CandleEvent>
{
  static void dispatch(const CandleEvent& ev, IMarketDataSubscriber& sub)
  {
    sub.onCandle(ev);
  }
};

template <>
struct EventDispatcher<OrderEvent>
{
  static void dispatch(const OrderEvent& ev, IOrderExecutionListener& listener)
  {
    ev.dispatchTo(listener);
  }
};

}  // namespace flox