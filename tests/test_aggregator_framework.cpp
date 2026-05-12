/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/aggregator.h"
#include "flox/replay/aggregators/bin_count.h"
#include "flox/replay/aggregators/event_type_stats.h"
#include "flox/replay/binary_format_v1.h"
#include "flox/replay/readers/binary_log_reader.h"
#include "flox/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <filesystem>

using namespace flox::replay;

namespace
{

// Test-only aggregator that just counts dispatched events and finalize() calls.
// Lets the framework test exercise the IAggregator contract end-to-end without
// pulling in any concrete aggregator implementation.
class CounterAggregator : public IAggregator
{
 public:
  void onEvent(const ReplayEvent& ev) override
  {
    ++_events;
    if (ev.type == EventType::Trade)
    {
      ++_trades;
    }
    else if (ev.type == EventType::BookSnapshot || ev.type == EventType::BookDelta)
    {
      ++_books;
    }
  }

  void finalize() override { ++_finalize_calls; }

  int events() const { return _events; }
  int trades() const { return _trades; }
  int books() const { return _books; }
  int finalize_calls() const { return _finalize_calls; }

 private:
  int _events{0};
  int _trades{0};
  int _books{0};
  int _finalize_calls{0};
};

}  // namespace

class AggregatorFrameworkTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    _test_dir = std::filesystem::temp_directory_path() / "flox_test_aggregator";
    std::filesystem::remove_all(_test_dir);
    std::filesystem::create_directories(_test_dir);
  }

  void TearDown() override { std::filesystem::remove_all(_test_dir); }

  // Write `num_trades` trades into _test_dir using LZ4 compression so the
  // run() path exercises decompression.
  void writeCompressedTrades(int num_trades)
  {
    WriterConfig config{.output_dir = _test_dir,
                        .index_interval = 100,
                        .compression = CompressionType::LZ4};
    BinaryLogWriter writer(config);

    for (int i = 0; i < num_trades; ++i)
    {
      TradeRecord trade{};
      trade.exchange_ts_ns = 1'000'000'000LL + i * 1'000'000LL;
      trade.symbol_id = 1;
      trade.trade_id = static_cast<uint64_t>(i);
      writer.writeTrade(trade);
    }
    writer.close();
  }

  std::filesystem::path _test_dir;
};

TEST_F(AggregatorFrameworkTest, RunEmptySpanIsNoOp)
{
  // Contract: run([]) does not walk the tape, does not decompress, and
  // returns true. Tested behaviourally: even on an empty data directory
  // (which would make forEach fail), run([]) still succeeds.
  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  std::array<IAggregator*, 0> none{};
  EXPECT_TRUE(reader.run(none));
}

TEST_F(AggregatorFrameworkTest, RunSingleAggregatorCountsAllEvents)
{
  constexpr int num_trades = 250;
  writeCompressedTrades(num_trades);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  CounterAggregator counter;
  std::array<IAggregator*, 1> aggregators{&counter};
  ASSERT_TRUE(reader.run(aggregators));

  EXPECT_EQ(counter.events(), num_trades);
  EXPECT_EQ(counter.trades(), num_trades);
  EXPECT_EQ(counter.books(), 0);
  EXPECT_EQ(counter.finalize_calls(), 1);
}

TEST_F(AggregatorFrameworkTest, RunMultipleAggregatorsSinglePass)
{
  // Functional single-pass guarantee: every aggregator sees every event
  // exactly once, finalize() fires exactly once per aggregator. This is
  // the contract that lets a 5-aggregator panel cost one decompression
  // scan, not five. The CI gate is functional (event-count parity);
  // wall-clock perf parity lives outside CI.
  constexpr int num_trades = 100;
  writeCompressedTrades(num_trades);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  CounterAggregator a;
  CounterAggregator b;
  CounterAggregator c;
  std::array<IAggregator*, 3> aggregators{&a, &b, &c};
  ASSERT_TRUE(reader.run(aggregators));

  EXPECT_EQ(a.events(), num_trades);
  EXPECT_EQ(b.events(), num_trades);
  EXPECT_EQ(c.events(), num_trades);
  EXPECT_EQ(a.finalize_calls(), 1);
  EXPECT_EQ(b.finalize_calls(), 1);
  EXPECT_EQ(c.finalize_calls(), 1);
}

