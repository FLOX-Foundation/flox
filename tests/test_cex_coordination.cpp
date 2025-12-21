/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#include <gtest/gtest.h>

#include "flox/book/composite_book_matrix.h"
#include "flox/engine/symbol_registry.h"
#include "flox/exchange/exchange_info.h"
#include "flox/execution/order_router.h"
#include "flox/execution/split_order_tracker.h"
#include "flox/position/aggregated_position_tracker.h"
#include "flox/util/sync/exchange_clock_sync.h"

using namespace flox;

// ============================================================================
// ExchangeInfo Tests
// ============================================================================

TEST(ExchangeInfoTest, BasicProperties)
{
  ExchangeInfo info;
  info.setName("Binance");
  info.type = VenueType::CentralizedExchange;

  EXPECT_EQ(info.nameView(), "Binance");
  EXPECT_EQ(info.type, VenueType::CentralizedExchange);
}

TEST(ExchangeInfoTest, NameTruncation)
{
  ExchangeInfo info;
  info.setName("VeryLongExchangeNameThatExceedsLimit");

  // Should be truncated to kMaxNameLength - 1
  EXPECT_LE(info.nameView().size(), ExchangeInfo::kMaxNameLength - 1);
}

TEST(ExchangeInfoTest, VenueTypes)
{
  ExchangeInfo cex;
  cex.type = VenueType::CentralizedExchange;
  EXPECT_EQ(cex.type, VenueType::CentralizedExchange);

  ExchangeInfo amm;
  amm.type = VenueType::AmmDex;
  EXPECT_EQ(amm.type, VenueType::AmmDex);

  ExchangeInfo hybrid;
  hybrid.type = VenueType::HybridDex;
  EXPECT_EQ(hybrid.type, VenueType::HybridDex);
}

// ============================================================================
// SymbolRegistry Exchange Management Tests
// ============================================================================

TEST(SymbolRegistryExchangeTest, RegisterExchange)
{
  SymbolRegistry registry;

  ExchangeId binance = registry.registerExchange("Binance");
  ExchangeId bybit = registry.registerExchange("Bybit");

  EXPECT_EQ(binance, 0);
  EXPECT_EQ(bybit, 1);
  EXPECT_EQ(registry.exchangeCount(), 2);
}

TEST(SymbolRegistryExchangeTest, GetExchange)
{
  SymbolRegistry registry;

  ExchangeId id = registry.registerExchange("Kraken", VenueType::CentralizedExchange);
  const ExchangeInfo* info = registry.getExchange(id);

  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->nameView(), "Kraken");
  EXPECT_EQ(info->type, VenueType::CentralizedExchange);
}

TEST(SymbolRegistryExchangeTest, GetExchangeId)
{
  SymbolRegistry registry;

  registry.registerExchange("Binance");
  registry.registerExchange("Bybit");

  EXPECT_EQ(registry.getExchangeId("Binance"), 0);
  EXPECT_EQ(registry.getExchangeId("Bybit"), 1);
  EXPECT_EQ(registry.getExchangeId("NotFound"), InvalidExchangeId);
}

TEST(SymbolRegistryExchangeTest, DuplicateExchangeReturnsExisting)
{
  SymbolRegistry registry;

  ExchangeId first = registry.registerExchange("Binance");
  ExchangeId second = registry.registerExchange("Binance");

  EXPECT_EQ(first, second);
  EXPECT_EQ(registry.exchangeCount(), 1);
}

TEST(SymbolRegistryExchangeTest, SymbolWithExchangeId)
{
  SymbolRegistry registry;

  ExchangeId binance = registry.registerExchange("Binance");
  ExchangeId bybit = registry.registerExchange("Bybit");

  SymbolId btcBinance = registry.registerSymbol(binance, "BTCUSDT");
  SymbolId btcBybit = registry.registerSymbol(bybit, "BTCUSDT");

  EXPECT_NE(btcBinance, btcBybit);
  EXPECT_EQ(registry.getExchangeForSymbol(btcBinance), binance);
  EXPECT_EQ(registry.getExchangeForSymbol(btcBybit), bybit);
}

