/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/book/book_update.h"
#include "flox/engine/abstract_market_data_subscriber.h"
#include "flox/util/memory/pool.h"

#include <memory_resource>

namespace flox
{

struct BookUpdateEvent : public pool::PoolableBase<BookUpdateEvent>
{
  using Listener = IMarketDataSubscriber;

  BookUpdate update;

  uint64_t tickSequence = 0;

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

}  // namespace flox
