/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/tape_diff.h"

#include "flox/replay/readers/binary_log_reader.h"

#include <algorithm>
#include <cstdlib>

namespace flox::replay
{

namespace
{

void readTrades(const std::filesystem::path& dir, std::vector<TapeDiffTrade>& out)
{
  ReaderConfig config{};
  config.data_dir = dir;
  BinaryLogReader reader(config);

  reader.forEach([&out](const ReplayEvent& ev)
                 {
    if (ev.type == EventType::Trade)
    {
      TapeDiffTrade t{};
      t.exchange_ts_ns = ev.trade.exchange_ts_ns;
      t.price_raw = ev.trade.price_raw;
      t.qty_raw = ev.trade.qty_raw;
      t.symbol_id = ev.trade.symbol_id;
      t.side = ev.trade.side;
      out.push_back(t);
    }
    return true; });
}

bool sameRecord(const TapeDiffTrade& a, const TapeDiffTrade& b, int64_t tol)
{
  if (std::llabs(a.exchange_ts_ns - b.exchange_ts_ns) > tol)
  {
    return false;
  }
  return a.symbol_id == b.symbol_id &&
         a.price_raw == b.price_raw &&
         a.qty_raw == b.qty_raw &&
         a.side == b.side;
}

}  // namespace

TapeDiffResult diffTapes(const std::filesystem::path& left,
                         const std::filesystem::path& right,
                         const TapeDiffOptions& opts)
{
  TapeDiffResult result;
  result.left_path = left.string();
  result.right_path = right.string();

  std::vector<TapeDiffTrade> lt;
  std::vector<TapeDiffTrade> rt;
  readTrades(left, lt);
  readTrades(right, rt);

  result.left_count = lt.size();
  result.right_count = rt.size();

  const uint64_t n = std::min<uint64_t>(lt.size(), rt.size());
  const uint32_t cap = opts.max_mismatches;

  for (uint64_t i = 0; i < n; ++i)
  {
    if (!sameRecord(lt[i], rt[i], opts.field_tolerance_ns))
    {
      if (!result.first_divergence_index.has_value())
      {
        result.first_divergence_index = i;
      }
      if (cap == 0 || result.mismatches.size() < cap)
      {
        result.mismatches.push_back(TapeDiffMismatch{i, lt[i], rt[i]});
      }
    }
  }

  if (result.left_count != result.right_count && !result.first_divergence_index.has_value())
  {
    result.first_divergence_index = n;
  }

  result.equal = (result.left_count == result.right_count) &&
                 !result.first_divergence_index.has_value();
  return result;
}

}  // namespace flox::replay