TEST_F(AggregatorFrameworkTest, RunIgnoresNullAggregators)
{
  // The dispatch loop skips nullptrs. Useful for binding layers that
  // build the span from an optional or a map-by-name lookup that may
  // return missing entries — null-tolerance keeps the C++ side simple.
  constexpr int num_trades = 50;
  writeCompressedTrades(num_trades);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  CounterAggregator counter;
  std::array<IAggregator*, 3> aggregators{nullptr, &counter, nullptr};
  ASSERT_TRUE(reader.run(aggregators));

  EXPECT_EQ(counter.events(), num_trades);
  EXPECT_EQ(counter.finalize_calls(), 1);
}

// ───────────────────────────────────────────────────────────────────────
// EventTypeStatsAggregator
// ───────────────────────────────────────────────────────────────────────

namespace
{

// Mixed-symbol mixed-event tape fixture. Three symbols, each gets a
// distinct mix of trades / snapshots / deltas so the per-symbol +
// per-event-kind counters can be verified independently.
void writeMixedTape(const std::filesystem::path& dir)
{
  WriterConfig config{.output_dir = dir,
                      .index_interval = 100,
                      .compression = CompressionType::LZ4};
  BinaryLogWriter writer(config);

  // symbol 1: 10 trades, 4 snapshots, 0 deltas
  for (int i = 0; i < 10; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 1'000'000'000LL + i * 1'000LL;
    trade.symbol_id = 1;
    trade.trade_id = static_cast<uint64_t>(i);
    writer.writeTrade(trade);
  }
  for (int i = 0; i < 4; ++i)
  {
    BookRecordHeader hdr{};
    hdr.exchange_ts_ns = 2'000'000'000LL + i * 1'000LL;
    hdr.symbol_id = 1;
    hdr.type = 0;  // snapshot
    hdr.bid_count = 1;
    hdr.ask_count = 1;
    std::vector<BookLevel> bids = {{1, 1}};
    std::vector<BookLevel> asks = {{2, 1}};
    writer.writeBook(hdr, bids, asks);
  }

  // symbol 2: 5 trades, 0 snapshots, 7 deltas
  for (int i = 0; i < 5; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 3'000'000'000LL + i * 1'000LL;
    trade.symbol_id = 2;
    trade.trade_id = static_cast<uint64_t>(100 + i);
    writer.writeTrade(trade);
  }
  for (int i = 0; i < 7; ++i)
  {
    BookRecordHeader hdr{};
    hdr.exchange_ts_ns = 4'000'000'000LL + i * 1'000LL;
    hdr.symbol_id = 2;
    hdr.type = 1;  // delta
    hdr.bid_count = 1;
    hdr.ask_count = 0;
    std::vector<BookLevel> bids = {{3, 1}};
    std::vector<BookLevel> asks;
    writer.writeBook(hdr, bids, asks);
  }

  // symbol 3: 2 trades, 1 snapshot, 1 delta
  for (int i = 0; i < 2; ++i)
  {
    TradeRecord trade{};
    trade.exchange_ts_ns = 5'000'000'000LL + i * 1'000LL;
    trade.symbol_id = 3;
    trade.trade_id = static_cast<uint64_t>(200 + i);
    writer.writeTrade(trade);
  }
  {
    BookRecordHeader hdr{};
    hdr.exchange_ts_ns = 6'000'000'000LL;
    hdr.symbol_id = 3;
    hdr.type = 0;
    hdr.bid_count = 1;
    hdr.ask_count = 1;
    std::vector<BookLevel> bids = {{4, 1}};
    std::vector<BookLevel> asks = {{5, 1}};
    writer.writeBook(hdr, bids, asks);
  }
  {
    BookRecordHeader hdr{};
    hdr.exchange_ts_ns = 6'001'000'000LL;
    hdr.symbol_id = 3;
    hdr.type = 1;
    hdr.bid_count = 0;
    hdr.ask_count = 1;
    std::vector<BookLevel> bids;
    std::vector<BookLevel> asks = {{6, 1}};
    writer.writeBook(hdr, bids, asks);
  }

  writer.close();
}

const EventTypeStatsAggregator::PerSymbolRow* findRow(
    const std::vector<EventTypeStatsAggregator::PerSymbolRow>& rows, uint32_t sid)
{
  auto it = std::find_if(rows.begin(), rows.end(),
                         [sid](const auto& r)
                         { return r.symbol_id == sid; });
  return it == rows.end() ? nullptr : &*it;
}

}  // namespace

