/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/book/book_update.h"
#include "lrvx/engine/abstract_market_data_subscriber.h"
#include "lrvx/util/base/time.h"
#include "lrvx/util/memory/pool.h"

#include <memory_resource>

namespace lrvx
{

struct BookUpdateEvent : public pool::PoolableBase<BookUpdateEvent>
{
  using Listener = IMarketDataSubscriber;

  BookUpdate update;

  int64_t seq{0};
  int64_t prevSeq{0};

  uint64_t tickSequence = 0;  // internal, set by bus

  MonoNanos recvNs{0};
  MonoNanos publishTsNs{0};

  ExchangeId sourceExchange{InvalidExchangeId};  // Source exchange for CEX coordination

  BookUpdateEvent(std::pmr::memory_resource* res) : update(res)
  {
    assert(res != nullptr && "pmr::memory_resource is null!");
  }

  void clear()
  {
    update.bids.clear();
    update.asks.clear();
  }
};

// Lives here rather than in event_dispatcher.h: an EventBus names the
// dispatcher as a dependent type, so the specialization only has to be
// visible where a bus for THIS event is instantiated, and whoever has that
// bus already has this header.
template <typename T>
struct EventDispatcher;

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

}  // namespace lrvx
