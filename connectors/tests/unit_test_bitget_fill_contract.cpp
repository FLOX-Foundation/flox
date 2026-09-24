/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline protocol tests for the live-fill and live-feed contract on Bitget.
 * Recorded-shape frames go into the connector's message handlers and the
 * assertions are taken on what the buses deliver.
 *
 * What is pinned:
 *
 *  - fillQty and fillPrice. The private "orders" channel reports the latest
 *    fill as baseVolume/fillPrice and the cumulative one as accBaseVolume;
 *    the connector reads none of them, so a fill reaches the engine as
 *    onOrderFilled(order, 0, 0) and never moves a position.
 *
 *  - status mapping. "partially_filled" falls into the else branch and is
 *    published as SUBMITTED, so a partial fill dispatches onOrderSubmitted.
 *
 *  - recvNs and sourceExchange. Neither is set on any Bitget event. Without
 *    sourceExchange, CompositeBookMatrix::onBookUpdate returns at its first
 *    line and the cross-venue book stays permanently empty; without recvNs,
 *    checkStaleness() skips the venue (lastUpdateNs == 0) and a frozen feed
 *    keeps being quoted forever. docs/how-to/custom-connector.md tells
 *    connectors to set recvNs.
 *
 * Bitget's handlers are private, unlike Bybit's handlePrivateMessage, which is
 * public precisely so offline protocol tests like these can exist. Each test is
 * therefore gated on that seam and reports the signature it needs; opening the
 * seam alone does not make any of them pass.
 */

#include "flox-connectors/bitget/bitget_exchange_connector.h"

#include <flox/book/bus/book_update_bus.h>
#include <flox/book/bus/trade_bus.h>
#include <flox/book/composite_book_matrix.h>
#include <flox/book/events/book_update_event.h>
#include <flox/common.h>
#include <flox/engine/symbol_registry.h>
#include <flox/execution/abstract_execution_listener.h>
#include <flox/execution/bus/order_execution_bus.h>
#include <flox/execution/events/order_event.h>
#include <flox/log/atomic_logger.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

using namespace flox;

namespace
{

// The offline seam these tests need. Access checking is part of template
// argument substitution, so a private handler makes this false.
template <typename C, typename = void>
struct HasPublicHandleMessage : std::false_type
{
};

template <typename C>
struct HasPublicHandleMessage<
    C, std::void_t<decltype(std::declval<C&>().handleMessage(std::string_view{}))>> : std::true_type
{
};

template <typename C, typename = void>
struct HasPublicHandlePrivateMessage : std::false_type
{
};

template <typename C>
struct HasPublicHandlePrivateMessage<
    C, std::void_t<decltype(std::declval<C&>().handlePrivateMessage(std::string_view{}))>>
    : std::true_type
{
};

constexpr const char* kPublicSeam =
    "needs: void BitgetExchangeConnector::handleMessage(std::string_view) declared public, "
    "like BybitExchangeConnector's, so recorded frames can be fed without a socket";
constexpr const char* kPrivateSeam =
    "needs: void BitgetExchangeConnector::handlePrivateMessage(std::string_view) declared public, "
    "like BybitExchangeConnector::handlePrivateMessage";

std::string tempLogDir()
{
  auto dir = std::filesystem::temp_directory_path() / "flox_bitget_fill_contract_logs";
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
  double orderFilledQuantity;
  uint64_t recvNs;
};

class FillListener final : public IOrderExecutionListener
{
 public:
  FillListener() : IOrderExecutionListener(51) {}

  void onOrderSubmitted(const Order& order) override
  {
    std::lock_guard<std::mutex> lk(_m);
    _submitted.push_back(order.id);
  }

  void onOrderPartiallyFilled(const Order& order, Quantity fillQty, Price fillPrice) override
  {
    record(OrderEventStatus::PARTIALLY_FILLED, order, fillQty, fillPrice);
  }

  void onOrderFilled(const Order& order, Quantity fillQty, Price fillPrice) override
  {
    record(OrderEventStatus::FILLED, order, fillQty, fillPrice);
  }

  // The raw fan-out is the only place recvNs is visible; it is not called by
  // the bus dispatcher, so the connector tests read it through a dedicated
  // subscriber below instead. Kept here for the fill payload only.
  std::vector<SeenFill> fills()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _fills;
  }

  std::vector<OrderId> submitted()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _submitted;
  }

 private:
  void record(OrderEventStatus status, const Order& order, Quantity fillQty, Price fillPrice)
  {
    std::lock_guard<std::mutex> lk(_m);
    _fills.push_back({status, order.id, fillQty.toDouble(), fillPrice.toDouble(),
                      order.quantity.toDouble(), order.filledQuantity.toDouble(), 0});
  }

  std::mutex _m;
  std::vector<SeenFill> _fills;
  std::vector<OrderId> _submitted;
};