TEST_F(AggregatorFrameworkTest, EventTypeStatsCountsPerSymbol)
{
  writeMixedTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  EventTypeStatsAggregator stats;
  std::array<IAggregator*, 1> aggregators{&stats};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = stats.result();
  ASSERT_EQ(rows.size(), 3u);
  // Sorted by symbol_id.
  EXPECT_EQ(rows[0].symbol_id, 1u);
  EXPECT_EQ(rows[1].symbol_id, 2u);
  EXPECT_EQ(rows[2].symbol_id, 3u);

  const auto* s1 = findRow(rows, 1);
  ASSERT_NE(s1, nullptr);
  EXPECT_EQ(s1->trades, 10u);
  EXPECT_EQ(s1->book_snapshots, 4u);
  EXPECT_EQ(s1->book_deltas, 0u);

  const auto* s2 = findRow(rows, 2);
  ASSERT_NE(s2, nullptr);
  EXPECT_EQ(s2->trades, 5u);
  EXPECT_EQ(s2->book_snapshots, 0u);
  EXPECT_EQ(s2->book_deltas, 7u);

  const auto* s3 = findRow(rows, 3);
  ASSERT_NE(s3, nullptr);
  EXPECT_EQ(s3->trades, 2u);
  EXPECT_EQ(s3->book_snapshots, 1u);
  EXPECT_EQ(s3->book_deltas, 1u);
}

TEST_F(AggregatorFrameworkTest, EventTypeStatsEventFilterTradesOnly)
{
  writeMixedTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  EventTypeStatsAggregator stats(AggregatorEventFilter::Trades);
  std::array<IAggregator*, 1> aggregators{&stats};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = stats.result();
  ASSERT_EQ(rows.size(), 3u);
  for (const auto& r : rows)
  {
    EXPECT_EQ(r.book_snapshots, 0u);
    EXPECT_EQ(r.book_deltas, 0u);
  }
  EXPECT_EQ(findRow(rows, 1)->trades, 10u);
  EXPECT_EQ(findRow(rows, 2)->trades, 5u);
  EXPECT_EQ(findRow(rows, 3)->trades, 2u);
}

TEST_F(AggregatorFrameworkTest, EventTypeStatsEventFilterBooksOnly)
{
  writeMixedTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  EventTypeStatsAggregator stats(AggregatorEventFilter::BooksOnly);
  std::array<IAggregator*, 1> aggregators{&stats};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = stats.result();
  // Only symbols with book events appear. Symbol 1: 4 snapshots, symbol
  // 2: 7 deltas, symbol 3: 1 + 1. All three have books, so 3 rows.
  ASSERT_EQ(rows.size(), 3u);
  for (const auto& r : rows)
  {
    EXPECT_EQ(r.trades, 0u);
  }
  EXPECT_EQ(findRow(rows, 1)->book_snapshots, 4u);
  EXPECT_EQ(findRow(rows, 2)->book_deltas, 7u);
  EXPECT_EQ(findRow(rows, 3)->book_snapshots, 1u);
  EXPECT_EQ(findRow(rows, 3)->book_deltas, 1u);
}

