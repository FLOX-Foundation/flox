/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "flox/book/bus/book_update_bus.h"
#include "flox/book/events/book_update_event.h"
#include "flox/common.h"
#include "flox/engine/abstract_market_data_subscriber.h"

using namespace flox;

namespace
{

using BookUpdatePool = pool::Pool<BookUpdateEvent, 63>;

class TestSubscriber : public IMarketDataSubscriber
{
 public:
  explicit TestSubscriber(SubscriberId id, std::atomic<int>& counter) : _id(id), _counter(counter)
  {
  }

  void onBookUpdate(const BookUpdateEvent& book) override
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ++_counter;
    _lastPrice.store(book.update.bids.empty() ? Price::fromRaw(-1).raw()
                                              : book.update.bids[0].price.raw());
  }

  SubscriberId id() const override { return _id; }
  SubscriberMode mode() const override { return SubscriberMode::PUSH; }

  double lastPrice() const { return static_cast<double>(_lastPrice.load()) / Price::Scale; }

 private:
  SubscriberId _id;
  std::atomic<int>& _counter;
  std::atomic<int64_t> _lastPrice{Price::fromRaw(-1).raw()};
};

TEST(MarketDataBusTest, SingleSubscriberReceivesUpdates)
{
  BookUpdateBus bus;
  std::atomic<int> receivedCount{0};

  auto subscriber = std::make_shared<TestSubscriber>(1, receivedCount);
  bus.subscribe(subscriber);

  bus.start();

  BookUpdatePool pool;
  for (int i = 0; i < 1; ++i)
  {
    auto opt = pool.acquire();
    EXPECT_EQ(opt.has_value(), true);

    auto& update = *opt;
    ASSERT_NE(update.get(), nullptr);
    update->update.type = BookUpdateType::SNAPSHOT;
    update->update.bids.emplace_back(Price::fromDouble(100.0 + 1), Quantity::fromDouble(1.0));
    bus.publish(std::move(update));
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  bus.stop();

  EXPECT_GE(receivedCount, 1);
  EXPECT_NE(subscriber->lastPrice(), -1.0);
  EXPECT_EQ(pool.inUse(), 0);
}

TEST(MarketDataBusTest, MultipleSubscribersReceiveAll)
{
  BookUpdateBus bus;
  std::atomic<int> received1{0};
  std::atomic<int> received2{0};

  auto sub1 = std::make_shared<TestSubscriber>(1, received1);
  auto sub2 = std::make_shared<TestSubscriber>(2, received2);

  bus.subscribe(sub1);
  bus.subscribe(sub2);

  bus.start();

  BookUpdatePool pool;
  for (int i = 0; i < 20; ++i)
  {
    auto opt = pool.acquire();
    EXPECT_EQ(opt.has_value(), true);

    auto& update = *opt;
    ASSERT_NE(update.get(), nullptr);
    update->update.type = BookUpdateType::SNAPSHOT;
    update->update.bids.emplace_back(Price::fromDouble(200.0 + i), Quantity::fromDouble(1.0));
    bus.publish(std::move(update));
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  bus.stop();

  EXPECT_GE(received1, 20);
  EXPECT_GE(received2, 20);
  EXPECT_NE(sub1->lastPrice(), -1.0);
  EXPECT_NE(sub2->lastPrice(), -1.0);
  EXPECT_EQ(pool.inUse(), 0);
}

TEST(MarketDataBusTest, GracefulStopDoesNotLeak)
{
  BookUpdateBus bus;
  std::atomic<int> count{0};
  bus.subscribe(std::make_shared<TestSubscriber>(1, count));

  bus.start();

  BookUpdatePool pool;
  for (int i = 0; i < 5; ++i)
  {
    auto opt = pool.acquire();
    EXPECT_EQ(opt.has_value(), true);

    auto& update = *opt;
    ASSERT_NE(update.get(), nullptr);
    update->update.type = BookUpdateType::SNAPSHOT;
    update->update.bids.emplace_back(Price::fromDouble(300.0 + i), Quantity::fromDouble(1.0));
    bus.publish(std::move(update));
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  bus.stop();

  EXPECT_GE(count.load(), 5);
  EXPECT_EQ(pool.inUse(), 0);
}

}  // namespace