struct SeenBook
{
  uint64_t recvNs;
  ExchangeId sourceExchange;
  size_t bidCount;
  size_t askCount;
};

class BookCapture final : public IMarketDataSubscriber
{
 public:
  SubscriberId id() const override { return 52; }

  void onBookUpdate(const BookUpdateEvent& ev) override
  {
    std::lock_guard<std::mutex> lk(_m);
    _books.push_back(
        {ev.recvNs.raw(), ev.sourceExchange, ev.update.bids.size(), ev.update.asks.size()});
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

// A books5 snapshot exactly as the venue sends it.
constexpr const char* kBookSnapshot =
    R"({"action":"snapshot","arg":{"instType":"USDT-FUTURES","channel":"books5",)"
    R"("instId":"BTCUSDT"},"data":[{"asks":[["60010","2"]],"bids":[["60000","3"]],)"
    R"("checksum":0,"ts":"1700000000000"}],"ts":1700000000000})";

// A private "orders" push. baseVolume is the latest fill's size, fillPrice the
// price it traded at, accBaseVolume the order's cumulative filled quantity.
std::string ordersFrame(const char* status, const char* fillPrice, const char* baseVolume,
                        const char* accBaseVolume)
{
  std::string s;
  s += R"({"action":"snapshot","arg":{"instType":"USDT-FUTURES","channel":"orders",)"
       R"("instId":"default"},"data":[{"instId":"BTCUSDT","orderId":"9001",)"
       R"("clientOid":"4242","price":"60000","size":"3","orderType":"limit","side":"buy",)"
       R"("fillPrice":")";
  s += fillPrice;
  s += R"(","tradeId":"7001","baseVolume":")";
  s += baseVolume;
  s += R"(","fillTime":"1700000000000","accBaseVolume":")";
  s += accBaseVolume;
  s += R"(","priceAvg":"60000","status":")";
  s += status;
  s += R"(","cTime":"1700000000000","uTime":"1700000000000"}],"ts":1700000000000})";
  return s;
}

// Templated on the connector so every call through it stays type-dependent and
// the discarded half of each if-constexpr below is never instantiated.
template <typename Connector>
struct Fixture
{
  BookUpdateBus bookBus;
  TradeBus tradeBus;
  OrderExecutionBus orderBus;
  SymbolRegistry registry;
  std::shared_ptr<AtomicLogger> logger;
  std::unique_ptr<Connector> connector;
  FillListener fillListener;
  BookCapture bookCapture;
  CompositeBookMatrix<4> matrix;

  explicit Fixture(const char* logName)
  {
    AtomicLoggerOptions logOpts;
    logOpts.directory = tempLogDir();
    logOpts.basename = logName;
    logger = std::make_shared<AtomicLogger>(logOpts);

    // Registered up front so the exchange id the connector must stamp is
    // deterministic and inside CompositeBookMatrix's range.
    registry.registerExchange("bitget");

    BitgetConfig cfg;
    cfg.publicEndpoint = "wss://unused.invalid";
    cfg.privateEndpoint = "wss://unused.invalid";
    cfg.enablePrivate = true;
    cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BitgetConfig::BookDepth::Depth5}};
    connector = std::make_unique<Connector>(cfg, &bookBus, &tradeBus, &orderBus, &registry, logger);

    matrix.setId(53);
    bookBus.subscribe(&bookCapture);
    bookBus.subscribe(&matrix);
    orderBus.subscribe(&fillListener);
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

// REVIEW connectors/5 and connectors/3.
template <typename Connector>
void partialFillCarriesQtyPriceAndStatus()
{
  if constexpr (!HasPublicHandlePrivateMessage<Connector>::value)
  {
    FAIL() << kPrivateSeam;
  }
  else
  {
    Fixture<Connector> f("bitget_partial_fill.log");

    f.connector->handlePrivateMessage(ordersFrame("partially_filled", "60000", "1", "1"));
    f.orderBus.flush();

    const auto fills = f.fillListener.fills();
    ASSERT_EQ(fills.size(), 1u)
        << "a partially_filled push must dispatch a fill, not onOrderSubmitted; submitted count="
        << f.fillListener.submitted().size();
    EXPECT_EQ(fills[0].dispatchedAs, OrderEventStatus::PARTIALLY_FILLED);
    EXPECT_EQ(fills[0].orderId, 4242u);
    EXPECT_DOUBLE_EQ(fills[0].fillQty, 1.0) << "baseVolume=1 is the size that just traded";
    EXPECT_DOUBLE_EQ(fills[0].fillPrice, 60000.0) << "fillPrice=60000 is where it traded";
    EXPECT_DOUBLE_EQ(fills[0].orderQuantity, 3.0);
    EXPECT_DOUBLE_EQ(fills[0].orderFilledQuantity, 1.0) << "accBaseVolume=1";
  }
}

