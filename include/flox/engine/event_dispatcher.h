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
#include "flox/engine/abstract_market_data_subscriber.h"
#include "flox/execution/events/order_event.h"

namespace flox
{

template <typename T>
struct EventDispatcher;

template <typename T>
struct EventDispatcher<pool::Handle<T>>
{
  template <typename Sub>
  static void dispatch(const pool::Handle<T>& ev, Sub& sub)
  {
    EventDispatcher<T>::dispatch(*ev, sub);
  }
};

template <>
struct EventDispatcher<BookUpdateEvent>
{
  // Templated on the subscriber so a statically-subscribed concrete type
  // keeps its identity all the way to the handler call (see subscribeStatic).
  template <typename Sub>
  static void dispatch(const BookUpdateEvent& ev, Sub& sub)
  {
    sub.onBookUpdate(ev);
  }
};

template <>
struct EventDispatcher<TradeEvent>
{
  // Templated on the subscriber so a statically-subscribed concrete type
  // keeps its identity all the way to the handler call (see subscribeStatic).
  template <typename Sub>
  static void dispatch(const TradeEvent& ev, Sub& sub)
  {
    sub.onTrade(ev);
  }
};

template <>
struct EventDispatcher<BarEvent>
{
  // Templated on the subscriber so a statically-subscribed concrete type
  // keeps its identity all the way to the handler call (see subscribeStatic).
  template <typename Sub>
  static void dispatch(const BarEvent& ev, Sub& sub)
  {
    sub.onBar(ev);
  }
};

template <>
struct EventDispatcher<OrderEvent>
{
  template <typename Sub>
  static void dispatch(const OrderEvent& ev, Sub& listener)
  {
    ev.dispatchTo(listener);
  }
};

}  // namespace flox