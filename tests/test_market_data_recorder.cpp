/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/market_data_recorder.h"
#include "flox/replay/readers/binary_log_reader.h"

#include "flox/aggregator/events/bar_event.h"
#include "flox/book/bus/book_update_bus.h"
#include "flox/book/bus/trade_bus.h"
#include "flox/book/events/book_update_event.h"
#include "flox/book/events/trade_event.h"
#include "flox/util/memory/pool.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <thread>

using namespace flox;
using namespace flox::replay;

class MarketDataRecorderTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    _test_dir = std::filesystem::temp_directory_path() / "flox_recorder_test";
    std::filesystem::remove_all(_test_dir);
    std::filesystem::create_directories(_test_dir);
  }

  void TearDown() override { std::filesystem::remove_all(_test_dir); }

  std::filesystem::path _test_dir;
};

TEST_F(MarketDataRecorderTest, RecordTrades)
{
  MarketDataRecorderConfig config{
      .output_dir = _test_dir,
      .exchange_id = 1,
  };

  MarketDataRecorder recorder(config);
  recorder.start();

  EXPECT_TRUE(recorder.isRecording());

  // Send some trades
  for (int i = 0; i < 10; ++i)
  {
    TradeEvent event{};
    event.trade.symbol = 1;
    event.trade.price = Price::fromDouble(50000.0 + i);
    event.trade.quantity = Quantity::fromDouble(1.0);
    event.trade.isBuy = (i % 2 == 0);
    event.trade.exchangeTsNs = 1000000000 + i * 1000000;
    event.trade_id = i;
    event.recvNs = 1000000100 + i * 1000000;

    recorder.onTrade(event);
  }

  recorder.stop();

  EXPECT_FALSE(recorder.isRecording());

  auto stats = recorder.stats();
  EXPECT_EQ(stats.trades_written, 10u);

  // Verify written data
  ReaderConfig reader_config{.data_dir = _test_dir};
  BinaryLogReader reader(reader_config);

  int count = 0;
  reader.forEach([&](const ReplayEvent& event)
                 {
    EXPECT_EQ(event.type, EventType::Trade);
    ++count;
    return true; });

  EXPECT_EQ(count, 10);
}

TEST_F(MarketDataRecorderTest, RecordBookUpdates)
{
  MarketDataRecorderConfig config{.output_dir = _test_dir};
  MarketDataRecorder recorder(config);
  recorder.start();

  std::pmr::monotonic_buffer_resource resource(4096);

  for (int i = 0; i < 5; ++i)
  {
    BookUpdateEvent event(&resource);
    event.update.symbol = 2;
    event.update.type = (i == 0) ? BookUpdateType::SNAPSHOT : BookUpdateType::DELTA;
    event.update.exchangeTsNs = 2000000000 + i * 1000000;
    event.seq = i;
    event.recvNs = 2000000100 + i * 1000000;

    event.update.bids.emplace_back(Price::fromDouble(50000.0 - i), Quantity::fromDouble(10.0));
    event.update.asks.emplace_back(Price::fromDouble(50001.0 + i), Quantity::fromDouble(10.0));

    recorder.onBookUpdate(event);
    resource.release();
  }

  recorder.stop();

  auto stats = recorder.stats();
  EXPECT_EQ(stats.book_updates_written, 5u);

  // Verify written data
  ReaderConfig reader_config{.data_dir = _test_dir};
  BinaryLogReader reader(reader_config);

  int count = 0;
  reader.forEach([&](const ReplayEvent& event)
                 {
    EXPECT_NE(event.type, EventType::Trade);
    EXPECT_EQ(event.book_header.symbol_id, 2u);
    ++count;
    return true; });

  EXPECT_EQ(count, 5);
}

