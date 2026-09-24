/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline protocol tests for the live-fill contract on Bybit. Recorded-shape
 * private frames go straight into handlePrivateMessage and the assertions are
 * taken on the canonical dispatch path (IOrderExecutionListener through a real
 * OrderExecutionBus), which is what a position tracker actually sees.
 *
 * Three separate holes are pinned here:
 *
 *  - fillPrice. OrderEvent carries one and IOrderExecutionListener's fill
 *    callbacks take one, but no live connector assigns it, so every live fill
 *    reaches PositionTracker::onOrderPartiallyFilled(order, fillQty, 0) and
 *    cost basis / realized PnL are built at price zero.
 *
 *  - Double counting. The private stream subscribes to both "order" and
 *    "execution" and both branches publish a fill for the same underlying
 *    execution, so a tracker summing what the bus delivers books twice the
 *    quantity that actually traded. unit_test_bybit_private_stream_fills.cpp
 *    named this and left it out of scope; it is in scope here.
 *
 *  - Order identity. The stream writes the venue's orderId into the engine's
 *    OrderId field and the executor never sends orderLinkId, so every fill
 *    arrives under an id the engine never issued and matches no live order.
 *
 * Each test feeds only frames a real Bybit V5 private stream would send; no
 * sockets, no credentials.
 */

#include "flox-connectors/bybit/authenticated_rest_client.h"
#include "flox-connectors/bybit/bybit_exchange_connector.h"
#include "flox-connectors/bybit/bybit_order_executor.h"

#include <flox/book/bus/book_update_bus.h>
#include <flox/book/bus/trade_bus.h>
#include <flox/common.h>
#include <flox/engine/symbol_registry.h>
#include <flox/execution/abstract_execution_listener.h>
#include <flox/execution/bus/order_execution_bus.h>
#include <flox/execution/events/order_event.h>
#include <flox/execution/order_tracker.h>
#include <flox/log/atomic_logger.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace flox;

namespace
{

// The engine-issued OrderId under test. The venue's own id is deliberately a
// different, much larger number so the two can never be confused.
constexpr OrderId kEngineOrderId = 4242;
constexpr const char* kVenueOrderId = "99887766";

std::string tempLogDir()
{
  auto dir = std::filesystem::temp_directory_path() / "flox_bybit_fill_contract_logs";
  std::filesystem::create_directories(dir);
  return dir.string();
}

struct SeenFill
{
  OrderEventStatus dispatchedAs;
  OrderId orderId;
  double fillQty;
  double fillPrice;
  double orderQuantity;
};

// Records only the two fill callbacks -- the ones a position tracker uses to
// move a position. Both overloads that carry a price are overridden, so a fill
// dispatched through either arrives here with whatever price the connector set.
class FillListener final : public IOrderExecutionListener
{
 public:
  FillListener() : IOrderExecutionListener(43) {}

  void onOrderPartiallyFilled(const Order& order, Quantity fillQty, Price fillPrice) override
  {
    std::lock_guard<std::mutex> lk(_m);
    _fills.push_back({OrderEventStatus::PARTIALLY_FILLED, order.id, fillQty.toDouble(),
                      fillPrice.toDouble(), order.quantity.toDouble()});
  }

