/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/common.h"

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace flox
{

enum class FeedClockPolicy : uint8_t
{
  WaitForAll = 0,      // fire when every feed has ticked since last fire
  FireOnAny = 1,       // fire on every tick of a registered feed
  LeaderFollower = 2,  // fire on a leader tick if every follower is fresh
};

// Snapshot returned by `tick`. `fired` tells the caller whether the
// clock fired on this event; per-feed last-seen + staleness vectors
// let the strategy decide how to weight a stale leg if it does act.
//
// Layout choice: parallel vectors keyed by `symbols` index so the
// snapshot is cheap to inspect from any binding without dict allocs.
struct FeedClockSnapshot
{
  bool fired = false;
  SymbolId triggeredBy = 0;
  std::vector<SymbolId> symbols;
  std::vector<int64_t> lastTsNs;     // 0 if a feed has never ticked
  std::vector<int64_t> stalenessNs;  // tsNs - lastTsNs[i], clamped at 0
};

class MultiFeedClock
{
 public:
  MultiFeedClock(std::vector<SymbolId> symbols, FeedClockPolicy policy,
                 int64_t timeoutMs, SymbolId leader, int64_t stalenessBudgetMs)
      : _symbols(std::move(symbols)),
        _policy(policy),
        _timeoutNs(timeoutMs * 1'000'000),
        _leader(leader),
        _stalenessBudgetNs(stalenessBudgetMs * 1'000'000)
  {
    if (_leader == 0 && !_symbols.empty())
    {
      _leader = _symbols[0];
    }
    _lastSeen.resize(_symbols.size(), 0);
  }

  FeedClockSnapshot tick(int64_t tsNs, SymbolId sym)
  {
    int idx = -1;
    for (size_t i = 0; i < _symbols.size(); ++i)
    {
      if (_symbols[i] == sym)
      {
        idx = static_cast<int>(i);
        break;
      }
    }
    if (idx < 0)
    {
      // Out-of-band symbol — never fires on its own.
      return snapshot(tsNs, /*fired=*/false, /*triggeredBy=*/sym);
    }

    _lastSeen[idx] = tsNs;
    _seenSinceFire.insert(sym);

    bool fired = false;
    switch (_policy)
    {
      case FeedClockPolicy::WaitForAll:
      {
        bool all = true;
        for (auto s : _symbols)
        {
          if (_seenSinceFire.find(s) == _seenSinceFire.end())
          {
            all = false;
            break;
          }
        }
        if (all)
        {
          fired = true;
        }
        else if (_lastFireTs > 0 && (tsNs - _lastFireTs) > _timeoutNs)
        {
          fired = true;
        }
        break;
      }
      case FeedClockPolicy::FireOnAny:
        fired = true;
        break;
      case FeedClockPolicy::LeaderFollower:
        if (sym == _leader)
        {
          bool allFresh = true;
          for (size_t i = 0; i < _symbols.size(); ++i)
          {
            if (_symbols[i] == _leader)
            {
              continue;
            }
            int64_t last = _lastSeen[i];
            if (last == 0 || (tsNs - last) > _stalenessBudgetNs)
            {
              allFresh = false;
              break;
            }
          }
          fired = allFresh;
        }
        break;
    }

    if (fired)
    {
      _lastFireTs = tsNs;
      _seenSinceFire.clear();
    }
    return snapshot(tsNs, fired, sym);
  }

  void reset()
  {
    std::fill(_lastSeen.begin(), _lastSeen.end(), 0);
    _lastFireTs = 0;
    _seenSinceFire.clear();
  }

  size_t symbolCount() const noexcept { return _symbols.size(); }
  FeedClockPolicy policy() const noexcept { return _policy; }

 private:
  FeedClockSnapshot snapshot(int64_t tsNs, bool fired, SymbolId triggeredBy) const
  {
    FeedClockSnapshot out;
    out.fired = fired;
    out.triggeredBy = triggeredBy;
    out.symbols = _symbols;
    out.lastTsNs = _lastSeen;
    out.stalenessNs.resize(_symbols.size());
    for (size_t i = 0; i < _symbols.size(); ++i)
    {
      out.stalenessNs[i] = _lastSeen[i] == 0 ? 0 : std::max<int64_t>(0, tsNs - _lastSeen[i]);
    }
    return out;
  }

  std::vector<SymbolId> _symbols;
  FeedClockPolicy _policy;
  int64_t _timeoutNs;
  SymbolId _leader;
  int64_t _stalenessBudgetNs;
  std::vector<int64_t> _lastSeen;
  int64_t _lastFireTs = 0;
  std::unordered_set<SymbolId> _seenSinceFire;
};

}  // namespace flox
