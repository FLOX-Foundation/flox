/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/book/events/book_update_event.h"
#include "flox/book/events/trade_event.h"
#include "flox/engine/abstract_market_data_subscriber.h"
#include "flox/util/eventing/event_bus.h"

#include <gtest/gtest.h>
#include <memory>

using namespace flox;

namespace
{

class DummySubscriber : public IMarketDataSubscriber
{
 public:
  SubscriberId id() const override { return 42; }
  SubscriberMode mode() const override { return SubscriberMode::PULL; }

  void onBookUpdate(const BookUpdateEvent&) override {}
  void onTrade(const TradeEvent&) override {}
};

using E = pool::Handle<BookUpdateEvent>;

TEST(EventBusInstantiationTest, CoversAllSpecializations)
{
  auto sub = std::make_shared<DummySubscriber>();

  {
    EventBus<E, AsyncPolicy<E>> bus;
    bus.subscribe(sub);
    auto* q = bus.getQueue(sub->id());
    (void)q;
    bus.start();
    bus.stop();
  }

  {
    EventBus<E, SyncPolicy<E>> bus;
    bus.subscribe(sub);
    auto* q = bus.getQueue(sub->id());
    (void)q;
    bus.start();
    bus.stop();
  }
}

}  // namespace