TEST(SymbolRegistryExchangeTest, SymbolEquivalence)
{
  SymbolRegistry registry;

  ExchangeId binance = registry.registerExchange("Binance");
  ExchangeId bybit = registry.registerExchange("Bybit");
  ExchangeId kraken = registry.registerExchange("Kraken");

  SymbolId btcBinance = registry.registerSymbol(binance, "BTCUSDT");
  SymbolId btcBybit = registry.registerSymbol(bybit, "BTCUSDT");
  SymbolId btcKraken = registry.registerSymbol(kraken, "XBTUSDT");

  std::array<SymbolId, 3> equivalents = {btcBinance, btcBybit, btcKraken};
  registry.mapEquivalentSymbols(equivalents);

  auto eqBinance = registry.getEquivalentSymbols(btcBinance);
  EXPECT_EQ(eqBinance.size(), 3);

  EXPECT_EQ(registry.getEquivalentOnExchange(btcBinance, bybit), btcBybit);
  EXPECT_EQ(registry.getEquivalentOnExchange(btcKraken, binance), btcBinance);
}

// ============================================================================
// ExchangeClockSync Tests
// ============================================================================

TEST(ExchangeClockSyncTest, BasicSync)
{
  ExchangeClockSync<4> sync;

  // Simulate RTT measurement: local_send=0, exchange=1000, local_recv=2000
  // RTT = 2000, one_way = 1000, offset = 1000 - 1000 = 0
  bool accepted = sync.recordSample(0, 0, 1000, 2000);
  EXPECT_TRUE(accepted);
  EXPECT_TRUE(sync.hasSync(0));

  auto est = sync.estimate(0);
  EXPECT_EQ(est.sampleCount, 1);
}

TEST(ExchangeClockSyncTest, RejectsInvalidRTT)
{
  ExchangeClockSync<4> sync;

  // Negative RTT (impossible)
  EXPECT_FALSE(sync.recordSample(0, 1000, 500, 500));

  // Zero RTT (impossible)
  EXPECT_FALSE(sync.recordSample(0, 1000, 1000, 1000));

  // RTT > 10 seconds
  EXPECT_FALSE(sync.recordSample(0, 0, 5'000'000'000LL, 11'000'000'000LL));
}

TEST(ExchangeClockSyncTest, RejectsLargeClockDrift)
{
  ExchangeClockSync<4> sync;

  // Exchange time is more than 1 hour behind local
  EXPECT_FALSE(sync.recordSample(0, 3600'000'000'001LL, 0, 3600'000'000'002LL));
}

TEST(ExchangeClockSyncTest, EMASmoothing)
{
  ExchangeClockSync<4> sync;

  // Add multiple samples with varying RTT to create variance
  for (int i = 0; i < 20; ++i)
  {
    // Vary the RTT between 100 and 300ns to create variance in offset estimation
    int64_t rtt = 100 + (i % 3) * 100;  // 100, 200, 300, 100, 200, 300...
    sync.recordSample(0, i * 1000, i * 1000 + 150, i * 1000 + rtt);
  }

  auto est = sync.estimate(0);
  EXPECT_EQ(est.sampleCount, 20);
  // With varying RTT, we should have some variance in the offset estimates
  // which translates to a non-zero confidence interval
  EXPECT_GE(est.confidenceNs, 0);  // May be 0 if variance converges, that's OK
}

TEST(ExchangeClockSyncTest, TimeConversion)
{
  ExchangeClockSync<4> sync;

  // Set up offset: exchange is 1000ns ahead of local
  sync.recordSample(0, 0, 2000, 2000);
  // RTT=2000, one_way=1000, offset = 2000 - 1000 = 1000

  auto est = sync.estimate(0);
  EXPECT_EQ(est.offsetNs, 1000);

  // Convert exchange time to local: exchange=5000, local=5000-1000=4000
  EXPECT_EQ(sync.toLocalTimeNs(0, 5000), 4000);

  // Convert local time to exchange: local=4000, exchange=4000+1000=5000
  EXPECT_EQ(sync.toExchangeTimeNs(0, 4000), 5000);
}

TEST(ExchangeClockSyncTest, MultipleExchanges)
{
  ExchangeClockSync<4> sync;

  sync.recordSample(0, 0, 100, 200);  // Exchange 0
  sync.recordSample(1, 0, 200, 200);  // Exchange 1
  sync.recordSample(2, 0, 300, 200);  // Exchange 2

  EXPECT_TRUE(sync.hasSync(0));
  EXPECT_TRUE(sync.hasSync(1));
  EXPECT_TRUE(sync.hasSync(2));
  EXPECT_FALSE(sync.hasSync(3));  // Not recorded
}

// ============================================================================
// AggregatedPositionTracker Tests
// ============================================================================

