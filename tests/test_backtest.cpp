/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/backtest_result.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/backtest/simulated_clock.h"
#include "flox/backtest/simulated_executor.h"
#include "flox/replay/abstract_event_reader.h"
#include "flox/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>
#include <cmath>
#include <filesystem>

using namespace flox;

class BacktestTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    _test_dir = std::filesystem::temp_directory_path() / "flox_backtest_test";
    std::filesystem::remove_all(_test_dir);
    std::filesystem::create_directories(_test_dir);
  }

  void TearDown() override { std::filesystem::remove_all(_test_dir); }

  std::filesystem::path _test_dir;
};

TEST_F(BacktestTest, SimulatedClockBasic)
{
  SimulatedClock clock;
  EXPECT_EQ(clock.nowNs(), 0);

  clock.advanceTo(1000);
  EXPECT_EQ(clock.nowNs(), 1000);

  clock.advanceTo(500);
  EXPECT_EQ(clock.nowNs(), 1000);

  clock.advanceTo(2000);
  EXPECT_EQ(clock.nowNs(), 2000);

  clock.reset(100);
  EXPECT_EQ(clock.nowNs(), 100);
}

TEST_F(BacktestTest, SimulatedExecutorMarketOrderFill)
{
  SimulatedClock clock;
  SimulatedExecutor executor(clock);

  std::vector<OrderEvent> events;
  executor.setOrderEventCallback([&events](const OrderEvent& ev)
                                 { events.push_back(ev); });

  std::pmr::monotonic_buffer_resource pool(4096);
  std::pmr::vector<BookLevel> bids(&pool);
  std::pmr::vector<BookLevel> asks(&pool);

  bids.emplace_back(Price::fromDouble(100.0), Quantity::fromDouble(10.0));
  asks.emplace_back(Price::fromDouble(101.0), Quantity::fromDouble(10.0));

  executor.onBookUpdate(1, bids, asks);

  Order buyOrder;
  buyOrder.id = 1;
  buyOrder.symbol = 1;
  buyOrder.side = Side::BUY;
  buyOrder.type = OrderType::MARKET;
  buyOrder.quantity = Quantity::fromDouble(1.0);

  executor.submitOrder(buyOrder);

  ASSERT_EQ(events.size(), 3);
  EXPECT_EQ(events[0].status, OrderEventStatus::SUBMITTED);
  EXPECT_EQ(events[1].status, OrderEventStatus::ACCEPTED);
  EXPECT_EQ(events[2].status, OrderEventStatus::FILLED);

  ASSERT_EQ(executor.fills().size(), 1);
  EXPECT_EQ(executor.fills()[0].orderId, 1);
  EXPECT_DOUBLE_EQ(executor.fills()[0].price.toDouble(), 101.0);
  EXPECT_DOUBLE_EQ(executor.fills()[0].quantity.toDouble(), 1.0);
}

TEST_F(BacktestTest, SimulatedExecutorLimitOrderFill)
{
  SimulatedClock clock;
  SimulatedExecutor executor(clock);

  std::vector<OrderEvent> events;
  executor.setOrderEventCallback([&events](const OrderEvent& ev)
                                 { events.push_back(ev); });

  std::pmr::monotonic_buffer_resource pool(4096);
  std::pmr::vector<BookLevel> bids(&pool);
  std::pmr::vector<BookLevel> asks(&pool);

  bids.emplace_back(Price::fromDouble(100.0), Quantity::fromDouble(10.0));
  asks.emplace_back(Price::fromDouble(105.0), Quantity::fromDouble(10.0));

  executor.onBookUpdate(1, bids, asks);

  Order limitBuy;
  limitBuy.id = 1;
  limitBuy.symbol = 1;
  limitBuy.side = Side::BUY;
  limitBuy.type = OrderType::LIMIT;
  limitBuy.price = Price::fromDouble(102.0);
  limitBuy.quantity = Quantity::fromDouble(1.0);

  executor.submitOrder(limitBuy);

  EXPECT_EQ(executor.fills().size(), 0);

  asks.clear();
  asks.emplace_back(Price::fromDouble(101.0), Quantity::fromDouble(10.0));
  executor.onBookUpdate(1, bids, asks);

  ASSERT_EQ(executor.fills().size(), 1);
  EXPECT_DOUBLE_EQ(executor.fills()[0].price.toDouble(), 101.0);
}

