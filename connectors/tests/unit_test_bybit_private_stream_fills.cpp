/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline protocol test: Bybit's private "order" and "execution"
 * WS topics used to leave OrderEvent::fillQty at its default of zero on
 * every partial fill, so the canonical typed callback
 * IOrderExecutionListener::onOrderPartiallyFilled(order, fillQty) -- the
 * path OrderExecutionBus actually dispatches through (see
 * EventDispatcher<OrderEvent>::dispatch, which calls OrderEvent::dispatchTo)
 * -- received fillQty=0 no matter how much of the order actually filled.
 * The "execution" branch additionally overwrote order.quantity with the
 * single fill's size instead of the order's real size.
 *
 * These tests feed each topic in isolation, subscribe a real
 * IOrderExecutionListener to a real OrderExecutionBus, and assert on what
 * the canonical dispatch path actually delivers -- not just on the raw
 * OrderEvent struct fields.
 *
 * Out of scope, and not claimed fixed here: Bybit republishes the same
 * underlying fill on both the "order" and "execution" topics
 * independently, so a consumer naively summing fillQty across *both*
 * topics at once still double-counts. That is a separate design question
 * (which topic is authoritative, or whether to de-duplicate) from the
 * "fillQty is always zero" bug these tests pin.
 */

#include "flox-connectors/bybit/bybit_exchange_connector.h"

#include <flox/book/bus/book_update_bus.h>
#include <flox/book/bus/trade_bus.h>
#include <flox/common.h>
#include <flox/engine/symbol_registry.h>
#include <flox/execution/abstract_execution_listener.h>
#include <flox/execution/bus/order_execution_bus.h>
#include <flox/execution/events/order_event.h>
#include <flox/log/atomic_logger.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

using namespace flox;

namespace
{

std::string tempLogDir()
{
  auto dir = std::filesystem::temp_directory_path() / "flox_bybit_private_fill_test_logs";
  std::filesystem::create_directories(dir);
  return dir.string();
}

struct SeenFill
{
  OrderEventStatus dispatchedAs;  // PARTIALLY_FILLED or FILLED, via which typed callback fired
  double orderQuantity;
  double orderFilledQuantity;
  double fillQty;  // the argument the canonical callback actually received
};

class RecordingListener final : public IOrderExecutionListener
{
 public:
  RecordingListener() : IOrderExecutionListener(42) {}

  void onOrderPartiallyFilled(const Order& order, Quantity fillQty) override
  {
    std::lock_guard<std::mutex> lk(_m);
    _fills.push_back({OrderEventStatus::PARTIALLY_FILLED, order.quantity.toDouble(),
                      order.filledQuantity.toDouble(), fillQty.toDouble()});
  }

  void onOrderFilled(const Order& order) override
  {
    std::lock_guard<std::mutex> lk(_m);
    // onOrderFilled carries no fillQty parameter (the order is complete by
    // definition); the last increment is order.quantity - filledQuantity
    // as it stood just before this event, which for a clean fill sequence
    // is whatever remained.
    _fills.push_back({OrderEventStatus::FILLED, order.quantity.toDouble(),
                      order.filledQuantity.toDouble(), -1.0});
  }

  std::vector<SeenFill> fills()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _fills;
  }

 private:
  std::mutex _m;
  std::vector<SeenFill> _fills;
};

struct Fixture
{
  BookUpdateBus bookBus;
  TradeBus tradeBus;
  OrderExecutionBus orderBus;
  SymbolRegistry registry;
  std::shared_ptr<AtomicLogger> logger;
  std::unique_ptr<BybitExchangeConnector> connector;
  RecordingListener listener;

  explicit Fixture(const char* logName)
  {
    AtomicLoggerOptions logOpts;
    logOpts.directory = tempLogDir();
    logOpts.basename = logName;
    logger = std::make_shared<AtomicLogger>(logOpts);

    BybitConfig cfg;
    cfg.privateEndpoint = "wss://unused.invalid";
    cfg.enablePrivate = true;
    connector = std::make_unique<BybitExchangeConnector>(cfg, &bookBus, &tradeBus, &orderBus,
                                                         &registry, logger);

    orderBus.subscribe(&listener);
    bookBus.start();
    tradeBus.start();
    orderBus.start();
  }

  ~Fixture()
  {
    bookBus.stop();
    tradeBus.stop();
    orderBus.stop();
  }
};

}  // namespace