TEST_F(AggregatorFrameworkTest, EventTypeStatsSymbolFilter)
{
  writeMixedTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  // Filter to symbols 1 and 3 only. Symbol 2 should not appear in the
  // result vector at all (its counters stay at zero and the row is
  // never inserted).
  EventTypeStatsAggregator stats(AggregatorEventFilter::Both, {1, 3});
  std::array<IAggregator*, 1> aggregators{&stats};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = stats.result();
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].symbol_id, 1u);
  EXPECT_EQ(rows[1].symbol_id, 3u);
  EXPECT_EQ(findRow(rows, 2), nullptr);
}

TEST_F(AggregatorFrameworkTest, EventTypeStatsResultEmptyBeforeRun)
{
  // The contract: result() returns an empty vector until run() has
  // finalised. Skipping run() (or never starting it) yields no rows
  // even if onEvent was somehow invoked manually — finalize() is the
  // step that publishes the result.
  EventTypeStatsAggregator stats;
  EXPECT_TRUE(stats.result().empty());
}

// ───────────────────────────────────────────────────────────────────────
// BinCountAggregator
// ───────────────────────────────────────────────────────────────────────

namespace
{

// Tape with predictable per-bucket counts. 10 buckets at 100 ms each,
// symbol 1 gets 3 BUY + 2 SELL per bucket = 5 trades, symbol 2 gets 1
// trade per bucket (all BUY). All trades.
void writeBucketedTape(const std::filesystem::path& dir)
{
  WriterConfig config{.output_dir = dir,
                      .index_interval = 100,
                      .compression = CompressionType::LZ4};
  BinaryLogWriter writer(config);

  constexpr int64_t bucket_ns = 100'000'000LL;  // 100 ms

  uint64_t tid = 0;
  for (int b = 0; b < 10; ++b)
  {
    const int64_t base = b * bucket_ns + 1;  // offset into bucket
    // symbol 1: 3 BUY, 2 SELL
    for (int i = 0; i < 5; ++i)
    {
      TradeRecord t{};
      t.exchange_ts_ns = base + i;
      t.symbol_id = 1;
      t.trade_id = tid++;
      t.side = (i < 3) ? 0 : 1;  // 0=BUY, 1=SELL
      writer.writeTrade(t);
    }
    // symbol 2: 1 BUY
    TradeRecord t{};
    t.exchange_ts_ns = base + 50;
    t.symbol_id = 2;
    t.trade_id = tid++;
    t.side = 0;
    writer.writeTrade(t);
  }

  writer.close();
}

const BinCountAggregator::Row* findBin(const std::vector<BinCountAggregator::Row>& rows,
                                       int64_t bucket, uint32_t sym, uint8_t side)
{
  auto it = std::find_if(rows.begin(), rows.end(),
                         [=](const auto& r)
                         {
                           return r.bucket_ts_ns == bucket && r.symbol_id == sym &&
                                  r.side == side;
                         });
  return it == rows.end() ? nullptr : &*it;
}

}  // namespace

TEST_F(AggregatorFrameworkTest, BinCountFlatNoSplit)
{
  // 100 ms buckets, no side/symbol split. Each bucket sees 6 trades
  // (5 from symbol 1 + 1 from symbol 2). 10 buckets total.
  writeBucketedTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  BinCountAggregator bins(100'000'000LL);
  std::array<IAggregator*, 1> aggregators{&bins};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = bins.result();
  ASSERT_EQ(rows.size(), 10u);
  for (size_t i = 0; i < 10; ++i)
  {
    EXPECT_EQ(rows[i].bucket_ts_ns, static_cast<int64_t>(i) * 100'000'000LL);
    EXPECT_EQ(rows[i].symbol_id, 0u);
    EXPECT_EQ(rows[i].side, 0u);
    EXPECT_EQ(rows[i].count, 6u);
  }
}