TEST_F(BacktestTest, SimulatedExecutorCancelOrder)
{
  SimulatedClock clock;
  SimulatedExecutor executor(clock);

  std::vector<OrderEvent> events;
  executor.setOrderEventCallback([&events](const OrderEvent& ev)
                                 { events.push_back(ev); });

  std::pmr::monotonic_buffer_resource pool(4096);
  std::pmr::vector<BookLevel> bids(&pool);
  std::pmr::vector<BookLevel> asks(&pool);

  bids.emplace_back(Price::fromDouble(100.0), Quantity::fromDouble(10.0));
  asks.emplace_back(Price::fromDouble(110.0), Quantity::fromDouble(10.0));

  executor.onBookUpdate(1, bids, asks);

  Order limitBuy;
  limitBuy.id = 1;
  limitBuy.symbol = 1;
  limitBuy.side = Side::BUY;
  limitBuy.type = OrderType::LIMIT;
  limitBuy.price = Price::fromDouble(102.0);
  limitBuy.quantity = Quantity::fromDouble(1.0);

  executor.submitOrder(limitBuy);
  executor.cancelOrder(1);

  bool hasCanceled = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::CANCELED)
    {
      hasCanceled = true;
      break;
    }
  }
  EXPECT_TRUE(hasCanceled);
}

TEST_F(BacktestTest, BacktestResultPnlTracking)
{
  BacktestConfig config;
  config.feeRate = 0.0;  // No fees for this test
  BacktestResult result(config);

  Fill buyFill;
  buyFill.orderId = 1;
  buyFill.symbol = 1;
  buyFill.side = Side::BUY;
  buyFill.price = Price::fromDouble(100.0);
  buyFill.quantity = Quantity::fromDouble(1.0);
  buyFill.timestampNs = 1000;

  result.recordFill(buyFill);

  Fill sellFill;
  sellFill.orderId = 2;
  sellFill.symbol = 1;
  sellFill.side = Side::SELL;
  sellFill.price = Price::fromDouble(110.0);
  sellFill.quantity = Quantity::fromDouble(1.0);
  sellFill.timestampNs = 2000;

  result.recordFill(sellFill);

  auto stats = result.computeStats();
  EXPECT_DOUBLE_EQ(stats.totalPnl, 10.0);
  EXPECT_EQ(stats.totalTrades, 1);
  EXPECT_EQ(stats.winningTrades, 1);
  EXPECT_EQ(stats.losingTrades, 0);
}

TEST_F(BacktestTest, BacktestResultDrawdown)
{
  BacktestConfig config;
  config.feeRate = 0.0;  // No fees for this test
  BacktestResult result(config);

  Fill buy1;
  buy1.orderId = 1;
  buy1.symbol = 1;
  buy1.side = Side::BUY;
  buy1.price = Price::fromDouble(100.0);
  buy1.quantity = Quantity::fromDouble(1.0);
  buy1.timestampNs = 1000;
  result.recordFill(buy1);

  Fill sell1;
  sell1.orderId = 2;
  sell1.symbol = 1;
  sell1.side = Side::SELL;
  sell1.price = Price::fromDouble(120.0);
  sell1.quantity = Quantity::fromDouble(1.0);
  sell1.timestampNs = 2000;
  result.recordFill(sell1);

  Fill buy2;
  buy2.orderId = 3;
  buy2.symbol = 1;
  buy2.side = Side::BUY;
  buy2.price = Price::fromDouble(120.0);
  buy2.quantity = Quantity::fromDouble(1.0);
  buy2.timestampNs = 3000;
  result.recordFill(buy2);

  Fill sell2;
  sell2.orderId = 4;
  sell2.symbol = 1;
  sell2.side = Side::SELL;
  sell2.price = Price::fromDouble(100.0);
  sell2.quantity = Quantity::fromDouble(1.0);
  sell2.timestampNs = 4000;
  result.recordFill(sell2);

  auto stats = result.computeStats();
  EXPECT_DOUBLE_EQ(stats.totalPnl, 0.0);
  EXPECT_DOUBLE_EQ(stats.maxDrawdown, 20.0);
}

