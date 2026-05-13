/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/aggregators/volume_bin.h"

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

VolumeBinAggregator::VolumeBinAggregator(int64_t bucket_ns, bool by_side, bool by_symbol,
                                         AggregatorEventFilter event_filter,
                                         std::vector<uint32_t> symbol_filter)
    : _bucket_ns(bucket_ns),
      _by_side(by_side),
      _by_symbol(by_symbol),
      _event_filter(event_filter),
      _symbol_filter(std::move(symbol_filter))
{
  if (bucket_ns <= 0)
  {
    throw std::invalid_argument("VolumeBinAggregator: bucket_ns must be > 0");
  }
  std::sort(_symbol_filter.begin(), _symbol_filter.end());
  _symbol_filter.erase(std::unique(_symbol_filter.begin(), _symbol_filter.end()),
                       _symbol_filter.end());
}

bool VolumeBinAggregator::symbolAllowed(uint32_t symbol_id) const noexcept
{
  if (_symbol_filter.empty())
  {
    return true;
  }
  return std::binary_search(_symbol_filter.begin(), _symbol_filter.end(), symbol_id);
}

void VolumeBinAggregator::onEvent(const ReplayEvent& ev)
{
  // Only trades carry the kind of qty this aggregator sums. The
  // event_filter parameter exists for API parity; passing BooksOnly
  // is a no-op, passing Trades / Both both behave the same here.
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
  // Side encoding in result rows: 0 = aggregate, 1 = BUY, 2 = SELL.
  // Trade.side raw encoding: 0 = BUY, 1 = SELL.
  uint8_t key_side = 0;
  if (_by_side)
  {
    key_side = static_cast<uint8_t>(ev.trade.side == 0 ? 1 : 2);
  }

  _qtys[std::tuple<int64_t, uint32_t, uint8_t>{bucket, key_sym, key_side}] +=
      ev.trade.qty_raw;
}

void VolumeBinAggregator::finalize()
{
  _rows.clear();
  _rows.reserve(_qtys.size());
  for (const auto& [key, qty] : _qtys)
  {
    Row r;
    r.bucket_ts_ns = std::get<0>(key);
    r.symbol_id = std::get<1>(key);
    r.side = std::get<2>(key);
    r.qty_raw = qty;
    _rows.push_back(r);
  }
}

std::unique_ptr<IAggregator> VolumeBinAggregator::cloneEmpty() const
{
  return std::make_unique<VolumeBinAggregator>(_bucket_ns, _by_side, _by_symbol,
                                               _event_filter, _symbol_filter);
}

void VolumeBinAggregator::merge(const IAggregator& other)
{
  const auto* o = dynamic_cast<const VolumeBinAggregator*>(&other);
  if (o == nullptr)
  {
    throw std::invalid_argument(
        "VolumeBinAggregator::merge: other is not the same concrete type");
  }
  for (const auto& [key, qty] : o->_qtys)
  {
    _qtys[key] += qty;
  }
}

}  // namespace flox::replay
