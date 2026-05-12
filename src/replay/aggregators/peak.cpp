/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/aggregators/peak.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <stdexcept>

namespace flox::replay
{

namespace
{
using HeapElem = std::pair<uint64_t, int64_t>;

// Min-heap on .first (count), ties broken by .second (start_ns). Allows
// efficient "is the new candidate better than the current minimum?"
// check at the top of a bounded heap.
struct HeapGreater
{
  bool operator()(const HeapElem& a, const HeapElem& b) const noexcept
  {
    if (a.first != b.first)
    {
      return a.first > b.first;
    }
    return a.second > b.second;
  }
};
}  // namespace

PeakAggregator::PeakAggregator(std::vector<int64_t> window_ns_list, std::size_t top_n,
                               std::size_t oversample_factor,
                               AggregatorEventFilter event_filter,
                               std::vector<uint32_t> symbol_filter)
    : _top_n(top_n),
      _candidate_budget(top_n * std::max<std::size_t>(oversample_factor, 1)),
      _event_filter(event_filter),
      _symbol_filter(std::move(symbol_filter))
{
  if (window_ns_list.empty())
  {
    throw std::invalid_argument("PeakAggregator: window_ns_list must be non-empty");
  }
  if (top_n == 0)
  {
    throw std::invalid_argument("PeakAggregator: top_n must be > 0");
  }
  for (int64_t w : window_ns_list)
  {
    if (w <= 0)
    {
      throw std::invalid_argument("PeakAggregator: window_ns must be > 0");
    }
    _scales.push_back(Scale{w, {}, {}});
  }
  std::sort(_symbol_filter.begin(), _symbol_filter.end());
  _symbol_filter.erase(std::unique(_symbol_filter.begin(), _symbol_filter.end()),
                       _symbol_filter.end());
}

bool PeakAggregator::symbolAllowed(uint32_t symbol_id) const noexcept
{
  if (_symbol_filter.empty())
  {
    return true;
  }
  return std::binary_search(_symbol_filter.begin(), _symbol_filter.end(), symbol_id);
}

void PeakAggregator::onEvent(const ReplayEvent& ev)
{
  // Peaks are a trade-only metric — book snapshots are typically at a
  // fixed rate so "peak windows" aren't meaningful for them.
  if (ev.type != EventType::Trade)
  {
    return;
  }
  if (_event_filter == AggregatorEventFilter::BooksOnly)
  {
    return;
  }
  if (!symbolAllowed(ev.trade.symbol_id))
  {
    return;
  }

  const int64_t t = ev.timestamp_ns;

  for (auto& scale : _scales)
  {
    // Sliding window pruning: drop timestamps no longer within w of
    // the latest event. Window is (t - w, t] — half-open on the left
    // so a window of width exactly w admits exactly the events whose
    // timestamps lie in that range.
    while (!scale.sliding.empty() && scale.sliding.front() <= t - scale.window_ns)
    {
      scale.sliding.pop_front();
    }
    scale.sliding.push_back(t);

    const uint64_t count = scale.sliding.size();
    const int64_t start_ns = t - scale.window_ns;

    HeapElem candidate{count, start_ns};
    if (scale.heap.size() < _candidate_budget)
    {
      scale.heap.push_back(candidate);
      std::push_heap(scale.heap.begin(), scale.heap.end(), HeapGreater{});
    }
    else if (HeapGreater{}(scale.heap.front(), candidate))
    {
      // candidate beats the current minimum — replace.
      std::pop_heap(scale.heap.begin(), scale.heap.end(), HeapGreater{});
      scale.heap.back() = candidate;
      std::push_heap(scale.heap.begin(), scale.heap.end(), HeapGreater{});
    }
  }
}

void PeakAggregator::finalize()
{
  _rows.clear();

  for (auto& scale : _scales)
  {
    // Sort heap descending by count (ties broken by ascending start_ns
    // — earlier peak wins on a tie, matching the Python prototype's
    // stable-sort semantics).
    std::vector<HeapElem>& cands = scale.heap;
    std::sort(cands.begin(), cands.end(),
              [](const HeapElem& a, const HeapElem& b)
              {
                if (a.first != b.first)
                {
                  return a.first > b.first;
                }
                return a.second < b.second;
              });

    // Dedup: skip any peak within 3*window_ns of a peak already kept.
    // Matches `dedup_peaks(cs, w * 3)` in the Python prototype.
    const int64_t suppress = 3 * scale.window_ns;
    std::vector<HeapElem> kept;
    kept.reserve(_top_n);
    for (const auto& c : cands)
    {
      bool dominated = false;
      for (const auto& k : kept)
      {
        if (std::llabs(c.second - k.second) <= suppress)
        {
          dominated = true;
          break;
        }
      }
      if (!dominated)
      {
        kept.push_back(c);
        if (kept.size() >= _top_n)
        {
          break;
        }
      }
    }

    for (const auto& k : kept)
    {
      _rows.push_back(Row{scale.window_ns, k.first, k.second});
    }

    // Drop the per-scale sliding window after finalisation — the
    // aggregator's result vector is the public surface.
    scale.sliding.clear();
    scale.heap.clear();
  }
}

}  // namespace flox::replay
