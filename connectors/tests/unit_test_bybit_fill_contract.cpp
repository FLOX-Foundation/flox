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
#include <flox/book/composite_book_matrix.h>
#include <flox/book/events/book_update_event.h>
#include <flox/common.h>
#include <flox/engine/symbol_registry.h>
#include <flox/execution/abstract_execution_listener.h>
#include <flox/execution/bus/order_execution_bus.h>
#include <flox/execution/events/order_event.h>
#include <flox/execution/order_tracker.h>
#include <flox/log/atomic_logger.h>
#include <flox/util/base/time.h>

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

struct SeenBook
{
  uint64_t recvNs;
  uint64_t publishTsNs;
  ExchangeId sourceExchange;
  size_t bidCount;
  size_t askCount;
};

class BookCapture final : public IMarketDataSubscriber
{
 public:
  SubscriberId id() const override { return 44; }

  void onBookUpdate(const BookUpdateEvent& ev) override
  {
    std::lock_guard<std::mutex> lk(_m);
    _books.push_back({ev.recvNs.raw(), ev.publishTsNs.raw(), ev.sourceExchange,
                      ev.update.bids.size(), ev.update.asks.size()});
  }

  std::vector<SeenBook> books()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _books;
  }

 private:
  std::mutex _m;
  std::vector<SeenBook> _books;
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
  BookCapture bookCapture;
  CompositeBookMatrix<4> matrix;

  explicit Fixture(const char* logName)
  {
    AtomicLoggerOptions logOpts;
    logOpts.directory = tempLogDir();
    logOpts.basename = logName;
    logger = std::make_shared<AtomicLogger>(logOpts);

    BybitConfig cfg;
    cfg.publicEndpoint = "wss://unused.invalid";
    cfg.privateEndpoint = "wss://unused.invalid";
    cfg.enablePrivate = true;
    cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50}};
    connector = std::make_unique<BybitExchangeConnector>(cfg, &bookBus, &tradeBus, &orderBus,
                                                         &registry, logger);

    matrix.setId(45);
    orderBus.subscribe(&listener);
    bookBus.subscribe(&bookCapture);
    bookBus.subscribe(&matrix);
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

// One "execution" frame, in the field order Bybit V5 sends. `linkId` is the
// engine's own order id, echoed back as orderLinkId; a nullptr leaves the field
// out, which is what the venue does for an order this engine did not place.
std::string executionFrameFor(const char* linkId, const char* execId, const char* execQty,
                              const char* execPrice, const char* leavesQty, int64_t execTimeMs)
{
  std::string s;
  s += R"({"topic":"execution","data":[{"symbol":"BTCUSDT","orderId":")";
  s += kVenueOrderId;
  s += R"(")";
  if (linkId)
  {
    s += R"(,"orderLinkId":")";
    s += linkId;
    s += R"(")";
  }
  s += R"(,"side":"Buy","execId":")";
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

std::string executionFrame(const char* execId, const char* execQty, const char* execPrice,
                           const char* leavesQty, int64_t execTimeMs)
{
  static const std::string link = std::to_string(kEngineOrderId);
  return executionFrameFor(link.c_str(), execId, execQty, execPrice, leavesQty, execTimeMs);
}

// One "order" frame, in the field order Bybit V5 sends.
std::string orderFrameFor(const char* linkId, const char* orderStatus, const char* cumExecQty,
                          const char* avgPrice, int64_t updatedTimeMs)
{
  std::string s;
  s += R"({"topic":"order","data":[{"symbol":"BTCUSDT","orderId":")";
  s += kVenueOrderId;
  s += R"(")";
  if (linkId)
  {
    s += R"(,"orderLinkId":")";
    s += linkId;
    s += R"(")";
  }
  s += R"(,"side":"Buy","price":"60000","qty":"3","avgPrice":")";
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

std::string orderFrame(const char* orderStatus, const char* cumExecQty, const char* avgPrice,
                       int64_t updatedTimeMs)
{
  static const std::string link = std::to_string(kEngineOrderId);
  return orderFrameFor(link.c_str(), orderStatus, cumExecQty, avgPrice, updatedTimeMs);
}

// A Bybit orderbook frame, same shape unit_test_bybit_gap.cpp feeds.
std::string bookFrame(const char* type, int64_t u, int64_t seq)
{
  std::string s = R"({"topic":"orderbook.50.BTCUSDT","type":")";
  s += type;
  s += R"(","ts":1700000000000,"cts":1700000000000,"data":{"s":"BTCUSDT",)"
       R"("b":[["60000","3"]],"a":[["60010","2"]],"u":)";
  s += std::to_string(u);
  s += R"(,"seq":)";
  s += std::to_string(seq);
  s += "}}";
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

// Two orders live on one symbol at the same time, each filling once. The fill
// watermark is per order: keyed by anything coarser -- the symbol, say -- the
// second order's cumulative looks like a repeat of the first order's and its
// fill is swallowed, which is silent and unrecoverable.
TEST(BybitFillContract, TwoLiveOrdersOnOneSymbolBothReportTheirFills)
{
  Fixture f("bybit_two_orders.log");

  f.connector->handlePrivateMessage(executionFrameFor("111", "E1", "1", "60000", "2", 1000));
  f.connector->handlePrivateMessage(executionFrameFor("222", "E2", "1", "60050", "2", 1001));
  f.orderBus.flush();

  const auto fills = f.listener.fills();
  ASSERT_EQ(fills.size(), 2u) << "both orders traded; both fills must reach the engine";
  EXPECT_EQ(fills[0].orderId, 111u);
  EXPECT_DOUBLE_EQ(fills[0].fillQty, 1.0);
  EXPECT_DOUBLE_EQ(fills[0].fillPrice, 60000.0);
  EXPECT_EQ(fills[1].orderId, 222u);
  EXPECT_DOUBLE_EQ(fills[1].fillQty, 1.0)
      << "the second order's fill must not be cancelled out by the first order's";
  EXPECT_DOUBLE_EQ(fills[1].fillPrice, 60050.0);
}

