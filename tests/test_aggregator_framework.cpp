/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/aggregator.h"
#include "flox/replay/binary_format_v1.h"
#include "flox/replay/readers/binary_log_reader.h"
#include "flox/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>
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
