/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * The de-duplication half of the live-fill contract, tested twice over: on
 * FillWatermark directly, and through the Bitget connector on the case the
 * watermark exists for -- the venue re-pushing an order's current state after
 * a private resubscribe, which without it books the same fill a second time.
 *
 * The cross-topic case (Bybit announcing one execution on both private
 * topics) lives in unit_test_bybit_fill_contract.cpp; what is pinned here is
 * the component both connectors share and the repeat-push case no acceptance
 * test covers.
 */

#include "flox-connectors/bitget/bitget_exchange_connector.h"
#include "flox-connectors/execution/fill_watermark.h"

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
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace flox;

namespace
{

Quantity qty(double v) { return Quantity::fromDouble(v); }

std::string tempLogDir()
{
  auto dir = std::filesystem::temp_directory_path() / "flox_fill_watermark_logs";
  std::filesystem::create_directories(dir);
  return dir.string();
}

struct SeenFill
{
  OrderEventStatus dispatchedAs;
  double fillQty;
  double fillPrice;
};

class FillListener final : public IOrderExecutionListener
{
 public:
  FillListener() : IOrderExecutionListener(71) {}

  void onOrderPartiallyFilled(const Order&, Quantity fillQty, Price fillPrice) override
  {
    push({OrderEventStatus::PARTIALLY_FILLED, fillQty.toDouble(), fillPrice.toDouble()});
  }

  void onOrderFilled(const Order&, Quantity fillQty, Price fillPrice) override
  {
    push({OrderEventStatus::FILLED, fillQty.toDouble(), fillPrice.toDouble()});
  }

  std::vector<SeenFill> fills()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _fills;
  }

 private:
  void push(SeenFill f)
  {
    std::lock_guard<std::mutex> lk(_m);
    _fills.push_back(f);
  }

  std::mutex _m;
  std::vector<SeenFill> _fills;
};

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

struct BitgetFixture
{
  BookUpdateBus bookBus;
  TradeBus tradeBus;
  OrderExecutionBus orderBus;
  SymbolRegistry registry;
  std::shared_ptr<AtomicLogger> logger;
  std::unique_ptr<BitgetExchangeConnector> connector;
  FillListener listener;

  explicit BitgetFixture(const char* logName)
  {
    AtomicLoggerOptions logOpts;
    logOpts.directory = tempLogDir();
    logOpts.basename = logName;
    logger = std::make_shared<AtomicLogger>(logOpts);

    BitgetConfig cfg;
    cfg.publicEndpoint = "wss://unused.invalid";
    cfg.privateEndpoint = "wss://unused.invalid";
    cfg.enablePrivate = true;
    cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BitgetConfig::BookDepth::Depth5}};
    connector = std::make_unique<BitgetExchangeConnector>(cfg, &bookBus, &tradeBus, &orderBus,
                                                          &registry, logger);

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

}  // namespace

TEST(FillWatermark, ReportsOnlyWhatACumulativeTotalAdds)
{
  FillWatermark watermark;

  EXPECT_TRUE(watermark.reported(1).isZero());
  EXPECT_EQ(watermark.advance(1, qty(1.0)).toDouble(), 1.0);
  EXPECT_EQ(watermark.reported(1).toDouble(), 1.0);

  // The same total again -- the other channel reporting the same execution.
  EXPECT_TRUE(watermark.advance(1, qty(1.0)).isZero());
  EXPECT_EQ(watermark.advance(1, qty(3.0)).toDouble(), 2.0);

  // A total that went backwards is a stale frame, not a negative fill.
  EXPECT_TRUE(watermark.advance(1, qty(2.0)).isZero());
  EXPECT_EQ(watermark.reported(1).toDouble(), 3.0);
}

TEST(FillWatermark, OrdersAreIndependent)
{
  FillWatermark watermark;

  EXPECT_EQ(watermark.advance(1, qty(2.0)).toDouble(), 2.0);
  EXPECT_EQ(watermark.advance(2, qty(5.0)).toDouble(), 5.0);
  EXPECT_TRUE(watermark.advance(1, qty(2.0)).isZero());
  EXPECT_EQ(watermark.reported(2).toDouble(), 5.0);
}

