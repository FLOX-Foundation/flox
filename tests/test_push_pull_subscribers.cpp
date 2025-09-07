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

#include "flox/book/bus/book_update_bus.h"
#include "flox/book/events/book_update_event.h"
#include "flox/engine/abstract_market_data_subscriber.h"
#include "flox/engine/abstract_subscriber.h"
#include "flox/util/concurrency/spsc_queue.h"

using namespace flox;

struct CountingMdSub : IMarketDataSubscriber
{
  std::atomic<int>& cnt;
  explicit CountingMdSub(std::atomic<int>& c) : cnt(c) {}
  void onBookUpdate(const BookUpdateEvent&) override { ++cnt; }

  SubscriberId id() const override { return 11; }
};

struct BookUpdateDto
{
  SymbolId symbol{};
  double bestBid{};
  double bestAsk{};
  uint64_t tick{};
};

struct QueuingMdSub : IMarketDataSubscriber
{
  using Q = SPSCQueue<BookUpdateDto, 128>;
  Q q;

  void onBookUpdate(const BookUpdateEvent& ev) override
  {
    BookUpdateDto d;
    d.symbol = ev.update.symbol;
    d.tick = ev.seq;
    if (!ev.update.bids.empty())
    {
      d.bestBid = ev.update.bids[0].price.toDouble();
    }
    if (!ev.update.asks.empty())
    {
      d.bestAsk = ev.update.asks[0].price.toDouble();
    }
    q.try_emplace(d);
  }

  SubscriberId id() const override { return 1; }
};

static pool::Handle<BookUpdateEvent> make_book(pool::Pool<BookUpdateEvent, 7>& pool, double px)
{
  auto h = pool.acquire();
  EXPECT_TRUE(h.has_value());
  h->get()->update.type = BookUpdateType::SNAPSHOT;
  h->get()->update.bids = {{Price::fromDouble(px), Quantity::fromDouble(1.0)}};
  return std::move(*h);
}

TEST(BookBus_Disruptor, OptionalConsumerQueuesLocally)
{
  auto bus = std::make_unique<BookUpdateBus>();

  std::atomic<int> gated{0};

  CountingMdSub required(gated);
  QueuingMdSub optional;

  bus->subscribe(&required);
  bus->subscribe(&optional);
  bus->start();

  pool::Pool<BookUpdateEvent, 7> pool;
  const auto seq = bus->publish(make_book(pool, 200.0));
  bus->waitConsumed(seq);

  auto* h = optional.q.try_pop();
  ASSERT_NE(h, nullptr);
  EXPECT_EQ(h->bestBid, 200.0);

  EXPECT_EQ(gated.load(), 1);

  bus->stop();
}

TEST(BookBus_Disruptor, MixedRequiredAndOptionalBothSeeEvent)
{
  BookUpdateBus bus;
  std::atomic<int> gated{0};

  CountingMdSub req(gated);
  QueuingMdSub opt;

  bus.subscribe(&req);
  bus.subscribe(&opt);
  bus.start();

  pool::Pool<BookUpdateEvent, 7> pool;
  const auto seq = bus.publish(make_book(pool, 105.5));
  bus.waitConsumed(seq);

  EXPECT_EQ(gated.load(), 1);

  auto* h = opt.q.try_pop();
  ASSERT_NE(h, nullptr);
  EXPECT_EQ(h->bestBid, 105.5);

  bus.stop();
}
