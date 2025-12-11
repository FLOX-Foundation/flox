/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/market_data_recorder.h"
#include "flox/replay/replay_connector.h"
#include "flox/replay/writers/binary_log_writer.h"

#include "flox/book/bus/book_update_bus.h"
#include "flox/book/bus/trade_bus.h"
#include "flox/common.h"
#include "flox/strategy/abstract_strategy.h"
#include "flox/util/memory/pool.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

using namespace flox;
using namespace flox::replay;

class ReplayConnectorTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    _test_dir = std::filesystem::temp_directory_path() / "flox_replay_test";
    std::filesystem::remove_all(_test_dir);
    std::filesystem::create_directories(_test_dir);
  }

  void TearDown() override { std::filesystem::remove_all(_test_dir); }

  std::filesystem::path _test_dir;
};

class TestStrategy : public IStrategy
{
 public:
  SubscriberId id() const override { return 42; }

  void onTrade(const TradeEvent& event) override
  {
    ++trades_received;
    last_trade_price = event.trade.price;
  }

  void onBookUpdate(const BookUpdateEvent& event) override
  {
    ++book_updates_received;
    if (!event.update.bids.empty())
    {
      last_bid = event.update.bids[0].price;
    }
  }

  std::atomic<int> trades_received{0};
  std::atomic<int> book_updates_received{0};
  Price last_trade_price{};
  Price last_bid{};
};

TEST_F(ReplayConnectorTest, BasicReplay)
{
  // First, write some test data
  {
    WriterConfig config{.output_dir = _test_dir};
    BinaryLogWriter writer(config);

    for (int i = 0; i < 10; ++i)
    {
      TradeRecord trade{};
      trade.exchange_ts_ns = 1000000000 + i * 1000000;
      trade.price_raw = (50000LL + i) * 1000000LL;
      trade.qty_raw = 1000000;
      trade.symbol_id = 1;
      trade.side = 1;
      writer.writeTrade(trade);
    }
    writer.close();
  }

  // Create replay connector
  ReplayConnectorConfig config{
      .data_dir = _test_dir,
      .speed = ReplaySpeed::max(),
  };

  auto replay = std::make_unique<ReplayConnector>(config);

  // Track received events
  std::atomic<int> trade_count{0};
  replay->setCallbacks(
      [](const BookUpdateEvent&) {},
      [&](const TradeEvent& event)
      {
        ++trade_count;
        EXPECT_EQ(event.trade.symbol, 1u);
      });

  replay->start();

  // Wait for replay to finish
  while (!replay->isFinished())
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  replay->stop();

  EXPECT_EQ(trade_count.load(), 10);
}

TEST_F(ReplayConnectorTest, ReplayWithBuses)
{
  // Write test data
  {
    WriterConfig config{.output_dir = _test_dir};
    BinaryLogWriter writer(config);

    for (int i = 0; i < 5; ++i)
    {
      TradeRecord trade{};
      trade.exchange_ts_ns = 1000000000 + i * 1000000;
      trade.price_raw = 50000LL * 1000000LL;
      trade.symbol_id = 1;
      writer.writeTrade(trade);
    }

    for (int i = 0; i < 3; ++i)
    {
      BookRecordHeader header{};
      header.exchange_ts_ns = 2000000000 + i * 1000000;
      header.symbol_id = 1;
      header.bid_count = 1;
      header.ask_count = 1;
      header.type = 0;

      std::vector<replay::BookLevel> bids = {{50000LL * 1000000LL, 1000000LL}};
      std::vector<replay::BookLevel> asks = {{50001LL * 1000000LL, 1000000LL}};
      writer.writeBook(header, bids, asks);
    }
    writer.close();
  }

  // Set up buses and strategy
  BookUpdateBus bookBus;
  TradeBus tradeBus;

  bookBus.enableDrainOnStop();
  tradeBus.enableDrainOnStop();

  TestStrategy strategy;
  bookBus.subscribe(&strategy);
  tradeBus.subscribe(&strategy);

  // Create replay connector
  ReplayConnectorConfig config{
      .data_dir = _test_dir,
      .speed = ReplaySpeed::max(),
  };

  auto replay = std::make_unique<ReplayConnector>(config);

  using BookUpdatePool = pool::Pool<BookUpdateEvent, 7>;
  BookUpdatePool bookPool;

  replay->setCallbacks(
      [&](const BookUpdateEvent& event)
      {
        auto handle = bookPool.acquire();
        if (handle)
        {
          (*handle)->update = event.update;
          (*handle)->seq = event.seq;
          bookBus.publish(std::move(*handle));
        }
      },
      [&](const TradeEvent& event)
      { tradeBus.publish(event); });

  // Start everything
  bookBus.start();
  tradeBus.start();
  replay->start();

  // Wait for replay to finish
  while (!replay->isFinished())
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  replay->stop();
  tradeBus.stop();
  bookBus.stop();

  EXPECT_EQ(strategy.trades_received.load(), 5);
  EXPECT_EQ(strategy.book_updates_received.load(), 3);
}

