/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline tests for the live-fill and live-feed contract on Hyperliquid. No
 * sockets and no signing helper: the executor's transport is injected (the
 * same seam unit_test_order_serialization_hyperliquid.cpp uses) and recorded
 * venue responses are handed back through it, while the connector's recorded
 * l2Book frames go into its message handler.
 *
 * What is pinned:
 *
 *  - The order path is silent. The executor holds no OrderExecutionBus and
 *    publishes no OrderEvent at all: a fill returned inline by the exchange
 *    never reaches a listener, and a venue rejection in
 *    response.data.statuses[0].error is not even parsed -- the order is
 *    recorded as submitted instead. docs/explanation/connectors.md promises a
 *    REJECTED event with a reason on every venue.
 *
 *  - When a fill does reach the bus it has to carry both the size and the
 *    price that traded; Hyperliquid returns them as totalSz and avgPx.
 *
 *  - recvNs and sourceExchange are set on no Hyperliquid event, so the
 *    cross-venue CompositeBookMatrix stays empty in live and its staleness
 *    sweep skips the venue entirely.
 *
 * hl_sign_with_sdk() is a free function the executor calls before it will
 * touch the transport; defining it here shadows the real one at link time, so
 * no signing helper is ever launched. Same technique as the serialization test.
 */

#include "flox-connectors/hyperliquid/hl_signer.h"
#include "flox-connectors/hyperliquid/hyperliquid_exchange_connector.h"
#include "flox-connectors/hyperliquid/hyperliquid_order_executor.h"

#include <flox/book/bus/book_update_bus.h>
#include <flox/book/bus/trade_bus.h>
#include <flox/book/composite_book_matrix.h>
#include <flox/book/events/book_update_event.h>
#include <flox/common.h>
#include <flox/engine/symbol_registry.h>
#include <flox/execution/abstract_execution_listener.h>
#include <flox/execution/bus/order_execution_bus.h>
#include <flox/execution/events/order_event.h>
#include <flox/execution/order.h>
#include <flox/execution/order_tracker.h>
#include <flox/log/atomic_logger.h>
#include <flox/util/base/time.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

namespace flox::hl
{
std::optional<HlSig> hl_sign_with_sdk(const HlSignParams&)
{
  return HlSig{"0xdeadbeef", "0xcafebabe", 27};
}
}  // namespace flox::hl

using namespace flox;

namespace
{

constexpr OrderId kEngineOrderId = 4242;

// The two seams these tests need. Access checking is part of template argument
// substitution, so a private handler or a missing setter makes these false.
template <typename C, typename = void>
struct HasPublicHandleMessage : std::false_type
{
};

template <typename C>
struct HasPublicHandleMessage<
    C, std::void_t<decltype(std::declval<C&>().handleMessage(std::string_view{}))>> : std::true_type
{
};

template <typename E, typename = void>
struct HasOrderBusSetter : std::false_type
{
};

template <typename E>
struct HasOrderBusSetter<
    E, std::void_t<decltype(std::declval<E&>().setOrderBus(std::declval<OrderExecutionBus*>()))>>
    : std::true_type
{
};

constexpr const char* kOrderBusSeam =
    "needs: void HyperliquidOrderExecutorT<Policies>::setOrderBus(OrderExecutionBus*), the same "
    "hook BybitOrderExecutorT already has; the Hyperliquid executor currently publishes no "
    "OrderEvent on any path";
constexpr const char* kFeedSeam =
    "needs: void HyperliquidExchangeConnector::handleMessage(std::string_view) declared public, "
    "like BybitExchangeConnector's, so recorded frames can be fed without a socket";

std::string tempLogDir()
{
  auto dir = std::filesystem::temp_directory_path() / "flox_hl_fill_contract_logs";
  std::filesystem::create_directories(dir);
  return dir.string();
}

std::shared_ptr<AtomicLogger> makeLogger(const char* name)
{
  AtomicLoggerOptions logOpts;
  logOpts.directory = tempLogDir();
  logOpts.basename = name;
  return std::make_shared<AtomicLogger>(logOpts);
}

struct Call
{
  std::string url;
  std::string body;
};

// The executor fetches the asset-id map from a hardcoded ".../info" URL in its
// constructor; that round-trip is answered here and kept out of `calls`.
class FakeTransport final : public ITransport
{
 public:
  void post(std::string_view url, std::string_view body,
            const std::vector<std::pair<std::string_view, std::string_view>>&,
            MoveOnlyFunction<void(std::string_view)> onSuccess,
            MoveOnlyFunction<void(std::string_view)> onError) override
  {
    if (url.ends_with("/info"))
    {
      if (onSuccess)
      {
        onSuccess(R"({"universe":[{"name":"BTC"}]})");
      }
      return;
    }
    calls.push_back({std::string(url), std::string(body)});
    // A round-trip that never completes -- DNS failure, a reset connection, a
    // timeout -- comes back through onError, not onSuccess. It is the half of
    // the transport contract a fake that only ever succeeds can never exercise.
    if (failNext)
    {
      if (onError)
      {
        onError(errorText);
      }
      return;
    }
    if (onSuccess)
    {
      onSuccess(nextResponse);
    }
  }

