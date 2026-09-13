/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Differential test: drive NLevelOrderBook and a std::map reference model with
 * the same random delta stream and require every reader to agree. The book
 * trades a flat tick-indexed array for O(1) lookups, and the bookkeeping that
 * buys is where its defects live, so a model that cannot have index bugs is the
 * cheapest guard against a regression in the core.
 */

#include "flox/book/events/book_update_event.h"
#include "flox/book/nlevel_order_book.h"
#include "flox/common.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory_resource>
#include <random>
#include <vector>

using namespace flox;

namespace
{

constexpr size_t kLevels = 512;
constexpr int64_t kTickRaw = 1000000;  // 0.01 at a price scale of 1e8
constexpr int64_t kMidTick = 1000000;  // 10000.00
constexpr int64_t kSpan = 200;         // stay inside the anchored window
constexpr int kDeltas = 300000;

using ModelSide = std::map<int64_t, int64_t>;  // tick index -> raw quantity

int64_t modelBest(const ModelSide& side, bool highest)
{
  if (side.empty())
  {
    return -1;
  }
  return highest ? side.rbegin()->first : side.begin()->first;
}

struct Consumed
{
  int64_t filledRaw{0};
  int64_t notionalRaw{0};
};

Consumed modelConsume(const ModelSide& side, int64_t needRaw, bool fromLowest)
{
  Consumed out;
#if defined(__SIZEOF_INT128__) && !defined(_MSC_VER)
  __int128_t notional = 0;
#endif
  int64_t rem = needRaw;

  auto take = [&](int64_t tick, int64_t qty)
  {
    if (rem <= 0)
    {
      return;
    }
    const int64_t t = (qty < rem) ? qty : rem;
#if defined(__SIZEOF_INT128__) && !defined(_MSC_VER)
    notional += static_cast<__int128_t>(t) * static_cast<__int128_t>(kTickRaw * tick);
#endif
    rem -= t;
  };

  if (fromLowest)
  {
    for (auto it = side.begin(); it != side.end(); ++it)
    {
      take(it->first, it->second);
    }
  }
  else
  {
    for (auto it = side.rbegin(); it != side.rend(); ++it)
    {
      take(it->first, it->second);
    }
  }

  out.filledRaw = needRaw - rem;
#if defined(__SIZEOF_INT128__) && !defined(_MSC_VER)
  out.notionalRaw = static_cast<int64_t>(notional / Volume::Scale);
#endif
  return out;
}

int64_t depthRaw(const ModelSide& side)
{
  int64_t total = 0;
  for (const auto& [tick, qty] : side)
  {
    total += qty;
  }
  return total;
}

}  // namespace

