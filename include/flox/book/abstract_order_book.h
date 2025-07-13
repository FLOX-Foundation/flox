/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/book/events/book_update_event.h"
#include "flox/common.h"

namespace flox
{

class IOrderBook
{
 public:
  virtual ~IOrderBook() = default;

  virtual void applyBookUpdate(const BookUpdateEvent& update) = 0;
  virtual std::optional<Price> bestBid() const = 0;
  virtual std::optional<Price> bestAsk() const = 0;

  virtual Quantity bidAtPrice(Price price) const = 0;
  virtual Quantity askAtPrice(Price price) const = 0;
};

}  // namespace flox