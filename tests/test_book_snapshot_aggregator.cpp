/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/aggregators/book_snapshot_bin.h"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <vector>

using flox::replay::AggregatorEventFilter;
using flox::replay::BookLevel;
using flox::replay::BookSnapshotBinAggregator;
using flox::replay::EventType;
using flox::replay::ReplayEvent;

namespace
{

constexpr int64_t kBase = 1'700'000'000'000'000'000;
constexpr int64_t kBucket = 60'000'000'000;  // 60s

ReplayEvent bookEvent(int64_t ts_ns, uint32_t symbol_id, bool is_snapshot,
                      std::vector<BookLevel> bids, std::vector<BookLevel> asks)
{
  ReplayEvent ev;
  ev.type = is_snapshot ? EventType::BookSnapshot : EventType::BookDelta;
  ev.timestamp_ns = ts_ns;
  ev.book_header.exchange_ts_ns = ts_ns;
  ev.book_header.symbol_id = symbol_id;
  ev.book_header.bid_count = static_cast<uint16_t>(bids.size());
  ev.book_header.ask_count = static_cast<uint16_t>(asks.size());
  ev.book_header.type = is_snapshot ? 0 : 1;
  ev.bids = std::move(bids);
  ev.asks = std::move(asks);
  return ev;
}

BookLevel lvl(int64_t price_raw, int64_t qty_raw)
{
  return BookLevel{price_raw, qty_raw};
}

}  // namespace

TEST(BookSnapshotAggregatorTest, EmitsLatestStatePerBucket)
{
  BookSnapshotBinAggregator agg(kBucket, /*levels=*/5);
  // Bucket A: snapshot then a delta inside the same bucket.
  agg.onEvent(bookEvent(kBase, 1, true,
                        {lvl(10000, 100), lvl(9900, 200)},
                        {lvl(10100, 150), lvl(10200, 250)}));
  agg.onEvent(bookEvent(kBase + 30'000'000'000, 1, false,
                        {lvl(10000, 50)}, {}));
  // Bucket B (next 60s window): removals.
  agg.onEvent(bookEvent(kBase + kBucket, 1, false,
                        {lvl(9900, 0)}, {lvl(10100, 0)}));
  agg.finalize();

  const auto& rows = agg.result();
  // Bucket A state: bids {10000:50, 9900:200}, asks {10100:150, 10200:250}
  // -> 2 rows. Bucket B (trailing): bids {10000:50}, asks {10200:250} -> 1 row.
  ASSERT_EQ(rows.size(), 3u);

  const int64_t bucket_a = (kBase / kBucket) * kBucket;
  EXPECT_EQ(rows[0].bucket_ts_ns, bucket_a);
  EXPECT_EQ(rows[0].level, 0);
  EXPECT_EQ(rows[0].bid_price_raw, 10000);
  EXPECT_EQ(rows[0].bid_qty_raw, 50);  // delta applied inside bucket A
  EXPECT_EQ(rows[0].ask_price_raw, 10100);
  EXPECT_EQ(rows[0].ask_qty_raw, 150);
  EXPECT_EQ(rows[1].level, 1);
  EXPECT_EQ(rows[1].bid_price_raw, 9900);
  EXPECT_EQ(rows[1].ask_price_raw, 10200);

  const int64_t bucket_b = ((kBase + kBucket) / kBucket) * kBucket;
  EXPECT_EQ(rows[2].bucket_ts_ns, bucket_b);
  EXPECT_EQ(rows[2].level, 0);
  EXPECT_EQ(rows[2].bid_price_raw, 10000);
  EXPECT_EQ(rows[2].ask_price_raw, 10200);
  EXPECT_EQ(rows[2].flags, 0);
}

TEST(BookSnapshotAggregatorTest, SnapshotResetsLadder)
{
  BookSnapshotBinAggregator agg(kBucket, 5);
  agg.onEvent(bookEvent(kBase, 1, true, {lvl(10000, 100)}, {lvl(10100, 100)}));
  agg.onEvent(bookEvent(kBase + kBucket, 1, true, {lvl(20000, 1)}, {lvl(20100, 1)}));
  agg.finalize();

  const auto& rows = agg.result();
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].bid_price_raw, 10000);
  // The second snapshot fully replaced the ladder.
  EXPECT_EQ(rows[1].bid_price_raw, 20000);
  EXPECT_EQ(rows[1].ask_price_raw, 20100);
}