  std::vector<Call> calls;
  std::string nextResponse;
  bool failNext{false};
  std::string errorText;
};

struct SeenEvent
{
  OrderEventStatus status;
  OrderId orderId;
  double fillQty;
  double fillPrice;
  double orderFilledQuantity;
  std::string reason;
};

class OrderListener final : public IOrderExecutionListener
{
 public:
  OrderListener() : IOrderExecutionListener(61) {}

  void onOrderPartiallyFilled(const Order& order, Quantity fillQty, Price fillPrice) override
  {
    push({OrderEventStatus::PARTIALLY_FILLED, order.id, fillQty.toDouble(), fillPrice.toDouble(),
          order.filledQuantity.toDouble(), ""});
  }

  void onOrderFilled(const Order& order, Quantity fillQty, Price fillPrice) override
  {
    push({OrderEventStatus::FILLED, order.id, fillQty.toDouble(), fillPrice.toDouble(),
          order.filledQuantity.toDouble(), ""});
  }

  void onOrderRejected(const Order& order, const std::string& reason) override
  {
    push({OrderEventStatus::REJECTED, order.id, 0.0, 0.0, order.filledQuantity.toDouble(), reason});
  }

  std::vector<SeenEvent> events()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _events;
  }

 private:
  void push(SeenEvent ev)
  {
    std::lock_guard<std::mutex> lk(_m);
    _events.push_back(std::move(ev));
  }

  std::mutex _m;
  std::vector<SeenEvent> _events;
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
  SubscriberId id() const override { return 62; }

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

constexpr const char* kL2BookFrame =
    R"({"channel":"l2Book","data":{"coin":"BTC","time":1700000000000,"levels":)"
    R"([[{"px":"60000","sz":"3","n":1}],[{"px":"60010","sz":"2","n":1}]]}})";

// A submit whose response the fake transport will return, wired to a live
// OrderExecutionBus. Templated on the executor so every call through it stays
// type-dependent and the discarded half of each if-constexpr is never
// instantiated.
template <typename Executor>
struct ExecutorFixture
{
  SymbolRegistry registry;
  OrderTracker tracker;
  OrderExecutionBus orderBus;
  OrderListener listener;
  FakeTransport* transport{nullptr};
  SymbolId symbol{};
  std::unique_ptr<Executor> executor;

  explicit ExecutorFixture(const char* logName, std::string response)
  {
    auto owned = std::make_unique<FakeTransport>();
    transport = owned.get();
    transport->nextResponse = std::move(response);

    SymbolInfo info;
    info.exchange = "hyperliquid";
    info.symbol = "BTC";
    info.type = InstrumentType::Future;
    symbol = registry.registerSymbol(info);

    executor =
        std::make_unique<Executor>(std::move(owned), "https://unused.invalid", "aa", &registry,
                                   &tracker, makeLogger(logName), "0xacct", std::nullopt, true);
    orderBus.subscribe(&listener);
    orderBus.start();
    executor->setOrderBus(&orderBus);
  }

  ~ExecutorFixture() { orderBus.stop(); }

  Order buyOrder() const
  {
    Order order;
    order.id = kEngineOrderId;
    order.symbol = symbol;
    order.side = Side::BUY;
    order.type = OrderType::LIMIT;
    order.price = Price::fromDouble(60000.0);
    order.quantity = Quantity::fromDouble(2.0);
    return order;
  }
};

template <typename Connector>
struct FeedFixture
{
  BookUpdateBus bookBus;
  TradeBus tradeBus;
  SymbolRegistry registry;
  std::shared_ptr<AtomicLogger> logger;
  std::unique_ptr<Connector> connector;
  BookCapture bookCapture;
  CompositeBookMatrix<4> matrix;

  explicit FeedFixture(const char* logName)
  {
    logger = makeLogger(logName);
    registry.registerExchange("hyperliquid");

    HyperliquidConfig cfg;
    cfg.wsEndpoint = "wss://unused.invalid";
    cfg.symbols = {"BTC"};
    connector = std::make_unique<Connector>(cfg, &bookBus, &tradeBus, &registry, logger);

    matrix.setId(63);
    bookBus.subscribe(&bookCapture);
    bookBus.subscribe(&matrix);
    bookBus.start();
    tradeBus.start();
  }