  void onOrderFilled(const Order& order, Quantity fillQty, Price fillPrice) override
  {
    std::lock_guard<std::mutex> lk(_m);
    _fills.push_back({OrderEventStatus::FILLED, order.id, fillQty.toDouble(), fillPrice.toDouble(),
                      order.quantity.toDouble()});
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
  FillListener listener;

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

// One "execution" frame, in the field order Bybit V5 sends.
std::string executionFrame(const char* execId, const char* execQty, const char* execPrice,
                           const char* leavesQty, int64_t execTimeMs)
{
  std::string s;
  s += R"({"topic":"execution","data":[{"symbol":"BTCUSDT","orderId":")";
  s += kVenueOrderId;
  s += R"(","orderLinkId":")";
  s += std::to_string(kEngineOrderId);
  s += R"(","side":"Buy","execId":")";
  s += execId;
  s += R"(","execPrice":")";
  s += execPrice;
  s += R"(","execQty":")";
  s += execQty;
  s += R"(","orderQty":"3","leavesQty":")";
  s += leavesQty;
  s += R"(","execTime":)";
  s += std::to_string(execTimeMs);
  s += R"(,"execType":"Trade"}]})";
  return s;
}

// One "order" frame, in the field order Bybit V5 sends.
std::string orderFrame(const char* orderStatus, const char* cumExecQty, const char* avgPrice,
                       int64_t updatedTimeMs)
{
  std::string s;
  s += R"({"topic":"order","data":[{"symbol":"BTCUSDT","orderId":")";
  s += kVenueOrderId;
  s += R"(","orderLinkId":")";
  s += std::to_string(kEngineOrderId);
  s += R"(","side":"Buy","price":"60000","qty":"3","avgPrice":")";
  s += avgPrice;
  s += R"(","cumExecQty":")";
  s += cumExecQty;
  s += R"(","updatedTime":)";
  s += std::to_string(updatedTimeMs);
  s += R"(,"orderStatus":")";
  s += orderStatus;
  s += R"("}]})";
  return s;
}

struct Call
{
  std::string url;
  std::string body;
};

class FakeTransport final : public ITransport
{
 public:
  void post(std::string_view url, std::string_view body,
            const std::vector<std::pair<std::string_view, std::string_view>>&,
            MoveOnlyFunction<void(std::string_view)> onSuccess,
            MoveOnlyFunction<void(std::string_view)>) override
  {
    calls.push_back({std::string(url), std::string(body)});
    if (onSuccess)
    {
      onSuccess(nextResponse);
    }
  }

  std::vector<Call> calls;
  std::string nextResponse = R"({"retCode":0,"retMsg":"OK","result":{"orderId":"99887766"}})";
};

}  // namespace

// REVIEW connectors/3. A fill whose price the venue reports must reach the
// listener with that price. The execution topic carries it as execPrice.
// Today fillPrice is left at its default, so a position opened on this fill is
// booked at a cost basis of zero.
TEST(BybitFillContract, ExecutionFillCarriesExecPrice)
{
  Fixture f("bybit_exec_fill_price.log");

  f.connector->handlePrivateMessage(executionFrame("E1", "1", "60000", "2", 1000));
  f.orderBus.flush();

  const auto fills = f.listener.fills();
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].dispatchedAs, OrderEventStatus::PARTIALLY_FILLED);
  EXPECT_DOUBLE_EQ(fills[0].fillQty, 1.0);
  EXPECT_DOUBLE_EQ(fills[0].fillPrice, 60000.0)
      << "execPrice=60000 must reach the fill callback; a fill at price 0 builds the position's "
         "cost basis at zero";
}

// REVIEW connectors/3, order topic. The same requirement on the other private
// topic: the order topic reports the order's average execution price, and a
// fill derived from it must carry a price too.
TEST(BybitFillContract, OrderTopicFillCarriesAveragePrice)
{
  Fixture f("bybit_order_fill_price.log");

  f.connector->handlePrivateMessage(orderFrame("New", "0", "0", 1000));
  f.connector->handlePrivateMessage(orderFrame("PartiallyFilled", "1", "60000", 1001));
  f.orderBus.flush();

  const auto fills = f.listener.fills();
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_DOUBLE_EQ(fills[0].fillQty, 1.0);
  EXPECT_DOUBLE_EQ(fills[0].fillPrice, 60000.0) << "avgPrice=60000 must reach the fill callback";
}

