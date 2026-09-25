/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * One rule, on every venue that reports fills: a fill the exchange has not
 * priced is never published as a fill.
 *
 * Price has no unset state -- its default and a parsed "0" are the same 64
 * bits, which BybitFillContract.AnUnsetFillPriceIsIndistinguishableFromAParsed
 * Zero asserts directly -- so "the venue has not told us the price yet" and
 * "it traded at zero" arrive at a listener as the same event. A position
 * tracker cannot tell them apart and builds the cost basis at zero, which is
 * not a rounding error but an entry price wrong by its whole value.
 *
 * The Bybit order topic's case is pinned in unit_test_bybit_fill_contract.cpp,
 * where the venue's avgPrice "0" before anything trades makes it the everyday
 * shape. What is pinned here is the same rule on the paths where an unpriced
 * fill means a malformed frame rather than a normal one, and -- where the
 * venue reports again -- that the quantity is held rather than dropped.
 */

#include "flox-connectors/bitget/bitget_exchange_connector.h"
#include "flox-connectors/bybit/bybit_exchange_connector.h"
#include "flox-connectors/hyperliquid/hl_signer.h"
#include "flox-connectors/hyperliquid/hyperliquid_order_executor.h"

#include <flox/book/bus/book_update_bus.h>
#include <flox/book/bus/trade_bus.h>
#include <flox/common.h>
#include <flox/engine/symbol_registry.h>
#include <flox/execution/abstract_execution_listener.h>
#include <flox/execution/bus/order_execution_bus.h>
#include <flox/execution/events/order_event.h>
#include <flox/execution/order.h>
#include <flox/execution/order_tracker.h>
#include <flox/log/atomic_logger.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Shadows the real signing helper at link time, so no signer is ever launched.
// Same technique as unit_test_order_serialization_hyperliquid.cpp.
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

std::string tempLogDir()
{
  auto dir = std::filesystem::temp_directory_path() / "flox_unpriced_fill_logs";
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

struct SeenFill
{
  OrderEventStatus dispatchedAs;
  double fillQty;
  double fillPrice;
};

// Records the fill callbacks -- the ones that move a position -- and, through
// the raw fan-out, every event the connector published, so "no fill was
// dispatched" can be told apart from "nothing was published at all".
class Listener final : public IOrderExecutionListener
{
 public:
  Listener() : IOrderExecutionListener(81) {}

  void onOrderPartiallyFilled(const Order&, Quantity fillQty, Price fillPrice) override
  {
    std::lock_guard<std::mutex> lk(_m);
    _fills.push_back(
        {OrderEventStatus::PARTIALLY_FILLED, fillQty.toDouble(), fillPrice.toDouble()});
  }

  void onOrderFilled(const Order&, Quantity fillQty, Price fillPrice) override
  {
    std::lock_guard<std::mutex> lk(_m);
    _fills.push_back({OrderEventStatus::FILLED, fillQty.toDouble(), fillPrice.toDouble()});
  }

  void onOrderEvent(const OrderEvent& ev) override
  {
    std::lock_guard<std::mutex> lk(_m);
    _statuses.push_back(ev.status);
    _fillQtyRaw.push_back(ev.fillQty.raw());
  }

  std::vector<SeenFill> fills()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _fills;
  }

  std::vector<OrderEventStatus> statuses()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _statuses;
  }

  // True if any event was published carrying fill quantity -- the thing a
  // position tracker acts on, whatever status it wore.
  bool anyEventCarriedFillQty()
  {
    std::lock_guard<std::mutex> lk(_m);
    for (int64_t raw : _fillQtyRaw)
    {
      if (raw > 0)
      {
        return true;
      }
    }
    return false;
  }

 private:
  std::mutex _m;
  std::vector<SeenFill> _fills;
  std::vector<OrderEventStatus> _statuses;
  std::vector<int64_t> _fillQtyRaw;
};

