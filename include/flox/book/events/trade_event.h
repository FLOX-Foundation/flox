/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/book/trade.h"
#include "flox/engine/abstract_market_data_subscriber.h"
#include "flox/util/base/time.h"

namespace flox
{

struct TradeEvent
{
  using Listener = IMarketDataSubscriber;

  Trade trade{};

  int64_t seq = 0;
  uint64_t trade_id = 0;

  uint64_t tickSequence = 0;  // internal, set by bus

  MonoNanos recvNs{0};
  MonoNanos publishTsNs{0};
  UnixNanos exchangeMsgTsNs{0};

  ExchangeId sourceExchange{InvalidExchangeId};  // Source exchange for CEX coordination
};

// Lives here rather than in event_dispatcher.h: an EventBus names the
// dispatcher as a dependent type, so the specialization only has to be
// visible where a bus for THIS event is instantiated, and whoever has that
// bus already has this header.
template <typename T>
struct EventDispatcher;

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

}  // namespace flox
