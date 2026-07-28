/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/messages.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace flox::venue
{

class IndexAggregator
{
 public:
  IndexAggregator(int64_t stalenessNs, int32_t maxDeviationBps, size_t minSources)
      : stalenessNs_(stalenessNs), maxDeviationBps_(maxDeviationBps), minSources_(minSources)
  {
  }

  void update(uint32_t sourceId, Price p, int64_t tsNs) { sources_[sourceId] = {p.raw(), tsNs}; }

  bool hasIndex(int64_t nowNs) const { return freshPrices(nowNs).size() >= minSources_; }

  // Median of the fresh, outlier-filtered source set. Returns an invalid Price
  // (raw 0) if fewer than minSources fresh feeds are available.
  Price index(int64_t nowNs) const
  {
    std::vector<int64_t> fresh = freshPrices(nowNs);
    if (fresh.size() < minSources_)
    {
      return Price{};
    }
    const int64_t m0 = median(fresh);
    // Drop sources deviating more than maxDeviationBps from the rough median.
    std::vector<int64_t> kept;
    kept.reserve(fresh.size());
    for (int64_t v : fresh)
    {
      const int64_t dev = (v > m0 ? v - m0 : m0 - v);
      if (m0 == 0 || dev * 10000 / m0 <= maxDeviationBps_)
      {
        kept.push_back(v);
      }
    }
    if (kept.size() < minSources_)
    {
      kept = std::move(fresh);  // all deviate similarly (real move) -> keep them
    }
    return Price::fromRaw(median(kept));
  }

 private:
  struct Src
  {
    int64_t priceRaw;
    int64_t tsNs;
  };

  std::vector<int64_t> freshPrices(int64_t nowNs) const
  {
    std::vector<int64_t> out;
    out.reserve(sources_.size());
    for (const auto& [id, s] : sources_)
    {
      if (nowNs - s.tsNs <= stalenessNs_)
      {
        out.push_back(s.priceRaw);
      }
    }
    return out;
  }

  static int64_t median(std::vector<int64_t>& v)
  {
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    if (n == 0)
    {
      return 0;
    }
    return (n & 1) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
  }

  int64_t stalenessNs_;
  int32_t maxDeviationBps_;
  size_t minSources_;
  std::unordered_map<uint32_t, Src> sources_;
};

// One book level for impact-price computation (price + resting size).
struct DepthLevel
{
  int64_t priceRaw;
  int64_t qtyRaw;
};

// Impact price: the volume-weighted price to fill `targetQtyRaw` walking the
// levels best-first. A thin top-of-book cannot swing it -- you must consume real
// depth to move the impact price. If depth is insufficient, returns the VWAP of
// what is available; 0 if there is no depth at all.
inline int64_t impactPriceRaw(const std::vector<DepthLevel>& levels, int64_t targetQtyRaw)
{
  if (targetQtyRaw <= 0)
  {
    return levels.empty() ? 0 : levels.front().priceRaw;
  }
  __int128 notional = 0;
  int64_t filled = 0;
  for (const auto& lv : levels)
  {
    const int64_t take = std::min<int64_t>(lv.qtyRaw, targetQtyRaw - filled);
    if (take <= 0)
    {
      break;
    }
    notional += static_cast<__int128>(lv.priceRaw) * take;
    filled += take;
    if (filled >= targetQtyRaw)
    {
      break;
    }
  }
  if (filled == 0)
  {
    return 0;
  }
  return static_cast<int64_t>(notional / filled);
}

// Impact mid = midpoint of the impact bid (VWAP into asks) and impact ask
// (VWAP into bids) for a given impact size. Feed this into MarkPrice::setMid.
inline int64_t impactMidRaw(const std::vector<DepthLevel>& bids, const std::vector<DepthLevel>& asks,
                            int64_t impactQtyRaw)
{
  const int64_t ib = impactPriceRaw(bids, impactQtyRaw);
  const int64_t ia = impactPriceRaw(asks, impactQtyRaw);
  if (ib == 0)
  {
    return ia;
  }
  if (ia == 0)
  {
    return ib;
  }
  return (ib + ia) / 2;
}

// Compute the impact mid directly from a live order book. Works with any book
// exposing `levels(Side, std::vector<std::pair<Price, Quantity>>&)` best-first
// (MatchingBook / LadderBook). Feed the result into MarkPrice::setMid.
template <class Book>
int64_t bookImpactMidRaw(const Book& book, int64_t impactQtyRaw)
{
  std::vector<std::pair<Price, Quantity>> bl, al;
  book.levels(Side::BUY, bl);
  book.levels(Side::SELL, al);
  std::vector<DepthLevel> bids, asks;
  bids.reserve(bl.size());
  asks.reserve(al.size());
  for (const auto& [p, q] : bl)
  {
    bids.push_back({p.raw(), q.raw()});
  }
  for (const auto& [p, q] : al)
  {
    asks.push_back({p.raw(), q.raw()});
  }
  return impactMidRaw(bids, asks, impactQtyRaw);
}

class MarkPrice
{
 public:
  explicit MarkPrice(int32_t clampBps) : clampBps_(clampBps) {}

  void setIndex(Price p) { indexRaw_ = p.raw(); }
  void setLast(Price p) { lastRaw_ = p.raw(); }
  void setMid(Price p) { midRaw_ = p.raw(); }

  bool valid() const { return indexRaw_ > 0; }

  // Median of {index, last, mid} (whichever are set), clamped to the index band.
  Price value() const
  {
    if (indexRaw_ <= 0)
    {
      return Price{};
    }
    std::vector<int64_t> pts{indexRaw_};
    if (lastRaw_ > 0)
    {
      pts.push_back(lastRaw_);
    }
    if (midRaw_ > 0)
    {
      pts.push_back(midRaw_);
    }
    std::sort(pts.begin(), pts.end());
    int64_t mark = pts[pts.size() / 2];

    const int64_t band = indexRaw_ * clampBps_ / 10000;
    const int64_t lo = indexRaw_ - band;
    const int64_t hi = indexRaw_ + band;
    if (mark < lo)
    {
      mark = lo;
    }
    if (mark > hi)
    {
      mark = hi;
    }
    return Price::fromRaw(mark);
  }

 private:
  int32_t clampBps_;
  int64_t indexRaw_{0};
  int64_t lastRaw_{0};
  int64_t midRaw_{0};
};

}  // namespace flox::venue
