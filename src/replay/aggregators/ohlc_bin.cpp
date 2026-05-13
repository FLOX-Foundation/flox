/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/aggregators/ohlc_bin.h"

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace flox::replay
{

namespace
{
int64_t bucketStart(int64_t ts_ns, int64_t bucket_ns)
{
  if (ts_ns >= 0)
  {
    return (ts_ns / bucket_ns) * bucket_ns;
  }
  int64_t q = ts_ns / bucket_ns;
  if (ts_ns % bucket_ns != 0)
  {
    --q;
  }
  return q * bucket_ns;
}
}  // namespace

OHLCBinAggregator::OHLCBinAggregator(int64_t bucket_ns, bool by_symbol,
                                     AggregatorEventFilter event_filter,
                                     std::vector<uint32_t> symbol_filter)
    : _bucket_ns(bucket_ns),
      _by_symbol(by_symbol),
      _event_filter(event_filter),
      _symbol_filter(std::move(symbol_filter))
{
  if (bucket_ns <= 0)
  {
    throw std::invalid_argument("OHLCBinAggregator: bucket_ns must be > 0");
  }
  std::sort(_symbol_filter.begin(), _symbol_filter.end());
  _symbol_filter.erase(std::unique(_symbol_filter.begin(), _symbol_filter.end()),
                       _symbol_filter.end());
}

bool OHLCBinAggregator::symbolAllowed(uint32_t symbol_id) const noexcept
{
  if (_symbol_filter.empty())
  {
    return true;
  }
  return std::binary_search(_symbol_filter.begin(), _symbol_filter.end(), symbol_id);
}

void OHLCBinAggregator::onEvent(const ReplayEvent& ev)
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

  const int64_t bucket = bucketStart(ev.timestamp_ns, _bucket_ns);
  const uint32_t key_sym = _by_symbol ? ev.trade.symbol_id : 0u;
  const int64_t price = ev.trade.price_raw;
  const int64_t ts = ev.timestamp_ns;

  auto [it, inserted] = _cells.try_emplace(std::tuple<int64_t, uint32_t>{bucket, key_sym});
  Cell& c = it->second;
  if (inserted)
  {
    c.open_ts_ns = ts;
    c.close_ts_ns = ts;
    c.open_raw = price;
    c.close_raw = price;
    c.high_raw = price;
    c.low_raw = price;
    return;
  }
  // Open: earliest ts wins. Close: latest ts wins. Hi/Lo: extrema.
  // Cross-block reorder buffer makes events arrive in ts order within
  // a worker, but explicit ts comparison keeps merge() and any future
  // alternative dispatcher correct.
  if (ts < c.open_ts_ns)
  {
    c.open_ts_ns = ts;
    c.open_raw = price;
  }
  if (ts > c.close_ts_ns)
  {
    c.close_ts_ns = ts;
    c.close_raw = price;
  }
  if (price > c.high_raw)
  {
    c.high_raw = price;
  }
  if (price < c.low_raw)
  {
    c.low_raw = price;
  }
}

void OHLCBinAggregator::finalize()
{
  _rows.clear();
  _rows.reserve(_cells.size());
  for (const auto& [key, c] : _cells)
  {
    Row r;
    r.bucket_ts_ns = std::get<0>(key);
    r.symbol_id = std::get<1>(key);
    r.open_raw = c.open_raw;
    r.high_raw = c.high_raw;
    r.low_raw = c.low_raw;
    r.close_raw = c.close_raw;
    _rows.push_back(r);
  }
}

std::unique_ptr<IAggregator> OHLCBinAggregator::cloneEmpty() const
{
  return std::make_unique<OHLCBinAggregator>(_bucket_ns, _by_symbol, _event_filter,
                                             _symbol_filter);
}

void OHLCBinAggregator::merge(const IAggregator& other)
{
  const auto* o = dynamic_cast<const OHLCBinAggregator*>(&other);
  if (o == nullptr)
  {
    throw std::invalid_argument(
        "OHLCBinAggregator::merge: other is not the same concrete type");
  }
  for (const auto& [key, src] : o->_cells)
  {
    auto [it, inserted] = _cells.try_emplace(key, src);
    if (inserted)
    {
      continue;
    }
    Cell& dst = it->second;
    if (src.open_ts_ns < dst.open_ts_ns)
    {
      dst.open_ts_ns = src.open_ts_ns;
      dst.open_raw = src.open_raw;
    }
    if (src.close_ts_ns > dst.close_ts_ns)
    {
      dst.close_ts_ns = src.close_ts_ns;
      dst.close_raw = src.close_raw;
    }
    if (src.high_raw > dst.high_raw)
    {
      dst.high_raw = src.high_raw;
    }
    if (src.low_raw < dst.low_raw)
    {
      dst.low_raw = src.low_raw;
    }
  }
}

}  // namespace flox::replay