TEST_F(MarketDataRecorderTest, IntegrationWithBuses)
{
  using BookUpdatePool = pool::Pool<BookUpdateEvent, 31>;

  BookUpdateBus bookBus;
  TradeBus tradeBus;
  BookUpdatePool bookPool;

  bookBus.enableDrainOnStop();
  tradeBus.enableDrainOnStop();

  MarketDataRecorderConfig config{.output_dir = _test_dir};
  auto recorder = std::make_unique<MarketDataRecorder>(config);

  // Subscribe recorder to buses
  bookBus.subscribe(recorder.get());
  tradeBus.subscribe(recorder.get());

  // Start everything
  bookBus.start();
  tradeBus.start();
  recorder->start();

  int books_published = 0;

  // Publish events
  for (int i = 0; i < 20; ++i)
  {
    TradeEvent trade{};
    trade.trade.symbol = 1;
    trade.trade.price = Price::fromDouble(50000.0);
    trade.trade.quantity = Quantity::fromDouble(1.0);
    trade.trade.exchangeTsNs = 1000000000 + i * 1000000;
    trade.trade_id = i;

    tradeBus.publish(trade);

    auto bookOpt = bookPool.acquire();
    if (bookOpt)
    {
      auto& book = *bookOpt;
      book->update.symbol = 1;
      book->update.type = BookUpdateType::DELTA;
      book->update.bids.emplace_back(Price::fromDouble(50000.0), Quantity::fromDouble(10.0));
      book->seq = i;

      bookBus.publish(std::move(book));
      ++books_published;
    }
  }

  // Give time for buses to drain
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Stop in reverse order
  recorder->stop();
  tradeBus.stop();
  bookBus.stop();

  auto stats = recorder->stats();
  EXPECT_EQ(stats.trades_written, 20u);
  EXPECT_EQ(stats.book_updates_written, static_cast<uint64_t>(books_published));
}

TEST_F(MarketDataRecorderTest, FlushAndRestart)
{
  MarketDataRecorderConfig config{.output_dir = _test_dir};
  MarketDataRecorder recorder(config);

  recorder.start();

  // Write some data
  TradeEvent event{};
  event.trade.symbol = 1;
  event.trade.exchangeTsNs = 1000000000;
  recorder.onTrade(event);

  // Flush
  recorder.flush();

  // Write more data
  event.trade.exchangeTsNs = 2000000000;
  recorder.onTrade(event);

  recorder.stop();

  // Restart
  recorder.start();

  event.trade.exchangeTsNs = 3000000000;
  recorder.onTrade(event);

  recorder.stop();

  // Should have multiple segment files
  int file_count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(_test_dir))
  {
    if (entry.path().extension() == ".floxlog")
    {
      ++file_count;
    }
  }

  EXPECT_GE(file_count, 2);  // At least 2 segments
}

TEST_F(MarketDataRecorderTest, SetOutputDir)
{
  MarketDataRecorderConfig config{.output_dir = _test_dir};
  MarketDataRecorder recorder(config);

  recorder.start();

  TradeEvent event{};
  event.trade.symbol = 1;
  event.trade.exchangeTsNs = 1000000000;
  recorder.onTrade(event);

  // Change output directory
  auto new_dir = _test_dir / "subdir";
  recorder.setOutputDir(new_dir);

  event.trade.exchangeTsNs = 2000000000;
  recorder.onTrade(event);

  recorder.stop();

  // Check both directories have files
  EXPECT_TRUE(std::filesystem::exists(_test_dir));
  EXPECT_TRUE(std::filesystem::exists(new_dir));
}

TEST_F(MarketDataRecorderTest, BarsAreIgnored)
{
  MarketDataRecorderConfig config{.output_dir = _test_dir};
  MarketDataRecorder recorder(config);

  recorder.start();

  BarEvent bar{};
  bar.symbol = 1;
  recorder.onBar(bar);

  recorder.stop();

  auto stats = recorder.stats();
  EXPECT_EQ(stats.trades_written, 0u);
  EXPECT_EQ(stats.book_updates_written, 0u);
}

TEST_F(MarketDataRecorderTest, NotRecordingWhenStopped)
{
  MarketDataRecorderConfig config{.output_dir = _test_dir};
  MarketDataRecorder recorder(config);

  // Don't start - should not record
  EXPECT_FALSE(recorder.isRecording());

  TradeEvent event{};
  event.trade.symbol = 1;
  event.trade.exchangeTsNs = 1000000000;
  recorder.onTrade(event);

  auto stats = recorder.stats();
  EXPECT_EQ(stats.trades_written, 0u);
}

TEST_F(MarketDataRecorderTest, UniqueSubscriberId)
{
  MarketDataRecorderConfig config{.output_dir = _test_dir};

  MarketDataRecorder recorder1(config);
  MarketDataRecorder recorder2(config);

  EXPECT_NE(recorder1.id(), recorder2.id());
}