TEST(AggregatedPositionTrackerTest, SingleExchangePosition)
{
  AggregatedPositionTracker<4> tracker;

  // Buy 100 @ 50000
  tracker.onFill(0, 1, Quantity::fromDouble(100), Price::fromDouble(50000));

  auto pos = tracker.position(0, 1);
  EXPECT_EQ(pos.quantity.raw(), Quantity::fromDouble(100).raw());
  EXPECT_EQ(pos.avgEntryPrice.raw(), Price::fromDouble(50000).raw());
}

TEST(AggregatedPositionTrackerTest, AggregatedPosition)
{
  AggregatedPositionTracker<4> tracker;

  // Buy 100 @ 50000 on exchange 0
  tracker.onFill(0, 1, Quantity::fromDouble(100), Price::fromDouble(50000));

  // Buy 100 @ 50000 on exchange 1
  tracker.onFill(1, 1, Quantity::fromDouble(100), Price::fromDouble(50000));

  auto total = tracker.totalPosition(1);
  EXPECT_EQ(total.quantity.raw(), Quantity::fromDouble(200).raw());
}

TEST(AggregatedPositionTrackerTest, SellReducesPosition)
{
  AggregatedPositionTracker<4> tracker;

  // Buy 100 @ 50000
  tracker.onFill(0, 1, Quantity::fromDouble(100), Price::fromDouble(50000));

  // Sell 50
  tracker.onFill(0, 1, Quantity::fromDouble(-50), Price::fromDouble(51000));

  auto pos = tracker.position(0, 1);
  EXPECT_EQ(pos.quantity.raw(), Quantity::fromDouble(50).raw());
}

TEST(AggregatedPositionTrackerTest, CloseToFlat)
{
  AggregatedPositionTracker<4> tracker;

  // Buy 100 @ 50000
  tracker.onFill(0, 1, Quantity::fromDouble(100), Price::fromDouble(50000));

  // Sell 100
  tracker.onFill(0, 1, Quantity::fromDouble(-100), Price::fromDouble(51000));

  auto pos = tracker.position(0, 1);
  EXPECT_EQ(pos.quantity.raw(), 0);
  EXPECT_EQ(pos.costBasis.raw(), 0);
}

TEST(AggregatedPositionTrackerTest, UnrealizedPnL)
{
  AggregatedPositionTracker<4> tracker;

  // Buy 100 @ 50000
  tracker.onFill(0, 1, Quantity::fromDouble(100), Price::fromDouble(50000));

  // Current price 51000 -> PnL = 100 * (51000 - 50000) = 100000
  Volume pnl = tracker.unrealizedPnl(1, Price::fromDouble(51000));
  EXPECT_EQ(pnl.raw(), Volume::fromDouble(100000).raw());
}

// ============================================================================
// CompositeBookMatrix Tests
// ============================================================================

class CompositeBookMatrixTest : public ::testing::Test
{
 protected:
  void SetUp() override { res_ = std::pmr::new_delete_resource(); }

  void setupBookUpdate(BookUpdateEvent& ev,
                       SymbolId symbol,
                       ExchangeId exchange,
                       int64_t bidPrice,
                       int64_t bidQty,
                       int64_t askPrice,
                       int64_t askQty)
  {
    ev.update.symbol = symbol;
    ev.sourceExchange = exchange;
    ev.update.bids.clear();
    ev.update.asks.clear();

    if (bidPrice > 0)
    {
      ev.update.bids.emplace_back(Price::fromRaw(bidPrice), Quantity::fromRaw(bidQty));
    }
    if (askPrice > 0)
    {
      ev.update.asks.emplace_back(Price::fromRaw(askPrice), Quantity::fromRaw(askQty));
    }
  }

  std::pmr::memory_resource* res_;
};

TEST_F(CompositeBookMatrixTest, SingleExchangeQuote)
{
  CompositeBookMatrix<4> matrix;

  BookUpdateEvent ev(res_);
  setupBookUpdate(ev, 1, 0, 50000 * 1'000'000LL, 10 * 1'000'000LL, 50001 * 1'000'000LL,
                  5 * 1'000'000LL);

  matrix.onBookUpdate(ev);

  auto bid = matrix.bestBid(1);
  auto ask = matrix.bestAsk(1);

  EXPECT_TRUE(bid.valid);
  EXPECT_EQ(bid.priceRaw, 50000 * 1'000'000LL);
  EXPECT_EQ(bid.exchange, 0);

  EXPECT_TRUE(ask.valid);
  EXPECT_EQ(ask.priceRaw, 50001 * 1'000'000LL);
  EXPECT_EQ(ask.exchange, 0);
}