// A 3.0-quantity order filled entirely via the "execution" topic, in three
// 1.0 increments, with orderQty/leavesQty present on every frame.
TEST(BybitPrivateStreamFills, ExecutionTopicReportsRealFillQtyAndOrderSize)
{
  Fixture f("bybit_exec_fills.log");

  f.connector->handlePrivateMessage(
      R"({"topic":"execution","data":[{"orderId":"5001","symbol":"BTCUSDT","side":"Buy",)"
      R"("execPrice":"60000","execQty":"1","orderQty":"3","leavesQty":"2","execTime":1000,)"
      R"("execType":"Trade"}]})");
  f.connector->handlePrivateMessage(
      R"({"topic":"execution","data":[{"orderId":"5001","symbol":"BTCUSDT","side":"Buy",)"
      R"("execPrice":"60000","execQty":"1","orderQty":"3","leavesQty":"1","execTime":1001,)"
      R"("execType":"Trade"}]})");
  f.connector->handlePrivateMessage(
      R"({"topic":"execution","data":[{"orderId":"5001","symbol":"BTCUSDT","side":"Buy",)"
      R"("execPrice":"60000","execQty":"1","orderQty":"3","leavesQty":"0","execTime":1002,)"
      R"("execType":"Trade"}]})");

  f.orderBus.flush();

  const auto fills = f.listener.fills();
  ASSERT_EQ(fills.size(), 3u);

  // Before the fix, fillQty was hardcoded to the default (0) on every one
  // of these; that is the exact regression this test pins. The third fill
  // completes the order, so it dispatches through onOrderFilled (no
  // fillQty parameter -- the sentinel -1 the test listener records for
  // that callback) rather than onOrderPartiallyFilled.
  EXPECT_DOUBLE_EQ(fills[0].fillQty, 1.0);
  EXPECT_DOUBLE_EQ(fills[1].fillQty, 1.0);
  EXPECT_EQ(fills[2].dispatchedAs, OrderEventStatus::FILLED);

  double naiveSum = fills[0].fillQty + fills[1].fillQty +
                    (fills[2].orderFilledQuantity - fills[1].orderFilledQuantity);
  EXPECT_DOUBLE_EQ(naiveSum, 3.0) << "true order size is 3; the naive sum must not be 0 (all "
                                     "fillQty zero) or otherwise wrong";

  // order.quantity must be the order's real size (3), not execQty (1) --
  // the field the pre-fix code overwrote with the fill size.
  EXPECT_DOUBLE_EQ(fills[0].orderQuantity, 3.0);
  EXPECT_DOUBLE_EQ(fills[1].orderQuantity, 3.0);
  EXPECT_DOUBLE_EQ(fills[2].orderQuantity, 3.0);

  // Last frame has leavesQty=0 -> the order is fully filled, so the
  // canonical status-reachable-at-all check: FILLED must actually fire
  // (it was unreachable from this branch before the fix).
  EXPECT_EQ(fills[2].dispatchedAs, OrderEventStatus::FILLED);
  EXPECT_DOUBLE_EQ(fills[2].orderFilledQuantity, 3.0);
}

// Same 3.0-quantity order, this time reported purely via the "order" topic
// (which carries cumulative filledQuantity, not a delta): New, then two
// PartiallyFilled steps, then Filled.
TEST(BybitPrivateStreamFills, OrderTopicDerivesIncrementalFillQtyFromCumulative)
{
  Fixture f("bybit_order_fills.log");

  f.connector->handlePrivateMessage(
      R"({"topic":"order","data":[{"symbol":"BTCUSDT","orderId":"5002","side":"Buy",)"
      R"("price":"60000","qty":"3","cumExecQty":"0","updatedTime":1000,)"
      R"("orderStatus":"New"}]})");
  f.connector->handlePrivateMessage(
      R"({"topic":"order","data":[{"symbol":"BTCUSDT","orderId":"5002","side":"Buy",)"
      R"("price":"60000","qty":"3","cumExecQty":"1","updatedTime":1001,)"
      R"("orderStatus":"PartiallyFilled"}]})");
  f.connector->handlePrivateMessage(
      R"({"topic":"order","data":[{"symbol":"BTCUSDT","orderId":"5002","side":"Buy",)"
      R"("price":"60000","qty":"3","cumExecQty":"2","updatedTime":1002,)"
      R"("orderStatus":"PartiallyFilled"}]})");
  f.connector->handlePrivateMessage(
      R"({"topic":"order","data":[{"symbol":"BTCUSDT","orderId":"5002","side":"Buy",)"
      R"("price":"60000","qty":"3","cumExecQty":"3","updatedTime":1003,)"
      R"("orderStatus":"Filled"}]})");

  f.orderBus.flush();

  const auto fills = f.listener.fills();
  // "New" carries no fill and does not dispatch through onOrderPartiallyFilled
  // or onOrderFilled (it dispatches onOrderSubmitted, which this listener
  // does not record).
  ASSERT_EQ(fills.size(), 3u);

  EXPECT_DOUBLE_EQ(fills[0].fillQty, 1.0);  // 0 -> 1
  EXPECT_DOUBLE_EQ(fills[1].fillQty, 1.0);  // 1 -> 2
  EXPECT_EQ(fills[2].dispatchedAs, OrderEventStatus::FILLED);

  double naiveSum = fills[0].fillQty + fills[1].fillQty;
  EXPECT_DOUBLE_EQ(naiveSum, 2.0) << "cumulative deltas up to (but not including) the terminal "
                                     "FILLED event should sum to filledQuantity just before it";
}
