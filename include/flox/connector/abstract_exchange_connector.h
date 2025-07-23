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
#include "flox/book/events/trade_event.h"

#include <functional>
#include <string>

namespace flox
{

class IExchangeConnector
{
 public:
  virtual ~IExchangeConnector() = default;

  using BookUpdateCallback = std::move_only_function<void(const BookUpdateEvent&)>;
  using TradeCallback = std::move_only_function<void(const TradeEvent&)>;

  virtual void start() = 0;
  virtual void stop() = 0;

  virtual std::string exchangeId() const = 0;

  virtual void setCallbacks(BookUpdateCallback onBookUpdate, TradeCallback onTrade)
  {
    _onBookUpdate = std::move(onBookUpdate);
    _onTrade = std::move(onTrade);
  }

 protected:
  void emitBookUpdate(const BookUpdateEvent& bu)
  {
    if (_onBookUpdate)
    {
      _onBookUpdate(bu);
    }
  }

  void emitTrade(const TradeEvent& t)
  {
    if (_onTrade)
    {
      _onTrade(t);
    }
  }

 private:
  BookUpdateCallback _onBookUpdate;
  TradeCallback _onTrade;
};

}  // namespace flox