// A venue redelivers frames: after a reconnect, or simply twice. The identical
// execution frame is one execution, not two. orderQty - leavesQty is the
// cumulative the venue itself reports, so the second copy adds nothing;
// treating execQty as an increment of what has already been published books it
// again.
TEST(BybitFillContract, ARedeliveredExecutionFrameDoesNotBookASecondFill)
{
  Fixture f("bybit_redelivery.log");

  const std::string frame = executionFrame("E1", "1", "60000", "2", 1000);
  f.connector->handlePrivateMessage(frame);
  f.connector->handlePrivateMessage(frame);
  f.orderBus.flush();

  const auto fills = f.listener.fills();
  ASSERT_EQ(fills.size(), 1u)
      << "the same execId, the same orderQty/leavesQty: one execution, one fill";
  EXPECT_DOUBLE_EQ(fills[0].fillQty, 1.0);
  EXPECT_DOUBLE_EQ(fills[0].fillPrice, 60000.0);
}

// An order this engine did not place -- entered in the venue UI, or by another
// process -- carries no orderLinkId. Its fills still have to be published, and
// the venue's own orderId is the only id there is for them. Dropping them
// leaves a position moving on the account that the engine cannot see at all.
TEST(BybitFillContract, ForeignOrderWithoutOrderLinkIdPublishesUnderTheVenueOrderId)
{
  Fixture f("bybit_foreign_order.log");

  f.connector->handlePrivateMessage(executionFrameFor(nullptr, "E1", "1", "60000", "2", 1000));
  f.orderBus.flush();

  auto fills = f.listener.fills();
  ASSERT_EQ(fills.size(), 1u) << "a fill on a foreign order must still be published";
  EXPECT_EQ(fills[0].orderId, 99887766u)
      << "with no orderLinkId the venue's orderId is the only id available";

  Fixture g("bybit_foreign_order_topic.log");
  g.connector->handlePrivateMessage(orderFrameFor(nullptr, "New", "0", "0", 1000));
  g.connector->handlePrivateMessage(orderFrameFor(nullptr, "PartiallyFilled", "1", "60000", 1001));
  g.orderBus.flush();

  fills = g.listener.fills();
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].orderId, 99887766u);
}

// REVIEW connectors/10. Bybit sets recvNs on its book events but used to leave
// sourceExchange at InvalidExchangeId, which CompositeBookMatrix rejects at its
// first line -- the cross-venue book was permanently empty in live.
TEST(BybitFeedContract, BookEventCarriesRecvNsAndSourceExchange)
{
  Fixture f("bybit_book_stamps.log");
  const ExchangeId expected = f.registry.getExchangeId(f.connector->exchangeId());
  ASSERT_NE(expected, InvalidExchangeId)
      << "the connector must register itself with the registry, or its events can name no venue";

  const int64_t before = nowNsMonotonic();
  f.connector->handleMessage(bookFrame("snapshot", 100, 1000));
  const int64_t after = nowNsMonotonic();
  f.bookBus.flush();

  const auto books = f.bookCapture.books();
  ASSERT_EQ(books.size(), 1u);
  EXPECT_EQ(books[0].bidCount, 1u);
  EXPECT_EQ(books[0].askCount, 1u);
  EXPECT_EQ(books[0].sourceExchange, expected)
      << "sourceExchange must be the connector's exchange id (" << expected
      << "); InvalidExchangeId makes CompositeBookMatrix drop the update";
  EXPECT_NE(books[0].recvNs, 0u);
  EXPECT_NE(books[0].publishTsNs, 0u);
  EXPECT_GE(static_cast<int64_t>(books[0].recvNs), before);
  EXPECT_LE(static_cast<int64_t>(books[0].recvNs), after);
  EXPECT_LT(books[0].recvNs, books[0].publishTsNs)
      << "recvNs is taken when the frame arrives, before it is parsed";
}

// The consumer the two stamps exist for.
TEST(BybitFeedContract, CompositeMatrixIsPopulatedAndGoesStale)
{
  Fixture f("bybit_matrix.log");
  const ExchangeId exId = f.registry.getExchangeId(f.connector->exchangeId());
  ASSERT_NE(exId, InvalidExchangeId);

  f.connector->handleMessage(bookFrame("snapshot", 100, 1000));
  f.bookBus.flush();

  const auto books = f.bookCapture.books();
  ASSERT_EQ(books.size(), 1u);
  const auto sym = f.registry.getSymbolId("bybit", "BTCUSDT");
  ASSERT_TRUE(sym.has_value());

  auto bid = f.matrix.bestBid(*sym);
  ASSERT_TRUE(bid.valid) << "a live snapshot must populate the cross-venue matrix";
  EXPECT_EQ(bid.priceRaw, Price::fromDouble(60000.0).raw());
  EXPECT_EQ(bid.exchange, exId);

  auto ask = f.matrix.bestAsk(*sym);
  ASSERT_TRUE(ask.valid);
  EXPECT_EQ(ask.priceRaw, Price::fromDouble(60010.0).raw());

  // Nothing arrives for two seconds against a one-second window.
  f.matrix.checkStaleness(static_cast<int64_t>(books[0].recvNs) + 2'000'000'000LL, 1'000'000'000LL);
  EXPECT_FALSE(f.matrix.bestBid(*sym).valid)
      << "the venue went quiet for 2s against a 1s window and must be marked stale";
  EXPECT_FALSE(f.matrix.bidForExchange(*sym, exId).valid);
}