  ~FeedFixture()
  {
    bookBus.stop();
    tradeBus.stop();
  }
};

// REVIEW connectors/6 and connectors/3. Hyperliquid answers a crossing order
// inline with the fill: statuses[0].filled carries totalSz and avgPx. That fill
// must reach a listener, with both numbers on it.
template <typename Executor>
void inlineFillReachesTheBus()
{
  if constexpr (!HasOrderBusSetter<Executor>::value)
  {
    FAIL() << kOrderBusSeam;
  }
  else
  {
    ExecutorFixture<Executor> f("hl_inline_fill.log",
                                R"({"status":"ok","response":{"type":"order","data":{"statuses":)"
                                R"([{"filled":{"totalSz":"2.0","avgPx":"60005.0","oid":777}}]}}})");

    f.executor->submitOrder(f.buyOrder());
    f.orderBus.flush();

    ASSERT_EQ(f.transport->calls.size(), 1u);

    const auto events = f.listener.events();
    ASSERT_EQ(events.size(), 1u) << "the venue reported the order fully filled; the engine must "
                                    "see exactly one fill event";
    EXPECT_EQ(events[0].status, OrderEventStatus::FILLED);
    EXPECT_EQ(events[0].orderId, kEngineOrderId);
    EXPECT_DOUBLE_EQ(events[0].fillQty, 2.0) << "totalSz=2.0";
    EXPECT_DOUBLE_EQ(events[0].fillPrice, 60005.0)
        << "avgPx=60005.0; a fill at price 0 builds the position's cost basis at zero";
    EXPECT_DOUBLE_EQ(events[0].orderFilledQuantity, 2.0)
        << "the order the event carries must say how much of it has filled; anything reading "
           "order.filledQuantity instead of the fill delta otherwise sees an untouched order";
  }
}

// REVIEW connectors/6, the other half of the order path: a round-trip that
// never completes. The venue never saw the order, so the strategy has to be
// told -- and told why, or the failure is indistinguishable from silence.
template <typename Executor>
void transportFailurePublishesRejection()
{
  if constexpr (!HasOrderBusSetter<Executor>::value)
  {
    FAIL() << kOrderBusSeam;
  }
  else
  {
    constexpr const char* kError = "Recv failure: Connection reset by peer";

    ExecutorFixture<Executor> f("hl_transport_error.log", "");
    f.transport->failNext = true;
    f.transport->errorText = kError;

    f.executor->submitOrder(f.buyOrder());
    f.orderBus.flush();

    ASSERT_EQ(f.transport->calls.size(), 1u);

    const auto events = f.listener.events();
    ASSERT_EQ(events.size(), 1u)
        << "a submit that failed in transport must produce exactly one event, not silence";
    EXPECT_EQ(events[0].status, OrderEventStatus::REJECTED);
    EXPECT_EQ(events[0].orderId, kEngineOrderId);
    EXPECT_NE(events[0].reason.find(kError), std::string::npos)
        << "the transport's own error text must survive into the reason: " << events[0].reason;

    EXPECT_FALSE(f.tracker.isActive(kEngineOrderId))
        << "an order that never reached the venue must not be left alive in the tracker";
  }
}

// REVIEW connectors/6. A venue rejection arrives inside statuses[0].error and
// is currently not parsed at all -- the order is handed to the tracker as
// submitted, so the strategy believes a resting order exists.
template <typename Executor>
void venueRejectionReachesTheBusWithItsReason()
{
  if constexpr (!HasOrderBusSetter<Executor>::value)
  {
    FAIL() << kOrderBusSeam;
  }
  else
  {
    constexpr const char* kReason = "Order could not immediately match against any resting orders.";

    ExecutorFixture<Executor> f(
        "hl_reject.log",
        std::string(R"({"status":"ok","response":{"type":"order","data":{"statuses":[{"error":")") +
            kReason + R"("}]}}})");

    f.executor->submitOrder(f.buyOrder());
    f.orderBus.flush();

    ASSERT_EQ(f.transport->calls.size(), 1u);

    const auto events = f.listener.events();
    ASSERT_EQ(events.size(), 1u) << "a rejected order must produce exactly one event";
    EXPECT_EQ(events[0].status, OrderEventStatus::REJECTED);
    EXPECT_EQ(events[0].orderId, kEngineOrderId);
    EXPECT_EQ(events[0].reason, kReason)
        << "the venue's own reason text must be carried through, not dropped";

    EXPECT_FALSE(f.tracker.isActive(kEngineOrderId))
        << "a rejected order must not be left alive in the tracker";
  }
}