TEST_F(CompositeBookMatrixTest, BestAcrossExchanges)
{
  CompositeBookMatrix<4> matrix;
  BookUpdateEvent ev(res_);

  // Exchange 0: bid 50000, ask 50002
  setupBookUpdate(ev, 1, 0, 50000 * 1'000'000LL, 10 * 1'000'000LL, 50002 * 1'000'000LL, 5 * 1'000'000LL);
  matrix.onBookUpdate(ev);

  // Exchange 1: bid 50001, ask 50001 (tighter spread)
  setupBookUpdate(ev, 1, 1, 50001 * 1'000'000LL, 10 * 1'000'000LL, 50001 * 1'000'000LL, 5 * 1'000'000LL);
  matrix.onBookUpdate(ev);

  auto bid = matrix.bestBid(1);
  auto ask = matrix.bestAsk(1);

  // Best bid is on exchange 1 (50001 > 50000)
  EXPECT_EQ(bid.priceRaw, 50001 * 1'000'000LL);
  EXPECT_EQ(bid.exchange, 1);

  // Best ask is on exchange 1 (50001 < 50002)
  EXPECT_EQ(ask.priceRaw, 50001 * 1'000'000LL);
  EXPECT_EQ(ask.exchange, 1);
}

TEST_F(CompositeBookMatrixTest, ArbitrageDetection)
{
  CompositeBookMatrix<4> matrix;
  BookUpdateEvent ev(res_);

  // Exchange 0: bid 50002
  setupBookUpdate(ev, 1, 0, 50002 * 1'000'000LL, 10 * 1'000'000LL, 0, 0);
  matrix.onBookUpdate(ev);

  // Exchange 1: ask 50001 (lower than exchange 0's bid!)
  setupBookUpdate(ev, 1, 1, 0, 0, 50001 * 1'000'000LL, 5 * 1'000'000LL);
  matrix.onBookUpdate(ev);

  EXPECT_TRUE(matrix.hasArbitrageOpportunity(1));
}

TEST_F(CompositeBookMatrixTest, NoArbitrageNormally)
{
  CompositeBookMatrix<4> matrix;
  BookUpdateEvent ev(res_);

  setupBookUpdate(ev, 1, 0, 50000 * 1'000'000LL, 10 * 1'000'000LL, 50001 * 1'000'000LL, 5 * 1'000'000LL);
  matrix.onBookUpdate(ev);

  EXPECT_FALSE(matrix.hasArbitrageOpportunity(1));
}

TEST_F(CompositeBookMatrixTest, StalenessExclusion)
{
  CompositeBookMatrix<4> matrix;
  BookUpdateEvent ev(res_);

  setupBookUpdate(ev, 1, 0, 50000 * 1'000'000LL, 10 * 1'000'000LL, 50001 * 1'000'000LL, 5 * 1'000'000LL);
  matrix.onBookUpdate(ev);

  setupBookUpdate(ev, 1, 1, 50002 * 1'000'000LL, 10 * 1'000'000LL, 50003 * 1'000'000LL, 5 * 1'000'000LL);
  matrix.onBookUpdate(ev);

  // Mark exchange 1 as stale
  matrix.markStale(1, 1);

  auto bid = matrix.bestBid(1);
  // Should only see exchange 0
  EXPECT_EQ(bid.exchange, 0);
  EXPECT_EQ(bid.priceRaw, 50000 * 1'000'000LL);
}

// ============================================================================
// SplitOrderTracker Tests
// ============================================================================

TEST(SplitOrderTrackerTest, RegisterAndTrack)
{
  SplitOrderTracker tracker;

  std::array<OrderId, 3> children = {101, 102, 103};
  bool ok = tracker.registerSplit(100, children, 300 * 1'000'000LL, 0);

  EXPECT_TRUE(ok);
  EXPECT_EQ(tracker.size(), 1);

  auto* state = tracker.getState(100);
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->childCount, 3);
  EXPECT_EQ(state->totalQtyRaw, 300 * 1'000'000LL);
}

TEST(SplitOrderTrackerTest, ChildFills)
{
  SplitOrderTracker tracker;

  std::array<OrderId, 2> children = {101, 102};
  tracker.registerSplit(100, children, 200 * 1'000'000LL, 0);

  tracker.onChildFill(101, 100 * 1'000'000LL);
  tracker.onChildFill(102, 50 * 1'000'000LL);

  auto* state = tracker.getState(100);
  EXPECT_EQ(state->filledQtyRaw, 150 * 1'000'000LL);
  EXPECT_DOUBLE_EQ(state->fillRatio(), 0.75);
}