// REVIEW connectors/4. Bybit reports one underlying execution on both private
// topics. Here the venue fills 1 then 2 of a 3-lot order and announces each
// fill twice, once per topic. The bus must deliver two fills totalling 3.0 --
// today it delivers four, totalling 6.0, so any position tracker ends up long
// double what actually traded.
TEST(BybitFillContract, OrderAndExecutionFramesForOneFillDispatchOneFill)
{
  Fixture f("bybit_double_fill.log");

  f.connector->handlePrivateMessage(orderFrame("New", "0", "0", 1000));
  f.connector->handlePrivateMessage(executionFrame("E1", "1", "60000", "2", 1001));
  f.connector->handlePrivateMessage(orderFrame("PartiallyFilled", "1", "60000", 1001));
  f.connector->handlePrivateMessage(executionFrame("E2", "2", "60000", "0", 1002));
  f.connector->handlePrivateMessage(orderFrame("Filled", "3", "60000", 1002));
  f.orderBus.flush();

  const auto fills = f.listener.fills();
  ASSERT_EQ(fills.size(), 2u)
      << "two executions were reported on two topics each; exactly two fills must be dispatched";

  EXPECT_EQ(fills[0].dispatchedAs, OrderEventStatus::PARTIALLY_FILLED);
  EXPECT_DOUBLE_EQ(fills[0].fillQty, 1.0);
  EXPECT_DOUBLE_EQ(fills[0].fillPrice, 60000.0);

  EXPECT_EQ(fills[1].dispatchedAs, OrderEventStatus::FILLED);
  EXPECT_DOUBLE_EQ(fills[1].fillQty, 2.0);
  EXPECT_DOUBLE_EQ(fills[1].fillPrice, 60000.0);

  double total = 0.0;
  for (const auto& fill : fills)
  {
    total += fill.fillQty;
  }
  EXPECT_DOUBLE_EQ(total, 3.0) << "the order's size is 3; the bus must not deliver 6";

  EXPECT_DOUBLE_EQ(fills[0].orderQuantity, 3.0);
  EXPECT_DOUBLE_EQ(fills[1].orderQuantity, 3.0);
}

// REVIEW connectors/23. Every fill dispatched to the engine must carry the
// OrderId the engine issued, which Bybit echoes back as orderLinkId. Today the
// venue's numeric orderId is cast into the OrderId field, so the fill refers to
// an order no tracker has ever heard of.
TEST(BybitFillContract, FillCarriesEngineOrderIdFromOrderLinkId)
{
  Fixture f("bybit_fill_id.log");

  f.connector->handlePrivateMessage(executionFrame("E1", "1", "60000", "2", 1000));
  f.connector->handlePrivateMessage(orderFrame("PartiallyFilled", "1", "60000", 1000));
  f.orderBus.flush();

  const auto fills = f.listener.fills();
  ASSERT_FALSE(fills.empty());
  for (const auto& fill : fills)
  {
    EXPECT_EQ(fill.orderId, kEngineOrderId)
        << "the fill must be keyed by the engine's order id (orderLinkId=" << kEngineOrderId
        << "), not by the venue's orderId " << kVenueOrderId;
  }
}

// REVIEW connectors/23, outgoing half. orderLinkId can only come back on the
// private stream if the executor sends it on order/create in the first place.
// grep for it over connectors/ returns nothing today.
TEST(BybitFillContract, SubmitSendsEngineOrderIdAsOrderLinkId)
{
  auto transport = std::make_unique<FakeTransport>();
  auto* rawTransport = transport.get();
  auto client = std::make_unique<AuthenticatedRestClient>("key", "secret", "https://api.bybit.com",
                                                          rawTransport);

  SymbolRegistry registry;
  OrderTracker tracker;
  SymbolInfo info;
  info.exchange = "bybit";
  info.symbol = "BTCUSDT";
  info.type = InstrumentType::Future;
  SymbolId sym = registry.registerSymbol(info);

  BybitOrderExecutorT<NoPolicies> executor(std::move(client), &registry, &tracker);

  Order order;
  order.id = kEngineOrderId;
  order.symbol = sym;
  order.side = Side::BUY;
  order.type = OrderType::LIMIT;
  order.price = Price::fromDouble(60000.0);
  order.quantity = Quantity::fromDouble(3.0);

  executor.submitOrder(order);

  ASSERT_EQ(rawTransport->calls.size(), 1u);
  const std::string& body = rawTransport->calls[0].body;
  const std::string expected = R"("orderLinkId":"4242")";
  EXPECT_NE(body.find(expected), std::string::npos)
      << "order/create must carry the engine order id as orderLinkId so fills come back under it: "
      << body;
}