TEST(NLevelOrderBookModel, MatchesReferenceModelOverRandomDeltas)
{
  NLevelOrderBook<kLevels> book{Price::fromRaw(kTickRaw)};
  ModelSide bidModel;
  ModelSide askModel;

  std::byte buf[8192];
  std::pmr::monotonic_buffer_resource res(buf, sizeof(buf));
  BookUpdateEvent ev(&res);

  // Anchor the window with a snapshot, exactly as a live feed does on connect.
  ev.update.type = BookUpdateType::SNAPSHOT;
  for (int64_t k = 0; k < 5; ++k)
  {
    const int64_t bidTick = kMidTick - 1 - k;
    const int64_t askTick = kMidTick + 1 + k;
    const int64_t qty = Quantity::fromDouble(1.0 + static_cast<double>(k)).raw();
    ev.update.bids.push_back({Price::fromRaw(bidTick * kTickRaw), Quantity::fromRaw(qty)});
    ev.update.asks.push_back({Price::fromRaw(askTick * kTickRaw), Quantity::fromRaw(qty)});
    bidModel[bidTick] = qty;
    askModel[askTick] = qty;
  }
  book.applyBookUpdate(ev);

  std::mt19937_64 rng(0x5EEDB05Full);
  std::uniform_int_distribution<int> sideDist(0, 1);
  std::uniform_int_distribution<int> countDist(1, 3);
  std::uniform_int_distribution<int64_t> offsetDist(1, kSpan);
  std::uniform_int_distribution<int> removeDist(0, 3);         // one delta level in four removes
  std::uniform_int_distribution<int64_t> qtyDist(1, 5000000);  // raw quantity
  std::uniform_int_distribution<int> consumeDist(0, 63);

  for (int step = 0; step < kDeltas; ++step)
  {
    ev.update.type = BookUpdateType::DELTA;
    ev.update.bids.clear();
    ev.update.asks.clear();

    const int levels = countDist(rng);
    for (int i = 0; i < levels; ++i)
    {
      const bool isBid = sideDist(rng) == 0;
      ModelSide& model = isBid ? bidModel : askModel;
      const int64_t tick = isBid ? (kMidTick - offsetDist(rng)) : (kMidTick + offsetDist(rng));
      const bool remove = removeDist(rng) == 0;
      int64_t qty = remove ? 0 : qtyDist(rng);

      // Both sides stay populated: an empty side is its own scenario, covered
      // by the targeted cases in test_nlevel_order_book_window.cpp.
      if (qty == 0 && model.size() == 1 && model.count(tick) == 1)
      {
        qty = 1000000;
      }

      if (qty == 0)
      {
        model.erase(tick);
      }
      else
      {
        model[tick] = qty;
      }

      const BookLevel level{Price::fromRaw(tick * kTickRaw), Quantity::fromRaw(qty)};
      if (isBid)
      {
        ev.update.bids.push_back(level);
      }
      else
      {
        ev.update.asks.push_back(level);
      }
    }

    book.applyBookUpdate(ev);

    const int64_t wantBid = modelBest(bidModel, /*highest=*/true);
    const int64_t wantAsk = modelBest(askModel, /*highest=*/false);

    const auto gotBid = book.bestBid();
    const auto gotAsk = book.bestAsk();
    ASSERT_TRUE(gotBid.has_value()) << "step " << step;
    ASSERT_TRUE(gotAsk.has_value()) << "step " << step;
    ASSERT_EQ(gotBid->raw(), wantBid * kTickRaw) << "step " << step;
    ASSERT_EQ(gotAsk->raw(), wantAsk * kTickRaw) << "step " << step;
    ASSERT_EQ(book.spread()->raw(), (wantAsk - wantBid) * kTickRaw) << "step " << step;
    ASSERT_EQ(book.mid()->raw(), (wantBid + wantAsk) * kTickRaw / 2) << "step " << step;
    ASSERT_FALSE(book.isCrossed()) << "step " << step;

    if (consumeDist(rng) == 0)
    {
      const int64_t askDepth = depthRaw(askModel);
      const int64_t bidDepth = depthRaw(bidModel);
      const int64_t wantQty = askDepth / 2;
      const auto [filled, notional] = book.consumeAsks(Quantity::fromRaw(wantQty));
      const auto want = modelConsume(askModel, wantQty, /*fromLowest=*/true);
      ASSERT_EQ(filled.raw(), want.filledRaw) << "step " << step;
#if defined(__SIZEOF_INT128__) && !defined(_MSC_VER)
      ASSERT_EQ(notional.raw(), want.notionalRaw) << "step " << step;
#else
      (void)notional;
#endif

      const int64_t wantBidQty = bidDepth / 2;
      const auto [bfilled, bnotional] = book.consumeBids(Quantity::fromRaw(wantBidQty));
      const auto bwant = modelConsume(bidModel, wantBidQty, /*fromLowest=*/false);
      ASSERT_EQ(bfilled.raw(), bwant.filledRaw) << "step " << step;
#if defined(__SIZEOF_INT128__) && !defined(_MSC_VER)
      ASSERT_EQ(bnotional.raw(), bwant.notionalRaw) << "step " << step;
#else
      (void)bnotional;
#endif

      const int64_t probe = kMidTick - offsetDist(rng);
      const auto it = bidModel.find(probe);
      const int64_t wantAt = (it == bidModel.end()) ? 0 : it->second;
      ASSERT_EQ(book.bidAtPrice(Price::fromRaw(probe * kTickRaw)).raw(), wantAt) << "step " << step;
    }
  }
}
