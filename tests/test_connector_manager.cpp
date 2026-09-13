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
#include "flox/common.h"
#include "flox/connector/abstract_exchange_connector.h"
#include "flox/connector/connector_manager.h"
#include "flox/util/memory/pool.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>
#include <string>

using namespace flox;
using ::testing::_;
using ::testing::Return;

class MockExchangeConnector : public IExchangeConnector
{
 public:
  MOCK_METHOD(void, start, (), (override));
  MOCK_METHOD(void, stop, (), (override));
  MOCK_METHOD(std::string, exchangeId, (), (const, override));
  void setCallbacks(BookUpdateCallback book, TradeCallback trade) override
  {
    _bookCb = std::move(book);
    _tradeCb = std::move(trade);
    onCallbacksSet();
  }
  MOCK_METHOD(void, onCallbacksSet, ());

  BookUpdateCallback _bookCb;
  TradeCallback _tradeCb;

  void triggerTestData()
  {
    if (_bookCb && _tradeCb)
    {
      pool::Pool<BookUpdateEvent, 3> bookUpdatePool;

      auto buOpt = bookUpdatePool.acquire();
      assert(buOpt);

      auto& bu = *buOpt;
      bu->update.symbol = 42;

      TradeEvent tradeEvent;
      tradeEvent.trade.symbol = 42;
      tradeEvent.trade.price = Price::fromDouble(3.14);

      _bookCb(*bu);
      _tradeCb(tradeEvent);
    }
  }
};

TEST(ConnectorManagerTest, RegisterAndStartAll)
{
  auto connector = std::make_shared<MockExchangeConnector>();
  ConnectorManager manager;

  EXPECT_CALL(*connector, exchangeId()).WillOnce(Return("bybit"));

  EXPECT_CALL(*connector, onCallbacksSet()).Times(1);

  EXPECT_CALL(*connector, start()).WillOnce([&]
                                            { connector->triggerTestData(); });

  manager.registerConnector(connector);

  bool bookUpdateCalled = false;
  bool tradeCalled = false;

  manager.startAll(
      [&](const BookUpdateEvent& event)
      {
        EXPECT_EQ(event.update.symbol, 42);
        bookUpdateCalled = true;
      },
      [&](const TradeEvent& event)
      {
        EXPECT_EQ(event.trade.symbol, 42);
        EXPECT_EQ(event.trade.price, Price::fromDouble(3.14));
        tradeCalled = true;
      });

  EXPECT_TRUE(bookUpdateCalled);
  EXPECT_TRUE(tradeCalled);
}

// POS-08: startAll used to move the caller's callbacks into a fresh lambda on
// every loop iteration. With two connectors, only the first (lexicographically
// smallest exchangeId(), since `connectors` is a std::map) kept a live
// callback; the second's copy of the moved-from MoveOnlyFunction was non-null
// but empty, and calling it through operator() dereferenced a null vtable
// pointer (SIGSEGV, exit 139 in the standalone repro). This registers two
// connectors and fires an event on the SECOND one only.
TEST(ConnectorManagerTest, StartAllWiresCallbacksToEveryConnectorNotJustTheFirst)
{
  auto first = std::make_shared<MockExchangeConnector>();
  auto second = std::make_shared<MockExchangeConnector>();
  ConnectorManager manager;

  EXPECT_CALL(*first, exchangeId()).WillRepeatedly(Return("aaa_binance"));
  EXPECT_CALL(*second, exchangeId()).WillRepeatedly(Return("bbb_bybit"));

  EXPECT_CALL(*first, onCallbacksSet()).Times(1);
  EXPECT_CALL(*second, onCallbacksSet()).Times(1);

  // Neither mock fires from start(); the test triggers the second connector's
  // event explicitly after startAll() has wired both, exactly like the
  // repro's "fire on connector #1 first (fine), then on #2 (used to crash)".
  EXPECT_CALL(*first, start()).Times(1);
  EXPECT_CALL(*second, start()).Times(1);

  manager.registerConnector(first);
  manager.registerConnector(second);

  int bookUpdateCount = 0;
  int tradeCount = 0;

  manager.startAll(
      [&](const BookUpdateEvent&)
      { ++bookUpdateCount; },
      [&](const TradeEvent&)
      { ++tradeCount; });

  first->triggerTestData();
  EXPECT_EQ(bookUpdateCount, 1);
  EXPECT_EQ(tradeCount, 1);

  // Before the fix this call on the second (non-first) connector dereferenced
  // a moved-from MoveOnlyFunction.
  second->triggerTestData();
  EXPECT_EQ(bookUpdateCount, 2);
  EXPECT_EQ(tradeCount, 2);
}

// CONN-12: ConnectorManager had no way to stop what it started, and its
// destructor let shared_ptr-owned connectors go out of scope while still
// running. stopAll() must reach every registered connector, and the
// destructor must call it.
TEST(ConnectorManagerTest, StopAllStopsEveryRegisteredConnector)
{
  auto first = std::make_shared<MockExchangeConnector>();
  auto second = std::make_shared<MockExchangeConnector>();

  EXPECT_CALL(*first, exchangeId()).WillRepeatedly(Return("aaa"));
  EXPECT_CALL(*second, exchangeId()).WillRepeatedly(Return("bbb"));
  EXPECT_CALL(*first, onCallbacksSet()).Times(1);
  EXPECT_CALL(*second, onCallbacksSet()).Times(1);
  EXPECT_CALL(*first, start()).Times(1);
  EXPECT_CALL(*second, start()).Times(1);
  // AtLeast(1), not exactly 1: the manager's destructor calls stopAll() again
  // at the end of this scope, on top of the explicit call below. stop() is
  // idempotent in every real connector, so the double call is harmless and
  // not itself under test here -- what's under test is that the explicit
  // stopAll() reaches every registered connector, not just one.
  EXPECT_CALL(*first, stop()).Times(::testing::AtLeast(1));
  EXPECT_CALL(*second, stop()).Times(::testing::AtLeast(1));

  {
    ConnectorManager manager;
    manager.registerConnector(first);
    manager.registerConnector(second);
    manager.startAll([](const BookUpdateEvent&) {}, [](const TradeEvent&) {});
    manager.stopAll();
  }
}

TEST(ConnectorManagerTest, DestructorStopsRunningConnectorsWithoutExplicitStopAll)
{
  auto connector = std::make_shared<MockExchangeConnector>();

  EXPECT_CALL(*connector, exchangeId()).WillRepeatedly(Return("aaa"));
  EXPECT_CALL(*connector, onCallbacksSet()).Times(1);
  EXPECT_CALL(*connector, start()).Times(1);
  EXPECT_CALL(*connector, stop()).Times(1);

  {
    ConnectorManager manager;
    manager.registerConnector(connector);
    manager.startAll([](const BookUpdateEvent&) {}, [](const TradeEvent&) {});
    // manager goes out of scope here without an explicit stopAll() call.
  }
}