TEST_F(AggregatorFrameworkTest, BinCountBySide)
{
  // Same tape, by_side=true. Each bucket: BUY = 3 (sym1) + 1 (sym2) =
  // 4, SELL = 2 (sym1). Rows are (bucket, sym=0, side=1) and
  // (bucket, sym=0, side=2).
  writeBucketedTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  BinCountAggregator bins(100'000'000LL, /*by_side=*/true);
  std::array<IAggregator*, 1> aggregators{&bins};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = bins.result();
  ASSERT_EQ(rows.size(), 20u);  // 10 buckets × 2 sides

  for (int b = 0; b < 10; ++b)
  {
    const int64_t bucket = b * 100'000'000LL;
    const auto* buys = findBin(rows, bucket, 0, 1);
    const auto* sells = findBin(rows, bucket, 0, 2);
    ASSERT_NE(buys, nullptr);
    ASSERT_NE(sells, nullptr);
    EXPECT_EQ(buys->count, 4u);
    EXPECT_EQ(sells->count, 2u);
  }
}

TEST_F(AggregatorFrameworkTest, BinCountBySymbol)
{
  writeBucketedTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  BinCountAggregator bins(100'000'000LL, /*by_side=*/false, /*by_symbol=*/true);
  std::array<IAggregator*, 1> aggregators{&bins};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = bins.result();
  ASSERT_EQ(rows.size(), 20u);  // 10 buckets × 2 symbols

  for (int b = 0; b < 10; ++b)
  {
    const int64_t bucket = b * 100'000'000LL;
    const auto* s1 = findBin(rows, bucket, 1, 0);
    const auto* s2 = findBin(rows, bucket, 2, 0);
    ASSERT_NE(s1, nullptr);
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(s1->count, 5u);
    EXPECT_EQ(s2->count, 1u);
  }
}

TEST_F(AggregatorFrameworkTest, BinCountBySideAndBySymbol)
{
  writeBucketedTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  BinCountAggregator bins(100'000'000LL, /*by_side=*/true, /*by_symbol=*/true);
  std::array<IAggregator*, 1> aggregators{&bins};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = bins.result();
  // Symbol 1 has BUY and SELL → 2 rows/bucket. Symbol 2 has only BUY → 1.
  ASSERT_EQ(rows.size(), 30u);  // 10 buckets × 3 (sym1 buy + sym1 sell + sym2 buy)

  for (int b = 0; b < 10; ++b)
  {
    const int64_t bucket = b * 100'000'000LL;
    EXPECT_EQ(findBin(rows, bucket, 1, 1)->count, 3u);  // sym1 BUY
    EXPECT_EQ(findBin(rows, bucket, 1, 2)->count, 2u);  // sym1 SELL
    EXPECT_EQ(findBin(rows, bucket, 2, 1)->count, 1u);  // sym2 BUY
    EXPECT_EQ(findBin(rows, bucket, 2, 2), nullptr);    // sym2 has no SELL
  }
}

TEST_F(AggregatorFrameworkTest, BinCountSymbolFilter)
{
  writeBucketedTape(_test_dir);

  ReaderConfig rconfig{.data_dir = _test_dir};
  BinaryLogReader reader(rconfig);

  // Filter to symbol 2 only — should drop symbol 1 trades entirely.
  BinCountAggregator bins(100'000'000LL, /*by_side=*/false, /*by_symbol=*/false,
                          AggregatorEventFilter::Both, {2});
  std::array<IAggregator*, 1> aggregators{&bins};
  ASSERT_TRUE(reader.run(aggregators));

  const auto& rows = bins.result();
  ASSERT_EQ(rows.size(), 10u);
  for (const auto& r : rows)
  {
    EXPECT_EQ(r.count, 1u);  // only sym2's single trade per bucket
  }
}

TEST_F(AggregatorFrameworkTest, BinCountRejectsNonPositiveBucket)
{
  EXPECT_THROW(BinCountAggregator(0), std::invalid_argument);
  EXPECT_THROW(BinCountAggregator(-1'000'000LL), std::invalid_argument);
}
