/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/aggregators/event_type_stats.h"

#include <algorithm>

namespace flox::replay
{

EventTypeStatsAggregator::EventTypeStatsAggregator(AggregatorEventFilter event_filter,
                                                   std::vector<uint32_t> symbol_filter)
    : _event_filter(event_filter), _symbol_filter(std::move(symbol_filter))
{
  // Sort the symbol filter once so symbolAllowed can binary-search
  // instead of linear-scan per event.
  std::sort(_symbol_filter.begin(), _symbol_filter.end());
  _symbol_filter.erase(std::unique(_symbol_filter.begin(), _symbol_filter.end()),
                       _symbol_filter.end());
}

bool EventTypeStatsAggregator::symbolAllowed(uint32_t symbol_id) const noexcept
{
  if (_symbol_filter.empty())
  {
    return true;
  }
  return std::binary_search(_symbol_filter.begin(), _symbol_filter.end(), symbol_id);
}

void EventTypeStatsAggregator::onEvent(const ReplayEvent& ev)
{
  uint32_t symbol_id = 0;
  bool is_trade = false;
  bool is_snapshot = false;
  bool is_delta = false;

  switch (ev.type)
  {
    case EventType::Trade:
      if (_event_filter == AggregatorEventFilter::BooksOnly)
      {
        return;
      }
      symbol_id = ev.trade.symbol_id;
      is_trade = true;
      break;
    case EventType::BookSnapshot:
      if (_event_filter == AggregatorEventFilter::Trades)
      {
        return;
      }
      symbol_id = ev.book_header.symbol_id;
      is_snapshot = true;
      break;
    case EventType::BookDelta:
      if (_event_filter == AggregatorEventFilter::Trades)
      {
        return;
      }
      symbol_id = ev.book_header.symbol_id;
      is_delta = true;
      break;
    default:
      return;
  }

  if (!symbolAllowed(symbol_id))
  {
    return;
  }

  auto& row = _counts[symbol_id];
  row.symbol_id = symbol_id;
  if (is_trade)
  {
    ++row.trades;
  }
  else if (is_snapshot)
  {
    ++row.book_snapshots;
  }
  else if (is_delta)
  {
    ++row.book_deltas;
  }
}

void EventTypeStatsAggregator::finalize()
{
  _rows.clear();
  _rows.reserve(_counts.size());
  for (const auto& [sid, row] : _counts)
  {
    _rows.push_back(row);
  }
  std::sort(_rows.begin(), _rows.end(),
            [](const PerSymbolRow& a, const PerSymbolRow& b)
            { return a.symbol_id < b.symbol_id; });
}

}  // namespace flox::replay