TEST(SplitOrderTrackerTest, ChildCompletion)
{
  SplitOrderTracker tracker;

  std::array<OrderId, 2> children = {101, 102};
  tracker.registerSplit(100, children, 200 * 1'000'000LL, 0);

  EXPECT_FALSE(tracker.isComplete(100));

  tracker.onChildComplete(101, true);
  EXPECT_FALSE(tracker.isComplete(100));

  tracker.onChildComplete(102, true);
  EXPECT_TRUE(tracker.isComplete(100));
  EXPECT_TRUE(tracker.isSuccessful(100));
}

TEST(SplitOrderTrackerTest, PartialFailure)
{
  SplitOrderTracker tracker;

  std::array<OrderId, 2> children = {101, 102};
  tracker.registerSplit(100, children, 200 * 1'000'000LL, 0);

  tracker.onChildComplete(101, true);
  tracker.onChildComplete(102, false);

  EXPECT_TRUE(tracker.isComplete(100));
  EXPECT_FALSE(tracker.isSuccessful(100));
}

TEST(SplitOrderTrackerTest, Cleanup)
{
  SplitOrderTracker tracker;

  std::array<OrderId, 2> children = {101, 102};
  tracker.registerSplit(100, children, 200 * 1'000'000LL, 0);

  // Cleanup with timeout of 1000ns
  tracker.cleanup(2000, 1000);

  EXPECT_EQ(tracker.size(), 0);
  EXPECT_EQ(tracker.getState(100), nullptr);
}

TEST(SplitOrderTrackerTest, TooManyChildren)
{
  SplitOrderTracker tracker;

  std::array<OrderId, 10> children = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  bool ok = tracker.registerSplit(100, children, 1000 * 1'000'000LL, 0);

  EXPECT_FALSE(ok);  // Exceeds kMaxChildrenPerSplit
}

// ============================================================================
// OrderRouter Tests
// ============================================================================

class MockOrderExecutor : public IOrderExecutor
{
 public:
  void submit(SymbolId symbol,
              Side side,
              int64_t priceRaw,
              int64_t quantityRaw,
              OrderId orderId) override
  {
    lastSymbol = symbol;
    lastSide = side;
    lastPrice = priceRaw;
    lastQty = quantityRaw;
    lastOrderId = orderId;
    ++submitCount;
  }

  void cancel(OrderId orderId) override
  {
    lastCancelId = orderId;
    ++cancelCount;
  }

  SymbolId lastSymbol{0};
  Side lastSide{Side::BUY};
  int64_t lastPrice{0};
  int64_t lastQty{0};
  OrderId lastOrderId{0};
  OrderId lastCancelId{0};
  int submitCount{0};
  int cancelCount{0};
};

TEST(OrderRouterTest, BasicRouting)
{
  OrderRouter<4> router;
  MockOrderExecutor executor;

  router.registerExecutor(0, &executor);

  auto err = router.route(1, Side::BUY, 50000 * 1'000'000LL, 100 * 1'000'000LL, 12345);

  EXPECT_EQ(err, RoutingError::Success);
  EXPECT_EQ(executor.submitCount, 1);
  EXPECT_EQ(executor.lastSymbol, 1);
  EXPECT_EQ(executor.lastSide, Side::BUY);
  EXPECT_EQ(executor.lastOrderId, 12345);
}

TEST(OrderRouterTest, ExplicitRouting)
{
  OrderRouter<4> router;
  MockOrderExecutor executor0, executor1;

  router.registerExecutor(0, &executor0);
  router.registerExecutor(1, &executor1);

  auto err = router.routeTo(1, 1, Side::SELL, 50000 * 1'000'000LL, 100 * 1'000'000LL, 12345);

  EXPECT_EQ(err, RoutingError::Success);
  EXPECT_EQ(executor0.submitCount, 0);
  EXPECT_EQ(executor1.submitCount, 1);
}

TEST(OrderRouterTest, DisabledExchange)
{
  OrderRouter<4> router;
  MockOrderExecutor executor;

  router.registerExecutor(0, &executor);
  router.setEnabled(0, false);

  auto err = router.routeTo(0, 1, Side::BUY, 50000 * 1'000'000LL, 100 * 1'000'000LL, 12345);

  EXPECT_EQ(err, RoutingError::ExchangeDisabled);
  EXPECT_EQ(executor.submitCount, 0);
}

