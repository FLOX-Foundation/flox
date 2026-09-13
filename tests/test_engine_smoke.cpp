/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/book/bus/book_update_bus.h"
#include "flox/book/bus/trade_bus.h"
#include "flox/book/events/book_update_event.h"
#include "flox/book/events/trade_event.h"
#include "flox/common.h"
#include "flox/engine/abstract_subscriber.h"
#include "flox/engine/abstract_subsystem.h"
#include "flox/engine/engine.h"
#include "flox/strategy/abstract_strategy.h"

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace flox;

// Smoke test: demonstrates how to create a strategy, wire it into the engine,
// publish trades and book updates via a mock connector, and receive results.

namespace
{

class TestStrategy : public IStrategy
{
 public:
  explicit TestStrategy(SymbolId symbol) : _symbol(symbol) {}

  SubscriberId id() const override { return 1; }

  void onTrade(const TradeEvent& event) override
  {
    if (event.trade.symbol == _symbol)
    {
      ++_seenTrades;
      _lastTradePrice = event.trade.price;
    }
  }

  void onBookUpdate(const BookUpdateEvent& event) override
  {
    if (event.update.symbol == _symbol && !event.update.bids.empty())
    {
      ++_seenBooks;
      _lastBid = event.update.bids[0].price;
    }
  }

  int seenTrades() const
  {
    return _seenTrades;
  }
  int seenBooks() const
  {
    return _seenBooks;
  }
  Price lastTradePrice() const
  {
    return _lastTradePrice;
  }
  Price lastBid() const
  {
    return _lastBid;
  }

 private:
  SymbolId _symbol;
  int _seenTrades = 0;
  int _seenBooks = 0;
  Price _lastTradePrice = Price::fromDouble(0.0);
  Price _lastBid = Price::fromDouble(0.0);
};

using BookUpdatePool = pool::Pool<BookUpdateEvent, 7>;

class MockConnector
{
 public:
  MockConnector(BookUpdateBus& bookUpdateBus, TradeBus& tradeBus, BookUpdatePool& bookPool,
                SymbolId symbol)
      : _bookUpdateBus(bookUpdateBus), _tradeBus(tradeBus), _bookPool(bookPool), _symbol(symbol)
  {
  }

  void publishTrade(Price price, Quantity qty)
  {
    TradeEvent event;

    event.trade.symbol = _symbol;
    event.trade.price = price;
    event.trade.quantity = qty;
    event.trade.isBuy = true;
    event.trade.exchangeTsNs = UnixNanos::fromRaw(static_cast<int64_t>(nowNsMonotonic()));  // synthetic tape

    _tradeBus.publish(event);
  }

  void publishBook(Price bidPrice, Quantity bidQty)
  {
    auto eventOpt = _bookPool.acquire();
    assert(eventOpt);
    auto& event = *eventOpt;
    event->update.symbol = _symbol;
    event->update.type = BookUpdateType::SNAPSHOT;
    event->update.bids.push_back({bidPrice, bidQty});

    _bookUpdateBus.publish(std::move(event));
  }

 private:
  BookUpdateBus& _bookUpdateBus;
  TradeBus& _tradeBus;
  BookUpdatePool& _bookPool;
  SymbolId _symbol;
};

class SmokeEngineBuilder
{
 public:
  SmokeEngineBuilder(SymbolId symbol, TestStrategy* strategy)
      : _symbol(symbol), _strategy(strategy)
  {
  }

  class EngineImpl : public ISubsystem
  {
   public:
    EngineImpl(BookUpdateBus& bookUpdateBus, TradeBus& tradeBus, MockConnector& connector, TestStrategy* strategy)
        : _bookUpdateBus(bookUpdateBus), _tradeBus(tradeBus), _connector(connector), _strategy(strategy)
    {
    }

    void start() override
    {
      _bookUpdateBus.start();
      _tradeBus.start();
    }
    void stop() override
    {
      _bookUpdateBus.stop();
      _tradeBus.stop();
    }

