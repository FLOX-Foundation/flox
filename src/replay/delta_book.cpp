/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/delta_book.h"

#include <algorithm>

namespace flox::replay
{

DeltaBookEncoder::DeltaBookEncoder(uint32_t anchor_every) : _anchor_every(anchor_every) {}

void DeltaBookEncoder::reset(uint32_t symbol_id) { _state.erase(symbol_id); }
void DeltaBookEncoder::resetAll() { _state.clear(); }

void DeltaBookEncoder::applySide(std::map<int64_t, int64_t>& state,
                                 const std::vector<BookLevel>& levels)
{
  state.clear();
  for (const auto& l : levels)
  {
    if (l.qty_raw > 0)
    {
      state[l.price_raw] = l.qty_raw;
    }
  }
}

std::vector<BookLevel> DeltaBookEncoder::diffSide(const std::map<int64_t, int64_t>& prev,
                                                  const std::vector<BookLevel>& current)
{
  std::map<int64_t, int64_t> cur_map;
  for (const auto& l : current)
  {
    if (l.qty_raw > 0)
    {
      cur_map[l.price_raw] = l.qty_raw;
    }
  }

  std::vector<BookLevel> out;
  // Removed prices: present in prev, absent in cur.
  for (const auto& [price, _] : prev)
  {
    if (cur_map.find(price) == cur_map.end())
    {
      BookLevel l{};
      l.price_raw = price;
      l.qty_raw = 0;
      out.push_back(l);
    }
  }
  // Added or changed prices.
  for (const auto& [price, qty] : cur_map)
  {
    auto it = prev.find(price);
    if (it == prev.end() || it->second != qty)
    {
      BookLevel l{};
      l.price_raw = price;
      l.qty_raw = qty;
      out.push_back(l);
    }
  }
  return out;
}

DeltaBookEncoder::Output DeltaBookEncoder::encode(uint32_t symbol_id,
                                                  const std::vector<BookLevel>& bids,
                                                  const std::vector<BookLevel>& asks)
{
  auto& state = _state[symbol_id];

  const bool first = state.bid_levels.empty() && state.ask_levels.empty();
  const bool due_anchor = (_anchor_every == 0) ||
                          (state.since_anchor + 1 >= _anchor_every);

  Output out;
  if (first || due_anchor)
  {
    out.is_delta = false;
    out.bids = bids;
    out.asks = asks;
    applySide(state.bid_levels, bids);
    applySide(state.ask_levels, asks);
    state.since_anchor = 0;
    return out;
  }

  out.is_delta = true;
  out.bids = diffSide(state.bid_levels, bids);
  out.asks = diffSide(state.ask_levels, asks);
  applySide(state.bid_levels, bids);
  applySide(state.ask_levels, asks);
  state.since_anchor += 1;
  return out;
}

// ── Replayer ─────────────────────────────────────────────────────

std::vector<BookLevel> DeltaBookReplayer::dumpSide(const std::map<int64_t, int64_t>& state,
                                                   bool descending)
{
  std::vector<BookLevel> out;
  out.reserve(state.size());
  for (const auto& [price, qty] : state)
  {
    BookLevel l{};
    l.price_raw = price;
    l.qty_raw = qty;
    out.push_back(l);
  }
  if (descending)
  {
    std::sort(out.begin(), out.end(),
              [](const BookLevel& a, const BookLevel& b)
              { return a.price_raw > b.price_raw; });
  }
  return out;
}

DeltaBookReplayer::Snapshot DeltaBookReplayer::apply(uint8_t type, uint32_t symbol_id,
                                                     const std::vector<BookLevel>& bids,
                                                     const std::vector<BookLevel>& asks)
{
  auto& state = _state[symbol_id];
  if (type == 0)
  {
    state.bid_levels.clear();
    state.ask_levels.clear();
    for (const auto& l : bids)
    {
      if (l.qty_raw > 0)
      {
        state.bid_levels[l.price_raw] = l.qty_raw;
      }
    }
    for (const auto& l : asks)
    {
      if (l.qty_raw > 0)
      {
        state.ask_levels[l.price_raw] = l.qty_raw;
      }
    }
  }
  else
  {
    auto merge = [](std::map<int64_t, int64_t>& side, const std::vector<BookLevel>& levels)
    {
      for (const auto& l : levels)
      {
        if (l.qty_raw == 0)
        {
          side.erase(l.price_raw);
        }
        else
        {
          side[l.price_raw] = l.qty_raw;
        }
      }
    };
    merge(state.bid_levels, bids);
    merge(state.ask_levels, asks);
  }

  Snapshot s;
  s.bids = dumpSide(state.bid_levels, /*descending=*/true);
  s.asks = dumpSide(state.ask_levels, /*descending=*/false);
  return s;
}

void DeltaBookReplayer::reset(uint32_t symbol_id) { _state.erase(symbol_id); }
void DeltaBookReplayer::resetAll() { _state.clear(); }

}  // namespace flox::replay
