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
#include "flox/connector/connector_manager.h"
#include "flox/connector/exchange_connector.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>
#include <string>

using namespace flox;
using ::testing::_;
using ::testing::Invoke;
using ::testing::Return;

class MockExchangeConnector : public ExchangeConnector
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

  EXPECT_CALL(*connector, start()).WillOnce(Invoke([&]
                                                   { connector->triggerTestData(); }));

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
