/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/aggregators/quantile.h"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <vector>

namespace flox::replay
{

QuantileAggregator::QuantileAggregator(std::vector<int64_t> window_ns_list,
                                       std::vector<double> quantiles,
                                       AggregatorEventFilter event_filter,
                                       std::vector<uint32_t> symbol_filter)
    : _quantiles(std::move(quantiles)),
      _event_filter(event_filter),
      _symbol_filter(std::move(symbol_filter))
{
  if (window_ns_list.empty())
  {
    throw std::invalid_argument(
        "QuantileAggregator: window_ns_list must be non-empty");
  }
  if (_quantiles.empty())
  {
    throw std::invalid_argument("QuantileAggregator: quantiles must be non-empty");
  }
  for (int64_t w : window_ns_list)
  {
    if (w <= 0)
    {
      throw std::invalid_argument("QuantileAggregator: window_ns must be > 0");
    }
    _scales.push_back(Scale{w, {}, {}, 0});
  }
  for (double q : _quantiles)
  {
    if (q <= 0.0 || q > 1.0)
    {
      throw std::invalid_argument(
          "QuantileAggregator: quantile must be in (0.0, 1.0]");
    }
  }
  std::sort(_quantiles.begin(), _quantiles.end());
  _quantiles.erase(std::unique(_quantiles.begin(), _quantiles.end()),
                   _quantiles.end());
  std::sort(_symbol_filter.begin(), _symbol_filter.end());
  _symbol_filter.erase(std::unique(_symbol_filter.begin(), _symbol_filter.end()),
                       _symbol_filter.end());
}

bool QuantileAggregator::symbolAllowed(uint32_t symbol_id) const noexcept
{
  if (_symbol_filter.empty())
  {
    return true;
  }
  return std::binary_search(_symbol_filter.begin(), _symbol_filter.end(), symbol_id);
}

void QuantileAggregator::onEvent(const ReplayEvent& ev)
{
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
    while (!scale.sliding.empty() && scale.sliding.front() <= t - scale.window_ns)
    {
      scale.sliding.pop_front();
    }
    scale.sliding.push_back(t);

    const uint64_t window_count = scale.sliding.size();
    ++scale.histogram[window_count];
    ++scale.total_observations;
  }
}

void QuantileAggregator::finalize()
{
  _rows.clear();
  _rows.reserve(_scales.size() * _quantiles.size());

  for (auto& scale : _scales)
  {
    if (scale.total_observations == 0)
    {
      // No observations for this scale (e.g. event_filter excluded
      // everything). Emit zero-count rows so result shape is stable.
      for (double q : _quantiles)
      {
        _rows.push_back(Row{scale.window_ns, q, 0});
      }
      continue;
    }

    // Move the histogram into a sorted vector for cumulative scan.
    std::vector<std::pair<uint64_t, uint64_t>> sorted;
    sorted.reserve(scale.histogram.size());
    for (const auto& [k, v] : scale.histogram)
    {
      sorted.emplace_back(k, v);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b)
              { return a.first < b.first; });

    // For each quantile (already sorted ascending), find the smallest
    // count value C such that the cumulative fraction of observations
    // with window_count ≤ C reaches q. Walk the histogram once and
    // emit quantiles in order — both inputs are sorted, so each
    // quantile resumes from where the previous left off.
    std::size_t idx = 0;
    uint64_t cum = 0;
    const auto N = static_cast<double>(scale.total_observations);

    for (double q : _quantiles)
    {
      const auto target = static_cast<uint64_t>(
          std::min<double>(N, std::max<double>(1.0, q * N)));
      while (idx < sorted.size())
      {
        if (cum + sorted[idx].second >= target)
        {
          break;
        }
        cum += sorted[idx].second;
        ++idx;
      }
      // sorted[idx] holds the count value containing the target rank.
      // Fallback to the highest observed count if we ran off the end
      // (shouldn't happen given target ≤ N but is defensive).
      const uint64_t threshold =
          idx < sorted.size() ? sorted[idx].first : sorted.back().first;
      _rows.push_back(Row{scale.window_ns, q, threshold});
    }

    scale.sliding.clear();
    scale.histogram.clear();
  }
}

std::unique_ptr<IAggregator> QuantileAggregator::cloneEmpty() const
{
  std::vector<int64_t> windows;
  windows.reserve(_scales.size());
  for (const auto& s : _scales)
  {
    windows.push_back(s.window_ns);
  }
  return std::make_unique<QuantileAggregator>(std::move(windows), _quantiles,
                                              _event_filter, _symbol_filter);
}

void QuantileAggregator::merge(const IAggregator& other)
{
  const auto* o = dynamic_cast<const QuantileAggregator*>(&other);
  if (o == nullptr)
  {
    throw std::invalid_argument(
        "QuantileAggregator::merge: other is not the same concrete type");
  }
  if (o->_scales.size() != _scales.size())
  {
    throw std::invalid_argument(
        "QuantileAggregator::merge: scale count differs (other has different "
        "window_ns_list)");
  }
  // cloneEmpty() preserves window_ns ordering — pair scales positionally
  // and add the other side's histogram counters in.
  for (size_t i = 0; i < _scales.size(); ++i)
  {
    auto& dst = _scales[i];
    const auto& src = o->_scales[i];
    if (src.window_ns != dst.window_ns)
    {
      throw std::invalid_argument(
          "QuantileAggregator::merge: window_ns mismatch at scale index " +
          std::to_string(i));
    }
    for (const auto& [count_value, occurrences] : src.histogram)
    {
      dst.histogram[count_value] += occurrences;
    }
    dst.total_observations += src.total_observations;
  }
}

}  // namespace flox::replay