TEST(BookSnapshotAggregatorTest, LevelsCapAndZeroPadding)
{
  BookSnapshotBinAggregator agg(kBucket, /*levels=*/2);
  agg.onEvent(bookEvent(kBase, 1, true,
                        {lvl(10000, 1), lvl(9900, 1), lvl(9800, 1)},
                        {lvl(10100, 1)}));
  agg.finalize();

  const auto& rows = agg.result();
  ASSERT_EQ(rows.size(), 2u);  // capped at 2 levels despite 3 bids
  EXPECT_EQ(rows[0].bid_price_raw, 10000);
  EXPECT_EQ(rows[0].ask_price_raw, 10100);
  EXPECT_EQ(rows[1].bid_price_raw, 9900);
  // No second ask level -> zero-padded.
  EXPECT_EQ(rows[1].ask_price_raw, 0);
  EXPECT_EQ(rows[1].ask_qty_raw, 0);
}

TEST(BookSnapshotAggregatorTest, CrossedFlag)
{
  BookSnapshotBinAggregator agg(kBucket, 5);
  agg.onEvent(bookEvent(kBase, 1, true, {lvl(10200, 1)}, {lvl(10100, 1)}));
  agg.finalize();

  const auto& rows = agg.result();
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].flags, BookSnapshotBinAggregator::kFlagCrossed);
}

TEST(BookSnapshotAggregatorTest, MultiSymbolSortedRows)
{
  BookSnapshotBinAggregator agg(kBucket, 5);
  agg.onEvent(bookEvent(kBase, 2, true, {lvl(200, 1)}, {lvl(201, 1)}));
  agg.onEvent(bookEvent(kBase + 1, 1, true, {lvl(100, 1)}, {lvl(101, 1)}));
  agg.finalize();

  const auto& rows = agg.result();
  ASSERT_EQ(rows.size(), 2u);
  // Sorted by (bucket, symbol_id, level): symbol 1 first.
  EXPECT_EQ(rows[0].symbol_id, 1u);
  EXPECT_EQ(rows[1].symbol_id, 2u);
}

TEST(BookSnapshotAggregatorTest, SymbolFilterAndTradeFilter)
{
  BookSnapshotBinAggregator agg(kBucket, 5, AggregatorEventFilter::BooksOnly, {1});
  agg.onEvent(bookEvent(kBase, 2, true, {lvl(200, 1)}, {lvl(201, 1)}));
  agg.finalize();
  EXPECT_TRUE(agg.result().empty());

  BookSnapshotBinAggregator trades_only(kBucket, 5, AggregatorEventFilter::Trades);
  trades_only.onEvent(bookEvent(kBase, 1, true, {lvl(100, 1)}, {lvl(101, 1)}));
  trades_only.finalize();
  EXPECT_TRUE(trades_only.result().empty());
}

TEST(BookSnapshotAggregatorTest, EmptyBucketsProduceNoRows)
{
  BookSnapshotBinAggregator agg(kBucket, 5);
  agg.onEvent(bookEvent(kBase, 1, true, {lvl(10000, 1)}, {lvl(10100, 1)}));
  // Next event 10 buckets later: intervening empty buckets emit nothing.
  agg.onEvent(bookEvent(kBase + 10 * kBucket, 1, false, {lvl(10000, 2)}, {}));
  agg.finalize();

  const auto& rows = agg.result();
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[1].bucket_ts_ns - rows[0].bucket_ts_ns, 10 * kBucket);
}

TEST(BookSnapshotAggregatorTest, RefusesParallelRun)
{
  BookSnapshotBinAggregator agg(kBucket, 5);
  EXPECT_THROW(agg.cloneEmpty(), std::runtime_error);
  BookSnapshotBinAggregator other(kBucket, 5);
  EXPECT_THROW(agg.merge(other), std::runtime_error);
}

TEST(BookSnapshotAggregatorTest, BadConstructorArgs)
{
  EXPECT_THROW(BookSnapshotBinAggregator(0, 5), std::invalid_argument);
  EXPECT_THROW(BookSnapshotBinAggregator(kBucket, 0), std::invalid_argument);
}