TEST_F(BacktestTest, BacktestRunnerWithGeneratedData)
{
  replay::WriterConfig writer_config{.output_dir = _test_dir};
  replay::BinaryLogWriter writer(writer_config);

  int64_t base_ts = 1000000000LL;

  std::vector<double> prices = {100.0, 101.0, 102.0, 103.0, 104.0, 105.0};

  for (size_t i = 0; i < prices.size(); ++i)
  {
    int64_t ts = base_ts + static_cast<int64_t>(i) * 1000000000LL;

    replay::BookRecordHeader book_header{};
    book_header.exchange_ts_ns = ts;
    book_header.recv_ts_ns = ts + 100;
    book_header.seq = static_cast<uint32_t>(i);
    book_header.symbol_id = 1;
    book_header.bid_count = 1;
    book_header.ask_count = 1;
    book_header.type = 0;

    std::vector<replay::BookLevel> bids = {
        {static_cast<int64_t>((prices[i] - 0.5) * 1000000), 10 * 1000000}};
    std::vector<replay::BookLevel> asks = {
        {static_cast<int64_t>((prices[i] + 0.5) * 1000000), 10 * 1000000}};

    writer.writeBook(book_header, bids, asks);

    replay::TradeRecord trade{};
    trade.exchange_ts_ns = ts + 500;
    trade.recv_ts_ns = ts + 600;
    trade.price_raw = static_cast<int64_t>(prices[i] * 1000000);
    trade.qty_raw = 1 * 1000000;
    trade.trade_id = static_cast<uint32_t>(i);
    trade.symbol_id = 1;
    trade.side = 1;

    writer.writeTrade(trade);
  }

  writer.close();

  replay::ReaderFilter filter;
  filter.symbols = {1};

  auto reader = replay::createMultiSegmentReader(_test_dir, filter);

  BacktestRunner runner;

  size_t trade_count = 0;
  size_t book_count = 0;

  class CountingStrategy : public IStrategy
  {
   public:
    CountingStrategy(size_t& tc, size_t& bc) : _trade_count(tc), _book_count(bc) {}
    SubscriberId id() const override { return 1; }
    void onTrade(const TradeEvent&) override { _trade_count++; }
    void onBookUpdate(const BookUpdateEvent&) override { _book_count++; }

   private:
    size_t& _trade_count;
    size_t& _book_count;
  };

  CountingStrategy strategy(trade_count, book_count);
  runner.setStrategy(&strategy);

  runner.run(*reader);

  EXPECT_EQ(trade_count, prices.size());
  EXPECT_EQ(book_count, prices.size());
}

TEST_F(BacktestTest, BacktestStatsCalculation)
{
  BacktestConfig config;
  config.feeRate = 0.0;  // No fees for this test
  BacktestResult result(config);

  std::vector<std::pair<double, double>> trades = {
      {100.0, 110.0}, {110.0, 105.0}, {105.0, 115.0}, {115.0, 112.0}, {112.0, 120.0}};

  OrderId orderId = 0;
  UnixNanos ts = 0;

  for (const auto& [entry, exit] : trades)
  {
    Fill buyFill;
    buyFill.orderId = ++orderId;
    buyFill.symbol = 1;
    buyFill.side = Side::BUY;
    buyFill.price = Price::fromDouble(entry);
    buyFill.quantity = Quantity::fromDouble(1.0);
    buyFill.timestampNs = ts++;
    result.recordFill(buyFill);

    Fill sellFill;
    sellFill.orderId = ++orderId;
    sellFill.symbol = 1;
    sellFill.side = Side::SELL;
    sellFill.price = Price::fromDouble(exit);
    sellFill.quantity = Quantity::fromDouble(1.0);
    sellFill.timestampNs = ts++;
    result.recordFill(sellFill);
  }

  auto stats = result.computeStats();

  EXPECT_EQ(stats.totalTrades, 5);
  EXPECT_EQ(stats.winningTrades, 3);
  EXPECT_EQ(stats.losingTrades, 2);
  EXPECT_DOUBLE_EQ(stats.winRate, 0.6);

  double expectedPnl = (110 - 100) + (105 - 110) + (115 - 105) + (112 - 115) + (120 - 112);
  EXPECT_DOUBLE_EQ(stats.totalPnl, expectedPnl);

  EXPECT_GT(stats.profitFactor, 0);
}
