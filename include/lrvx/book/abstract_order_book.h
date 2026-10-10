/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/book/events/book_update_event.h"
#include "lrvx/common.h"

namespace lrvx
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

}  // namespace lrvx