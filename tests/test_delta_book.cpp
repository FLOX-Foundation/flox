/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/delta_book.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

using flox::replay::BookLevel;
using flox::replay::DeltaBookEncoder;
using flox::replay::DeltaBookReplayer;

namespace
{

BookLevel level(int64_t price, int64_t qty)
{
  BookLevel l{};
  l.price_raw = price;
  l.qty_raw = qty;
  return l;
}

std::vector<std::pair<int64_t, int64_t>> normalize(const std::vector<BookLevel>& side)
{
  std::vector<std::pair<int64_t, int64_t>> out;
  for (const auto& l : side)
  {
    out.emplace_back(l.price_raw, l.qty_raw);
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

TEST(DeltaBookReplayerTest, SnapshotThenDeltaRoundTrips)
{
  DeltaBookEncoder enc(100);
  DeltaBookReplayer rep;

  const std::vector<std::pair<std::vector<BookLevel>, std::vector<BookLevel>>> steps = {
      {{level(10000, 10), level(9999, 5)}, {level(10001, 8)}},
      {{level(10000, 12)}, {level(10001, 8), level(10002, 3)}},
      {{level(10000, 12), level(9998, 1)}, {level(10001, 7)}},
  };

  for (const auto& [bids, asks] : steps)
  {
    const auto ev = enc.encode(1, bids, asks);
    const auto snap = rep.apply(ev.is_delta ? 1 : 0, 1, ev.bids, ev.asks);
    EXPECT_TRUE(snap.anchored);
    EXPECT_EQ(normalize(snap.bids), normalize(bids));
    EXPECT_EQ(normalize(snap.asks), normalize(asks));
  }
}

// Seeking into the middle of a tape hands the replayer a delta with no anchor
// behind it. It used to merge that delta into empty state and hand back the
// result as a full book, with no way for the caller to tell the difference.
TEST(DeltaBookReplayerTest, DeltaBeforeAnyAnchorIsRefused)
{
  DeltaBookReplayer rep;

  const auto snap = rep.apply(1, 7, {level(10000, 5)}, {level(10001, 3)});

  EXPECT_FALSE(snap.anchored);
  EXPECT_TRUE(snap.bids.empty());
  EXPECT_TRUE(snap.asks.empty());

  const auto second = rep.apply(1, 7, {level(9999, 2)}, {});
  EXPECT_FALSE(second.anchored);
  EXPECT_TRUE(second.bids.empty());
}

TEST(DeltaBookReplayerTest, AnchorRecoversAfterRefusedDeltas)
{
  DeltaBookReplayer rep;

  rep.apply(1, 7, {level(10000, 5)}, {level(10001, 3)});

  const auto anchored = rep.apply(0, 7, {level(20000, 4)}, {level(20001, 6)});
  ASSERT_TRUE(anchored.anchored);
  EXPECT_EQ(normalize(anchored.bids), normalize({level(20000, 4)}));
  EXPECT_EQ(normalize(anchored.asks), normalize({level(20001, 6)}));

  const auto merged = rep.apply(1, 7, {level(19999, 1)}, {});
  EXPECT_TRUE(merged.anchored);
  EXPECT_EQ(merged.bids.size(), 2u);
}

TEST(DeltaBookReplayerTest, ResetDropsTheAnchor)
{
  DeltaBookReplayer rep;

  rep.apply(0, 3, {level(10000, 5)}, {level(10001, 3)});
  ASSERT_TRUE(rep.apply(1, 3, {level(9999, 1)}, {}).anchored);

  rep.reset(3);
  const auto afterReset = rep.apply(1, 3, {level(9999, 1)}, {});
  EXPECT_FALSE(afterReset.anchored);
  EXPECT_TRUE(afterReset.bids.empty());

  rep.apply(0, 3, {level(10000, 5)}, {});
  ASSERT_TRUE(rep.apply(1, 3, {level(9999, 1)}, {}).anchored);

  rep.resetAll();
  EXPECT_FALSE(rep.apply(1, 3, {level(9999, 1)}, {}).anchored);
}

TEST(DeltaBookReplayerTest, SymbolsAnchorIndependently)
{
  DeltaBookReplayer rep;

  rep.apply(0, 1, {level(10000, 5)}, {level(10001, 3)});

  EXPECT_TRUE(rep.apply(1, 1, {level(9999, 2)}, {}).anchored);
  EXPECT_FALSE(rep.apply(1, 2, {level(9999, 2)}, {}).anchored);
}

// An anchored book still swallows a removal for a price it does not hold; that
// is a no-op by design and must not disturb the rest of the side.
TEST(DeltaBookReplayerTest, RemovingAnUnknownLevelLeavesTheBookIntact)
{
  DeltaBookReplayer rep;

  rep.apply(0, 1, {level(10000, 5), level(9999, 4)}, {level(10001, 3)});
  const auto snap = rep.apply(1, 1, {level(5000, 0)}, {});

  EXPECT_TRUE(snap.anchored);
  EXPECT_EQ(snap.bids.size(), 2u);
  EXPECT_EQ(snap.asks.size(), 1u);
}