// REVIEW connectors/10 and connectors/11.
template <typename Connector>
void bookEventCarriesRecvNsAndSourceExchange()
{
  if constexpr (!HasPublicHandleMessage<Connector>::value)
  {
    FAIL() << kFeedSeam;
  }
  else
  {
    FeedFixture<Connector> f("hl_book_stamps.log");
    const ExchangeId expected = f.registry.getExchangeId(f.connector->exchangeId());
    ASSERT_NE(expected, InvalidExchangeId);

    const int64_t before = nowNsMonotonic();
    f.connector->handleMessage(kL2BookFrame);
    const int64_t after = nowNsMonotonic();
    f.bookBus.flush();

    const auto books = f.bookCapture.books();
    ASSERT_EQ(books.size(), 1u);
    EXPECT_EQ(books[0].bidCount, 1u);
    EXPECT_EQ(books[0].askCount, 1u);
    EXPECT_NE(books[0].recvNs, 0u)
        << "recvNs must be stamped on receipt; at zero, CompositeBookMatrix::checkStaleness "
           "skips this venue and a frozen feed is quoted forever";
    EXPECT_EQ(books[0].sourceExchange, expected)
        << "sourceExchange must be the connector's exchange id (" << expected
        << "); InvalidExchangeId makes CompositeBookMatrix drop the update";
    EXPECT_NE(books[0].publishTsNs, 0u)
        << "publishTsNs is the bus-publish stamp every other event path sets; without it the "
           "connector-to-consumer leg cannot be measured at all";
    EXPECT_LT(books[0].recvNs, books[0].publishTsNs)
        << "recvNs is taken when the frame arrives, before it is parsed; publishTsNs when it "
           "goes on the bus";
    EXPECT_GE(static_cast<int64_t>(books[0].recvNs), before);
    EXPECT_LE(static_cast<int64_t>(books[0].publishTsNs), after);
  }
}

// REVIEW connectors/10 and connectors/11, observed through the consumer that
// the two missing fields actually break.
template <typename Connector>
void compositeMatrixIsPopulatedAndGoesStale()
{
  if constexpr (!HasPublicHandleMessage<Connector>::value)
  {
    FAIL() << kFeedSeam;
  }
  else
  {
    FeedFixture<Connector> f("hl_matrix.log");
    const ExchangeId exId = f.registry.getExchangeId(f.connector->exchangeId());
    ASSERT_NE(exId, InvalidExchangeId);

    f.connector->handleMessage(kL2BookFrame);
    f.bookBus.flush();

    const auto books = f.bookCapture.books();
    ASSERT_EQ(books.size(), 1u);
    const auto sym = f.registry.getSymbolId("hyperliquid", "BTC");
    ASSERT_TRUE(sym.has_value());

    auto bid = f.matrix.bestBid(*sym);
    ASSERT_TRUE(bid.valid) << "a live snapshot must populate the cross-venue matrix";
    EXPECT_EQ(bid.priceRaw, Price::fromDouble(60000.0).raw());
    EXPECT_EQ(bid.exchange, exId);

    auto ask = f.matrix.bestAsk(*sym);
    ASSERT_TRUE(ask.valid);
    EXPECT_EQ(ask.priceRaw, Price::fromDouble(60010.0).raw());

    // Nothing arrives for two seconds against a one-second window.
    const int64_t lastUpdateNs = static_cast<int64_t>(books[0].recvNs);
    f.matrix.checkStaleness(lastUpdateNs + 2'000'000'000LL, 1'000'000'000LL);

    EXPECT_FALSE(f.matrix.bestBid(*sym).valid)
        << "the venue went quiet for 2s against a 1s window and must be marked stale";
    EXPECT_FALSE(f.matrix.bidForExchange(*sym, exId).valid);
  }
}

}  // namespace

TEST(HyperliquidFillContract, InlineFillReachesTheBus)
{
  inlineFillReachesTheBus<HyperliquidOrderExecutorT<NoPolicies>>();
}

TEST(HyperliquidFillContract, VenueRejectionReachesTheBusWithItsReason)
{
  venueRejectionReachesTheBusWithItsReason<HyperliquidOrderExecutorT<NoPolicies>>();
}

TEST(HyperliquidFillContract, TransportFailurePublishesRejection)
{
  transportFailurePublishesRejection<HyperliquidOrderExecutorT<NoPolicies>>();
}

TEST(HyperliquidFeedContract, BookEventCarriesRecvNsAndSourceExchange)
{
  bookEventCarriesRecvNsAndSourceExchange<HyperliquidExchangeConnector>();
}

TEST(HyperliquidFeedContract, CompositeMatrixIsPopulatedAndGoesStale)
{
  compositeMatrixIsPopulatedAndGoesStale<HyperliquidExchangeConnector>();
}