    void runTrade(Price price, Quantity qty) { _connector.publishTrade(price, qty); }
    void runBook(Price price, Quantity qty) { _connector.publishBook(price, qty); }

   private:
    BookUpdateBus& _bookUpdateBus;
    TradeBus& _tradeBus;
    MockConnector& _connector;
    TestStrategy* _strategy;
  };

  std::unique_ptr<EngineImpl> build()
  {
    _bookUpdateBus = std::make_unique<BookUpdateBus>();
    _tradeBus = std::make_unique<TradeBus>();
    _bookPool = std::make_unique<BookUpdatePool>();

    _bookUpdateBus->enableDrainOnStop();
    _tradeBus->enableDrainOnStop();

    _connector = std::make_unique<MockConnector>(*_bookUpdateBus, *_tradeBus, *_bookPool, _symbol);

    _bookUpdateBus->subscribe(_strategy);
    _tradeBus->subscribe(_strategy);

    return std::make_unique<EngineImpl>(*_bookUpdateBus, *_tradeBus, *_connector, _strategy);
  }

  SymbolId _symbol;
  TestStrategy* _strategy;
  std::unique_ptr<BookUpdateBus> _bookUpdateBus;
  std::unique_ptr<TradeBus> _tradeBus;
  std::unique_ptr<BookUpdatePool> _bookPool;
  std::unique_ptr<MockConnector> _connector;
};

}  // namespace

TEST(SmokeEngineTest, StrategyReceivesBothEvents)
{
  constexpr SymbolId SYMBOL = 777;
  auto strategy = std::make_unique<TestStrategy>(SYMBOL);
  SmokeEngineBuilder builder(SYMBOL, strategy.get());
  auto engineBase = builder.build();
  auto engine = static_cast<SmokeEngineBuilder::EngineImpl*>(engineBase.get());

  engine->start();
  engine->runTrade(Price::fromDouble(101.25), Quantity::fromDouble(10.0));
  engine->runBook(Price::fromDouble(101.10), Quantity::fromDouble(5.0));
  engine->stop();

  EXPECT_EQ(strategy->seenTrades(), 1);
  EXPECT_EQ(strategy->seenBooks(), 1);
  EXPECT_EQ(strategy->lastTradePrice(), Price::fromDouble(101.25));
  EXPECT_EQ(strategy->lastBid(), Price::fromDouble(101.10));
}

namespace
{

// Records the order in which the engine touches its subsystems.
class OrderSpy : public ISubsystem
{
 public:
  OrderSpy(std::string name, std::vector<std::string>& log) : _name(std::move(name)), _log(log) {}

  void start() override { _log.push_back("start:" + _name); }
  void stop() override { _log.push_back("stop:" + _name); }

 private:
  std::string _name;
  std::vector<std::string>& _log;
};

}  // namespace

// Subsystems come up in dependency order and go down in the reverse of it --
// the bus before its publishers on the way up, after them on the way down. The
// second cycle has to hold the same order as the first: an engine that is
// stopped and started again (a reconnect, a session roll) must not bring the
// strategy up before the bus it publishes into.
TEST(SmokeEngineTest, SubsystemOrderSurvivesASecondCycle)
{
  std::vector<std::string> log;
  std::vector<std::unique_ptr<ISubsystem>> subsystems;
  subsystems.push_back(std::make_unique<OrderSpy>("bus", log));
  subsystems.push_back(std::make_unique<OrderSpy>("risk", log));
  subsystems.push_back(std::make_unique<OrderSpy>("strategy", log));

  EngineConfig cfg;
  Engine engine(cfg, std::move(subsystems), {});

  engine.start();
  engine.stop();
  const std::vector<std::string> cycle1 = log;
  log.clear();

  engine.start();
  engine.stop();

  const std::vector<std::string> expected{"start:bus", "start:risk", "start:strategy",
                                          "stop:strategy", "stop:risk", "stop:bus"};
  EXPECT_EQ(cycle1, expected);
  EXPECT_EQ(log, expected) << "the second cycle ran the subsystems in a different order than the "
                              "first";
}
