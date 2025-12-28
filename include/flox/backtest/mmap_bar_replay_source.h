/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/backtest/bar_replay_source.h"
#include "flox/backtest/mmap_bar_storage.h"

#include <iostream>
#include <mutex>
#include <queue>

namespace flox
{

class MmapBarReplaySource : public IBarReplaySource
{
 public:
  explicit MmapBarReplaySource(std::unique_ptr<MmapBarStorage> storage, SymbolId symbolId = 0)
      : _storage(std::move(storage)), _symbolId(symbolId)
  {
  }

  size_t replay(std::function<void(const BarEvent&)> callback) override
  {
    std::call_once(_cacheInitFlag, [this]()
                   { buildSortedCache(); });

    for (const auto& entry : _sortedBars)
    {
      BarEvent ev;
      ev.symbol = _symbolId;
      ev.barType = entry.timeframe.type;
      ev.barTypeParam = entry.timeframe.param;
      ev.bar = *entry.barPtr;
      callback(ev);
    }

    return _sortedBars.size();
  }

  std::pair<TimePoint, TimePoint> timeRange() const override { return _storage->timeRange(); }

  size_t totalBars() const override { return _storage->totalBars(); }

  std::vector<TimeframeId> timeframes() const override { return _storage->timeframes(); }

  const MmapBarStorage* storage() const { return _storage.get(); }

 private:
  struct SortedBarEntry
  {
    TimeframeId timeframe;
    const Bar* barPtr;

    bool operator<(const SortedBarEntry& other) const
    {
      return barPtr->endTime < other.barPtr->endTime;
    }
  };

  void buildSortedCache()
  {
    struct TimeframeIterator
    {
      TimeframeId timeframe;
      std::span<const Bar>::iterator current;
      std::span<const Bar>::iterator end;

      bool operator>(const TimeframeIterator& other) const
      {
        return current->endTime > other.current->endTime;
      }
    };

    std::priority_queue<TimeframeIterator, std::vector<TimeframeIterator>, std::greater<>> heap;

    for (const auto& tf : _storage->timeframes())
    {
      auto span = _storage->getBars(tf);
      if (!span.empty())
      {
        heap.push({tf, span.begin(), span.end()});
      }
    }

    _sortedBars.reserve(_storage->totalBars());

    while (!heap.empty())
    {
      auto iter = heap.top();
      heap.pop();

      _sortedBars.push_back({iter.timeframe, &(*iter.current)});

      ++iter.current;
      if (iter.current != iter.end)
      {
        heap.push(iter);
      }
    }
  }

  std::unique_ptr<MmapBarStorage> _storage;
  std::vector<SortedBarEntry> _sortedBars;
  std::once_flag _cacheInitFlag;
  SymbolId _symbolId{0};
};

}  // namespace flox