template <typename Connector>
void terminalFillCarriesQtyAndPrice()
{
  if constexpr (!HasPublicHandlePrivateMessage<Connector>::value)
  {
    FAIL() << kPrivateSeam;
  }
  else
  {
    Fixture<Connector> f("bitget_full_fill.log");

    f.connector->handlePrivateMessage(ordersFrame("partially_filled", "60000", "1", "1"));
    f.connector->handlePrivateMessage(ordersFrame("filled", "60010", "2", "3"));
    f.orderBus.flush();

    const auto fills = f.fillListener.fills();
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[1].dispatchedAs, OrderEventStatus::FILLED);
    EXPECT_DOUBLE_EQ(fills[1].fillQty, 2.0);
    EXPECT_DOUBLE_EQ(fills[1].fillPrice, 60010.0);
    EXPECT_DOUBLE_EQ(fills[1].orderFilledQuantity, 3.0);

    double total = fills[0].fillQty + fills[1].fillQty;
    EXPECT_DOUBLE_EQ(total, 3.0) << "the two pushes together filled the whole 3-lot order";
  }
}

// REVIEW connectors/10 and connectors/11.
template <typename Connector>
void bookEventCarriesRecvNsAndSourceExchange()
{
  if constexpr (!HasPublicHandleMessage<Connector>::value)
  {
    FAIL() << kPublicSeam;
  }
  else
  {
    Fixture<Connector> f("bitget_book_stamps.log");
    const ExchangeId expected = f.registry.getExchangeId(f.connector->exchangeId());
    ASSERT_NE(expected, InvalidExchangeId);

    f.connector->handleMessage(kBookSnapshot);
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
  }
}

// REVIEW connectors/10 and connectors/11, observed through the consumer that
// the two missing fields actually break.
template <typename Connector>
void compositeMatrixIsPopulatedAndGoesStale()
{
  if constexpr (!HasPublicHandleMessage<Connector>::value)
  {
    FAIL() << kPublicSeam;
  }
  else
  {
    Fixture<Connector> f("bitget_matrix.log");
    const ExchangeId exId = f.registry.getExchangeId(f.connector->exchangeId());
    ASSERT_NE(exId, InvalidExchangeId);

    f.connector->handleMessage(kBookSnapshot);
    f.bookBus.flush();

    const auto books = f.bookCapture.books();
    ASSERT_EQ(books.size(), 1u);
    const SymbolId sym = *f.registry.getSymbolId("bitget", "BTCUSDT");

    auto bid = f.matrix.bestBid(sym);
    ASSERT_TRUE(bid.valid) << "a live snapshot must populate the cross-venue matrix";
    EXPECT_EQ(bid.priceRaw, Price::fromDouble(60000.0).raw());
    EXPECT_EQ(bid.exchange, exId);

    auto ask = f.matrix.bestAsk(sym);
    ASSERT_TRUE(ask.valid);
    EXPECT_EQ(ask.priceRaw, Price::fromDouble(60010.0).raw());

    // Nothing arrives for two seconds against a one-second window.
    const int64_t lastUpdateNs = static_cast<int64_t>(books[0].recvNs);
    f.matrix.checkStaleness(lastUpdateNs + 2'000'000'000LL, 1'000'000'000LL);

    EXPECT_FALSE(f.matrix.bestBid(sym).valid)
        << "the venue went quiet for 2s against a 1s window and must be marked stale";
    EXPECT_FALSE(f.matrix.bidForExchange(sym, exId).valid);
  }
}

}  // namespace

TEST(BitgetFillContract, PartialFillCarriesQtyPriceAndStatus)
{
  partialFillCarriesQtyPriceAndStatus<BitgetExchangeConnector>();
}

TEST(BitgetFillContract, TerminalFillCarriesQtyAndPrice)
{
  terminalFillCarriesQtyAndPrice<BitgetExchangeConnector>();
}

TEST(BitgetFeedContract, BookEventCarriesRecvNsAndSourceExchange)
{
  bookEventCarriesRecvNsAndSourceExchange<BitgetExchangeConnector>();
}

TEST(BitgetFeedContract, CompositeMatrixIsPopulatedAndGoesStale)
{
  compositeMatrixIsPopulatedAndGoesStale<BitgetExchangeConnector>();
}