TEST_F(ReplayConnectorTest, DataRange)
{
  // Write test data with known time range
  {
    WriterConfig config{.output_dir = _test_dir};
    BinaryLogWriter writer(config);

    TradeRecord trade{};
    trade.exchange_ts_ns = 1000;
    trade.symbol_id = 1;
    writer.writeTrade(trade);

    trade.exchange_ts_ns = 9000;
    writer.writeTrade(trade);

    writer.close();
  }

  ReplayConnectorConfig config{.data_dir = _test_dir};
  auto replay = std::make_unique<ReplayConnector>(config);

  replay->setCallbacks([](const BookUpdateEvent&) {}, [](const TradeEvent&) {});

  replay->start();

  // Wait a bit for reader to initialize
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto range = replay->dataRange();
  ASSERT_TRUE(range.has_value());
  EXPECT_EQ(range->start_ns, 1000);
  EXPECT_EQ(range->end_ns, 9000);

  replay->stop();
}

TEST_F(ReplayConnectorTest, SpeedChange)
{
  // Write some events
  {
    WriterConfig config{.output_dir = _test_dir};
    BinaryLogWriter writer(config);

    for (int i = 0; i < 10; ++i)
    {
      TradeRecord trade{};
      trade.exchange_ts_ns = i * 100000000;  // 100ms apart
      trade.symbol_id = 1;
      writer.writeTrade(trade);
    }
    writer.close();
  }

  ReplayConnectorConfig config{
      .data_dir = _test_dir,
      .speed = ReplaySpeed::max(),
  };

  auto replay = std::make_unique<ReplayConnector>(config);

  std::atomic<int> count{0};
  replay->setCallbacks([](const BookUpdateEvent&) {}, [&](const TradeEvent&)
                       { ++count; });

  replay->start();

  // Change speed mid-replay
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  replay->setSpeed(ReplaySpeed::fast(100.0));

  while (!replay->isFinished())
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  replay->stop();

  EXPECT_EQ(count.load(), 10);
}

TEST_F(ReplayConnectorTest, ExchangeId)
{
  ReplayConnectorConfig config{.data_dir = _test_dir};
  auto replay = std::make_unique<ReplayConnector>(config);

  EXPECT_EQ(replay->exchangeId(), "replay");
  EXPECT_FALSE(replay->isLive());
}

TEST_F(ReplayConnectorTest, CurrentPosition)
{
  // Write events with increasing timestamps
  {
    WriterConfig config{.output_dir = _test_dir};
    BinaryLogWriter writer(config);

    for (int i = 1; i <= 5; ++i)
    {
      TradeRecord trade{};
      trade.exchange_ts_ns = i * 1000000000LL;  // 1s, 2s, 3s, 4s, 5s
      trade.symbol_id = 1;
      writer.writeTrade(trade);
    }
    writer.close();
  }

  ReplayConnectorConfig config{
      .data_dir = _test_dir,
      .speed = ReplaySpeed::max(),
  };

  auto replay = std::make_unique<ReplayConnector>(config);

  int64_t last_pos = 0;
  replay->setCallbacks([](const BookUpdateEvent&) {},
                       [&](const TradeEvent&)
                       { last_pos = replay->currentPosition(); });

  replay->start();

  while (!replay->isFinished())
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  replay->stop();

  EXPECT_EQ(last_pos, 5000000000LL);  // Last event timestamp
}

TEST_F(ReplayConnectorTest, SeekTo)
{
  // Write events with index for seeking
  {
    WriterConfig config{
        .output_dir = _test_dir,
        .create_index = true,
        .index_interval = 10,
    };
    BinaryLogWriter writer(config);

    for (int i = 1; i <= 100; ++i)
    {
      TradeRecord trade{};
      trade.exchange_ts_ns = i * 1000000000LL;  // 1s, 2s, ..., 100s
      trade.symbol_id = 1;
      trade.price_raw = i * 1000000LL;
      writer.writeTrade(trade);
    }
    writer.close();
  }

  ReplayConnectorConfig config{
      .data_dir = _test_dir,
      .speed = ReplaySpeed::max(),
  };

  auto replay = std::make_unique<ReplayConnector>(config);

  std::atomic<int64_t> first_received_ts{0};
  std::atomic<int> trade_count{0};

  replay->setCallbacks([](const BookUpdateEvent&) {},
                       [&](const TradeEvent& ev)
                       {
                         int64_t expected = 0;
                         first_received_ts.compare_exchange_strong(expected, ev.trade.exchangeTsNs);
                         ++trade_count;
                       });

  // Seek before starting
  EXPECT_TRUE(replay->seekTo(50LL * 1000000000LL));  // Seek to 50s

  replay->start();

  while (!replay->isFinished())
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  replay->stop();

  // Should have received events from ~50s onwards
  EXPECT_GE(first_received_ts.load(), 50LL * 1000000000LL);
  EXPECT_LE(trade_count.load(), 60);  // Approx 50 events (50s to 100s) + some tolerance
}
