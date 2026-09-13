/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/aggregator/bar_series.h"
#include "flox/aggregator/events/bar_event.h"
#include "flox/aggregator/timeframe.h"
#include "flox/common.h"
#include "flox/engine/abstract_market_data_subscriber.h"

#include <algorithm>
#include <array>
#include <span>
#include <vector>

// Upper bound on a single BarMatrix instantiation, in bytes. An instantiation
// past this is almost certainly a typo in the template arguments rather than an
// intent to hold that many bars; define it to a larger value before including
// this header if you really do want one.
#ifndef FLOX_BAR_MATRIX_MAX_BYTES
#define FLOX_BAR_MATRIX_MAX_BYTES (256ull * 1024 * 1024)
#endif

namespace flox
{

// A BarMatrix owns MaxSymbols x MaxTimeframes ring buffers of Depth bars each,
// so its footprint is the product of all three parameters. The defaults come to
// roughly 40 MB, and even a trimmed <256, 4, 64> is about 5 MB -- past a
// worker thread's stack, and past the main thread's too once assertions are on,
// because FLOX_SCALE_CHECKS widens every Decimal and pushes sizeof(Bar) from 80
// to 136 bytes. Declaring one as a local is a stack overflow before the first
// line of the function body runs, which no compiler diagnostic will warn about.
//
// Allocate it on the heap (std::make_unique) or give it static storage, and
// read kStorageBytes if you need the number for a budget of your own.
template <size_t MaxSymbols = 256, size_t MaxTimeframes = 8, size_t Depth = 256>
class BarMatrix : public IMarketDataSubscriber
{
 public:
  static constexpr size_t kMaxSymbols = MaxSymbols;
  static constexpr size_t kMaxTimeframes = MaxTimeframes;
  static constexpr size_t kDepth = Depth;

  // Bar storage alone; the object is a little larger for the timeframe table
  // and the per-symbol bookkeeping.
  static constexpr size_t kStorageBytes = MaxSymbols * MaxTimeframes * Depth * sizeof(Bar);

  static_assert(kStorageBytes <= FLOX_BAR_MATRIX_MAX_BYTES,
                "BarMatrix instantiation exceeds FLOX_BAR_MATRIX_MAX_BYTES. Its size is "
                "MaxSymbols * MaxTimeframes * Depth * sizeof(Bar); shrink a parameter, or "
                "define FLOX_BAR_MATRIX_MAX_BYTES higher before including this header. Note "
                "that an object this size belongs on the heap, never on a stack.");

  BarMatrix() = default;

  void configure(std::span<const TimeframeId> timeframes)
  {
    _numTimeframes = std::min(timeframes.size(), MaxTimeframes);
    for (size_t i = 0; i < _numTimeframes; ++i)
    {
      _timeframes[i] = timeframes[i];
    }
  }

  SubscriberId id() const override { return reinterpret_cast<SubscriberId>(this); }

  void onBar(const BarEvent& ev) override
  {
    const auto tfIdx = findTimeframeIndex(TimeframeId(ev.barType, ev.barTypeParam));
    if (tfIdx >= _numTimeframes)
    {
      return;
    }

    auto& symbolData = getSymbolData(ev.symbol);
    symbolData.series[tfIdx].push(ev.bar);
  }

  const Bar* bar(SymbolId sym, TimeframeId tf, size_t idx = 0) const noexcept
  {
    const auto tfIdx = findTimeframeIndex(tf);
    if (tfIdx >= _numTimeframes)
    {
      return nullptr;
    }
    return bar(sym, tfIdx, idx);
  }

  const Bar* bar(SymbolId sym, size_t tfIdx, size_t idx = 0) const noexcept
  {
    if (tfIdx >= _numTimeframes)
    {
      return nullptr;
    }

    const auto* symbolData = tryGetSymbolData(sym);
    if (!symbolData)
    {
      return nullptr;
    }

    return symbolData->series[tfIdx].at(idx);
  }

  const BarSeries<Depth>* series(SymbolId sym, TimeframeId tf) const noexcept
  {
    const auto tfIdx = findTimeframeIndex(tf);
    if (tfIdx >= _numTimeframes)
    {
      return nullptr;
    }
    return series(sym, tfIdx);
  }

  const BarSeries<Depth>* series(SymbolId sym, size_t tfIdx) const noexcept
  {
    if (tfIdx >= _numTimeframes)
    {
      return nullptr;
    }
    const auto* symbolData = tryGetSymbolData(sym);
    if (!symbolData)
    {
      return nullptr;
    }
    return &symbolData->series[tfIdx];
  }

  void warmup(SymbolId sym, TimeframeId tf, std::span<const Bar> history)
  {
    const auto tfIdx = findTimeframeIndex(tf);
    if (tfIdx >= _numTimeframes)
    {
      return;
    }

    auto& symbolData = getSymbolData(sym);
    auto& s = symbolData.series[tfIdx];

    for (auto it = history.rbegin(); it != history.rend(); ++it)
    {
      s.push(*it);
    }
  }

  std::span<const TimeframeId> timeframes() const noexcept
  {
    return std::span<const TimeframeId>(_timeframes.data(), _numTimeframes);
  }

  size_t timeframeIndex(TimeframeId tf) const noexcept
  {
    return findTimeframeIndex(tf);
  }

  void clear()
  {
    for (auto& data : _flat)
    {
      for (auto& s : data.series)
      {
        s.clear();
      }
    }
    _initialized = {};
    _overflow.clear();
  }

 private:
  struct SymbolData
  {
    std::array<BarSeries<Depth>, MaxTimeframes> series{};
  };

  size_t findTimeframeIndex(TimeframeId tf) const noexcept
  {
    for (size_t i = 0; i < _numTimeframes; ++i)
    {
      if (_timeframes[i] == tf)
      {
        return i;
      }
    }
    return _numTimeframes;
  }

  SymbolData& getSymbolData(SymbolId sym)
  {
    if (sym < MaxSymbols) [[likely]]
    {
      if (!_initialized[sym])
      {
        _flat[sym] = SymbolData{};
        _initialized[sym] = true;
      }
      return _flat[sym];
    }
    return getOverflow(sym);
  }

  const SymbolData* tryGetSymbolData(SymbolId sym) const noexcept
  {
    if (sym < MaxSymbols)
    {
      return _initialized[sym] ? &_flat[sym] : nullptr;
    }
    for (const auto& [id, data] : _overflow)
    {
      if (id == sym)
      {
        return &data;
      }
    }
    return nullptr;
  }

  SymbolData& getOverflow(SymbolId sym)
  {
    for (auto& [id, data] : _overflow)
    {
      if (id == sym)
      {
        return data;
      }
    }
    _overflow.emplace_back(sym, SymbolData{});
    return _overflow.back().second;
  }

  std::array<TimeframeId, MaxTimeframes> _timeframes{};
  size_t _numTimeframes = 0;

  alignas(64) std::array<SymbolData, MaxSymbols> _flat{};
  std::array<bool, MaxSymbols> _initialized{};
  std::vector<std::pair<SymbolId, SymbolData>> _overflow;
};

}  // namespace flox