struct BitgetFixture
{
  BookUpdateBus bookBus;
  TradeBus tradeBus;
  OrderExecutionBus orderBus;
  SymbolRegistry registry;
  std::unique_ptr<BitgetExchangeConnector> connector;
  Listener listener;

  explicit BitgetFixture(const char* logName)
  {
    BitgetConfig cfg;
    cfg.publicEndpoint = "wss://unused.invalid";
    cfg.privateEndpoint = "wss://unused.invalid";
    cfg.enablePrivate = true;
    cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BitgetConfig::BookDepth::Depth5}};
    connector = std::make_unique<BitgetExchangeConnector>(cfg, &bookBus, &tradeBus, &orderBus,
                                                          &registry, makeLogger(logName));
    orderBus.subscribe(&listener);
    bookBus.start();
    tradeBus.start();
    orderBus.start();
  }

  ~BitgetFixture()
  {
    bookBus.stop();
    tradeBus.stop();
    orderBus.stop();
  }
};

std::string bitgetOrdersFrame(const char* status, const char* fillPrice, const char* baseVolume,
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

struct BybitFixture
{
  BookUpdateBus bookBus;
  TradeBus tradeBus;
  OrderExecutionBus orderBus;
  SymbolRegistry registry;
  std::unique_ptr<BybitExchangeConnector> connector;
  Listener listener;

  explicit BybitFixture(const char* logName)
  {
    BybitConfig cfg;
    cfg.privateEndpoint = "wss://unused.invalid";
    cfg.enablePrivate = true;
    connector = std::make_unique<BybitExchangeConnector>(cfg, &bookBus, &tradeBus, &orderBus,
                                                         &registry, makeLogger(logName));
    orderBus.subscribe(&listener);
    bookBus.start();
    tradeBus.start();
    orderBus.start();
  }

  ~BybitFixture()
  {
    bookBus.stop();
    tradeBus.stop();
    orderBus.stop();
  }
};

std::string bybitExecutionFrame(const char* execId, const char* execQty, const char* execPrice,
                                const char* leavesQty)
{
  std::string s;
  s += R"({"topic":"execution","data":[{"symbol":"BTCUSDT","orderId":"99887766",)"
       R"("orderLinkId":"4242","side":"Buy","execId":")";
  s += execId;
  s += R"(","execPrice":")";
  s += execPrice;
  s += R"(","execQty":")";
  s += execQty;
  s += R"(","orderQty":"3","leavesQty":")";
  s += leavesQty;
  s += R"(","execTime":1000,"execType":"Trade"}]})";
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
    if (url.ends_with("/info"))
    {
      if (onSuccess)
      {
        onSuccess(R"({"universe":[{"name":"BTC"}]})");
      }
      return;
    }
    calls.push_back({std::string(url), std::string(body)});
    if (onSuccess)
    {
      onSuccess(nextResponse);
    }
  }

  std::vector<Call> calls;
  std::string nextResponse;
};

}  // namespace

// Bitget's fillPrice is "0" only on a malformed push, but the consequence is
// the Bybit one: a fill booked at zero. The quantity must not be lost either --
// the venue reports the same accBaseVolume on the next push, and that one
// carries the price, so it publishes everything held.
TEST(UnpricedFill, BitgetHoldsAnUnpricedIncrementUntilAPriceArrives)
{
  BitgetFixture f("bitget_unpriced.log");

  f.connector->handlePrivateMessage(bitgetOrdersFrame("partially_filled", "0", "1", "1"));
  f.orderBus.flush();

  EXPECT_TRUE(f.listener.fills().empty())
      << "an increment the venue has not priced must not reach a fill callback";
  EXPECT_FALSE(f.listener.anyEventCarriedFillQty())
      << "and it must not reach the bus carrying fill quantity under any status either";
  ASSERT_EQ(f.listener.statuses().size(), 1u) << "the order's status update still goes out";
  EXPECT_EQ(f.listener.statuses()[0], OrderEventStatus::ACCEPTED);

  f.connector->handlePrivateMessage(bitgetOrdersFrame("partially_filled", "60000", "1", "2"));
  f.orderBus.flush();

  const auto fills = f.listener.fills();
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_DOUBLE_EQ(fills[0].fillPrice, 60000.0);
  EXPECT_DOUBLE_EQ(fills[0].fillQty, 2.0)
      << "accBaseVolume went 0 -> 2 across the two pushes and only this one is priced, so it "
         "carries both units; publishing baseVolume=1 would drop the quantity that was held";
}