// A completed order keeps its watermark, because the duplicate of the last
// fill arrives on the other channel after the terminal status. It is only the
// eviction window that ends.
TEST(FillWatermark, ACompletedOrderStillRejectsItsDuplicate)
{
  FillWatermark watermark;

  EXPECT_EQ(watermark.advance(7, qty(3.0)).toDouble(), 3.0);
  watermark.complete(7);
  watermark.complete(7);  // idempotent

  EXPECT_TRUE(watermark.advance(7, qty(3.0)).isZero())
      << "the terminal fill announced on a second channel must add nothing";
}

TEST(FillWatermark, EvictsCompletedOrdersOnceTheHistoryIsFull)
{
  FillWatermark watermark(2);

  for (OrderId id = 1; id <= 3; ++id)
  {
    EXPECT_EQ(watermark.advance(id, qty(1.0)).toDouble(), 1.0);
    watermark.complete(id);
  }

  // Order 1 fell out of the two-deep history; the two newest are still held.
  EXPECT_TRUE(watermark.reported(1).isZero());
  EXPECT_EQ(watermark.reported(2).toDouble(), 1.0);
  EXPECT_EQ(watermark.reported(3).toDouble(), 1.0);
}

// Bitget re-pushes an order's current state on every private resubscribe. The
// repeat carries the same accBaseVolume, so it must add nothing.
TEST(BitgetFillContractExtra, RepushedOrderStateDoesNotBookASecondFill)
{
  BitgetFixture f("bitget_repush.log");

  f.connector->handlePrivateMessage(ordersFrame("partially_filled", "60000", "1", "1"));
  f.connector->handlePrivateMessage(ordersFrame("partially_filled", "60000", "1", "1"));
  f.connector->handlePrivateMessage(ordersFrame("filled", "60010", "2", "3"));
  f.connector->handlePrivateMessage(ordersFrame("filled", "60010", "2", "3"));
  f.orderBus.flush();

  const auto fills = f.listener.fills();
  ASSERT_EQ(fills.size(), 2u) << "two executions were pushed twice each; the order's size is 3 and "
                                 "the bus must not deliver 6";
  EXPECT_EQ(fills[0].dispatchedAs, OrderEventStatus::PARTIALLY_FILLED);
  EXPECT_DOUBLE_EQ(fills[0].fillQty, 1.0);
  EXPECT_EQ(fills[1].dispatchedAs, OrderEventStatus::FILLED);
  EXPECT_DOUBLE_EQ(fills[1].fillQty, 2.0);
  EXPECT_DOUBLE_EQ(fills[1].fillPrice, 60010.0);
}

// complete() is called from a per-frame path, so the same order reaches it
// once per terminal frame the venue sends -- and venues re-push terminal state
// after a resubscribe. Only the first call may take a slot in the completion
// queue. If every call queued the order again, its own repeats would walk the
// history window forward and evict the entry the window exists to protect: the
// order would be forgotten while its duplicates are still arriving, which is
// exactly the double-booking the watermark is there to stop.
TEST(FillWatermark, RepeatedCompletionOfOneOrderTakesOneHistorySlot)
{
  constexpr size_t kHistory = 4;
  FillWatermark watermark(kHistory);

  EXPECT_EQ(watermark.advance(7, qty(3.0)).toDouble(), 3.0);

  // The venue re-pushes the terminal frame enough times to fill the window on
  // its own.
  for (size_t i = 0; i < kHistory + 1; ++i)
  {
    watermark.complete(7);
  }

  // One unrelated order completes. With one slot per order that is the second
  // entry in a four-deep queue and evicts nothing.
  EXPECT_EQ(watermark.advance(8, qty(1.0)).toDouble(), 1.0);
  watermark.complete(8);

  EXPECT_EQ(watermark.reported(7).toDouble(), 3.0)
      << "order 7 occupies one history slot however many times it completed; it must still be "
         "held after a single unrelated completion";
  EXPECT_TRUE(watermark.advance(7, qty(3.0)).isZero())
      << "and its late duplicate must still add nothing";
  EXPECT_EQ(watermark.reported(8).toDouble(), 1.0);
}
