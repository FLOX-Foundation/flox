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
#include "flox/common.h"
#include "flox/engine/abstract_subsystem.h"
#include "flox/util/base/move_only_function.h"

#include <string>
#include <string_view>

namespace flox
{

class IExchangeConnector : public ISubsystem, public IDrainable
{
 public:
  virtual ~IExchangeConnector() = default;

  bool drain(uint32_t /*timeoutMs*/) override { return true; }

  using BookUpdateCallback = MoveOnlyFunction<void(const BookUpdateEvent&)>;
  using TradeCallback = MoveOnlyFunction<void(const TradeEvent&)>;
  using DisconnectCallback = MoveOnlyFunction<void(std::string_view reason)>;
  using SequenceGapCallback = MoveOnlyFunction<void(uint64_t expected, uint64_t received)>;
  using StaleDataCallback = MoveOnlyFunction<void(SymbolId symbol, uint64_t lastUpdateMs)>;

  virtual std::string exchangeId() const = 0;

  virtual void setCallbacks(BookUpdateCallback onBookUpdate, TradeCallback onTrade)
  {
    _onBookUpdate = std::move(onBookUpdate);
    _onTrade = std::move(onTrade);
  }

  virtual void setErrorCallbacks(DisconnectCallback onDisconnect,
                                 SequenceGapCallback onSequenceGap,
                                 StaleDataCallback onStaleData)
  {
    _onDisconnect = std::move(onDisconnect);
    _onSequenceGap = std::move(onSequenceGap);
    _onStaleData = std::move(onStaleData);
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

  void emitDisconnect(std::string_view reason)
  {
    if (_onDisconnect)
    {
      _onDisconnect(reason);
    }
  }

  void emitSequenceGap(uint64_t expected, uint64_t received)
  {
    if (_onSequenceGap)
    {
      _onSequenceGap(expected, received);
    }
  }

  void emitStaleData(SymbolId symbol, uint64_t lastUpdateMs)
  {
    if (_onStaleData)
    {
      _onStaleData(symbol, lastUpdateMs);
    }
  }

 private:
  BookUpdateCallback _onBookUpdate;
  TradeCallback _onTrade;
  DisconnectCallback _onDisconnect;
  SequenceGapCallback _onSequenceGap;
  StaleDataCallback _onStaleData;
};

}  // namespace flox
