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
#include "flox/util/base/time.h"

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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

  // A feed that stops delivering while its socket stays open is invisible to
  // the disconnect path, and no connector owns a timer of its own, so the
  // check is driven from outside on the supervisor's own cadence. Connectors
  // that track per-symbol arrival times override this and forward their
  // configured window to checkStaleFeeds().
  virtual void pollFeedHealth(MonoNanos /*now*/) {}

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

  // Record that data for `symbol` arrived at `when`. Called from the
  // market-data thread; checkStaleFeeds() reads it from whichever thread
  // drives pollFeedHealth(), hence the lock. It is uncontended in practice
  // and costs orders of magnitude less than the JSON parse that precedes
  // every call.
  void markFeedActivity(SymbolId symbol, MonoNanos when)
  {
    std::lock_guard<std::mutex> lk(_feedMutex);
    auto& state = _feeds[symbol];
    state.lastUpdate = when;
    state.reported = false;  // fresh data re-arms the report
  }

  // Emit emitStaleData() for every symbol whose last update is older than
  // `timeoutMs`, once per staleness episode -- a supervisor polling at 1 Hz
  // wants one event per feed that died, not one per poll, and fresh data
  // re-arms it through markFeedActivity(). A window of 0 (or less) disables
  // the check entirely.
  void checkStaleFeeds(MonoNanos now, int timeoutMs)
  {
    if (timeoutMs <= 0)
    {
      return;
    }

    const uint64_t windowNs = static_cast<uint64_t>(timeoutMs) * 1'000'000ULL;
    std::vector<std::pair<SymbolId, uint64_t>> stale;
    {
      std::lock_guard<std::mutex> lk(_feedMutex);
      for (auto& [symbol, state] : _feeds)
      {
        if (state.reported || now.raw() <= state.lastUpdate.raw() ||
            now.raw() - state.lastUpdate.raw() < windowNs)
        {
          continue;
        }
        state.reported = true;
        stale.emplace_back(symbol, state.lastUpdate.raw() / 1'000'000ULL);
      }
    }

    // Emitted outside the lock: the callback belongs to the supervisor and
    // may call straight back into this connector.
    for (const auto& [symbol, lastUpdateMs] : stale)
    {
      emitStaleData(symbol, lastUpdateMs);
    }
  }

 private:
  struct FeedState
  {
    MonoNanos lastUpdate{0};
    bool reported{false};
  };

  std::mutex _feedMutex;
  std::unordered_map<SymbolId, FeedState> _feeds;

  BookUpdateCallback _onBookUpdate;
  TradeCallback _onTrade;
  DisconnectCallback _onDisconnect;
  SequenceGapCallback _onSequenceGap;
  StaleDataCallback _onStaleData;
};

}  // namespace flox