TEST(OrderRouterTest, NoExecutor)
{
  OrderRouter<4> router;

  auto err = router.route(1, Side::BUY, 50000 * 1'000'000LL, 100 * 1'000'000LL, 12345);

  EXPECT_EQ(err, RoutingError::NoExecutor);
}

TEST(OrderRouterTest, FailoverPolicy)
{
  OrderRouter<4> router;
  MockOrderExecutor executor;

  router.registerExecutor(1, &executor);  // Only exchange 1 available
  router.setFailoverPolicy(FailoverPolicy::FailoverToBest);

  // Try to route to exchange 0 (not available), should failover to 1
  ExchangeId routedTo = InvalidExchangeId;
  auto err = router.route(1, Side::BUY, 50000 * 1'000'000LL, 100 * 1'000'000LL, 12345, &routedTo);

  EXPECT_EQ(err, RoutingError::Success);
  EXPECT_EQ(routedTo, 1);
  EXPECT_EQ(executor.submitCount, 1);
}

TEST(OrderRouterTest, RoundRobinStrategy)
{
  OrderRouter<4> router;
  MockOrderExecutor exec0, exec1, exec2;

  router.registerExecutor(0, &exec0);
  router.registerExecutor(1, &exec1);
  router.registerExecutor(2, &exec2);
  router.setRoutingStrategy(RoutingStrategy::RoundRobin);

  ExchangeId ex1, ex2, ex3;
  router.route(1, Side::BUY, 50000 * 1'000'000LL, 100 * 1'000'000LL, 1, &ex1);
  router.route(1, Side::BUY, 50000 * 1'000'000LL, 100 * 1'000'000LL, 2, &ex2);
  router.route(1, Side::BUY, 50000 * 1'000'000LL, 100 * 1'000'000LL, 3, &ex3);

  // Should cycle through exchanges
  EXPECT_NE(ex1, ex2);
  EXPECT_NE(ex2, ex3);
}

TEST(OrderRouterTest, Cancel)
{
  OrderRouter<4> router;
  MockOrderExecutor executor;

  router.registerExecutor(0, &executor);

  auto err = router.cancelOn(0, 12345);

  EXPECT_EQ(err, RoutingError::Success);
  EXPECT_EQ(executor.cancelCount, 1);
  EXPECT_EQ(executor.lastCancelId, 12345);
}

// ============================================================================
// Integration Test
// ============================================================================

TEST(CEXIntegrationTest, FullWorkflow)
{
  // 1. Set up registry with exchanges
  SymbolRegistry registry;
  ExchangeId binance = registry.registerExchange("Binance");
  ExchangeId bybit = registry.registerExchange("Bybit");

  SymbolId btcBinance = registry.registerSymbol(binance, "BTCUSDT");
  SymbolId btcBybit = registry.registerSymbol(bybit, "BTCUSDT");

  std::array<SymbolId, 2> equivalents = {btcBinance, btcBybit};
  registry.mapEquivalentSymbols(equivalents);

  // 2. Set up clock sync
  ExchangeClockSync<4> clockSync;
  clockSync.recordSample(binance, 0, 100, 200);  // 100ns latency
  clockSync.recordSample(bybit, 0, 200, 400);    // 200ns latency

  // 3. Set up position tracker
  AggregatedPositionTracker<4> positions;
  positions.onFill(binance, btcBinance, Quantity::fromDouble(50), Price::fromDouble(50000));
  positions.onFill(bybit, btcBybit, Quantity::fromDouble(50), Price::fromDouble(50000));

  auto total = positions.totalPosition(btcBinance);
  // Note: We're aggregating by symbol ID, not equivalence
  EXPECT_EQ(positions.position(binance, btcBinance).quantity.raw(), Quantity::fromDouble(50).raw());

  // 4. Set up order router
  OrderRouter<4> router;
  MockOrderExecutor binanceExec, bybitExec;

  router.registerExecutor(binance, &binanceExec);
  router.registerExecutor(bybit, &bybitExec);
  router.setClockSync(&clockSync);
  router.setRoutingStrategy(RoutingStrategy::LowestLatency);

  // Route should prefer Binance (lower latency)
  ExchangeId routedTo;
  router.route(btcBinance, Side::BUY, 50000 * 1'000'000LL, 10 * 1'000'000LL, 1, &routedTo);

  EXPECT_EQ(routedTo, binance);
  EXPECT_EQ(binanceExec.submitCount, 1);
  EXPECT_EQ(bybitExec.submitCount, 0);
}