// The execution topic always carries execPrice, so a zero there is a malformed
// frame rather than "not priced yet" -- but the rule does not change, and the
// increment stays held until a well-formed frame reports the same cumulative.
TEST(UnpricedFill, BybitExecutionTopicHoldsAFillWithNoExecPrice)
{
  BybitFixture f("bybit_unpriced_exec.log");

  f.connector->handlePrivateMessage(bybitExecutionFrame("E1", "1", "0", "2"));
  f.orderBus.flush();

  EXPECT_TRUE(f.listener.fills().empty())
      << "execPrice 0 is not a price; a fill published at it books the position at zero";
  EXPECT_FALSE(f.listener.anyEventCarriedFillQty());
  ASSERT_EQ(f.listener.statuses().size(), 1u);
  EXPECT_EQ(f.listener.statuses()[0], OrderEventStatus::ACCEPTED);

  f.connector->handlePrivateMessage(bybitExecutionFrame("E2", "1", "60000", "1"));
  f.orderBus.flush();

  const auto fills = f.listener.fills();
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_DOUBLE_EQ(fills[0].fillPrice, 60000.0);
  EXPECT_DOUBLE_EQ(fills[0].fillQty, 2.0)
      << "orderQty - leavesQty went 0 -> 2 over the two frames and only the second is priced";
}

// Hyperliquid answers the submit with the fill inline and has no second
// channel, so a held quantity is never republished. The rule still holds: the
// engine is told the order's state, and is not handed a position priced at
// zero.
TEST(UnpricedFill, HyperliquidDoesNotPublishAnInlineFillWithoutAnAveragePrice)
{
  SymbolRegistry registry;
  OrderTracker tracker;
  OrderExecutionBus orderBus;
  Listener listener;

  auto owned = std::make_unique<FakeTransport>();
  auto* transport = owned.get();
  // statuses[0].filled without avgPx: the size traded, the price is missing.
  transport->nextResponse = R"({"status":"ok","response":{"type":"order","data":{"statuses":)"
                            R"([{"filled":{"totalSz":"2.0","oid":777}}]}}})";

  SymbolInfo info;
  info.exchange = "hyperliquid";
  info.symbol = "BTC";
  info.type = InstrumentType::Future;
  const SymbolId symbol = registry.registerSymbol(info);

  HyperliquidOrderExecutorT<NoPolicies> executor(std::move(owned), "https://unused.invalid", "aa",
                                                 &registry, &tracker, makeLogger("hl_unpriced.log"),
                                                 "0xacct", std::nullopt, true);
  orderBus.subscribe(&listener);
  orderBus.start();
  executor.setOrderBus(&orderBus);

  Order order;
  order.id = kEngineOrderId;
  order.symbol = symbol;
  order.side = Side::BUY;
  order.type = OrderType::LIMIT;
  order.price = Price::fromDouble(60000.0);
  order.quantity = Quantity::fromDouble(2.0);

  executor.submitOrder(order);
  orderBus.flush();

  ASSERT_EQ(transport->calls.size(), 1u);
  EXPECT_TRUE(listener.fills().empty())
      << "the venue reported no avgPx; publishing the fill anyway books the position at price 0";
  EXPECT_FALSE(listener.anyEventCarriedFillQty());
  ASSERT_EQ(listener.statuses().size(), 1u) << "the order's state still reaches the engine";
  EXPECT_EQ(listener.statuses()[0], OrderEventStatus::ACCEPTED);

  orderBus.stop();
}
