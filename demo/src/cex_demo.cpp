/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/book/composite_book_matrix.h"
#include "flox/engine/symbol_registry.h"
#include "flox/exchange/exchange_info.h"
#include "flox/execution/order_router.h"
#include "flox/execution/split_order_tracker.h"
#include "flox/position/aggregated_position_tracker.h"
#include "flox/util/sync/exchange_clock_sync.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <thread>

using namespace flox;

// Simulated order executor for demo purposes
class SimulatedExecutor : public IOrderExecutor
{
 public:
  explicit SimulatedExecutor(std::string name) : _name(std::move(name)) {}

  void submit(SymbolId symbol,
              Side side,
              int64_t priceRaw,
              int64_t quantityRaw,
              OrderId orderId) override
  {
    std::cout << "  [" << _name << "] ORDER: id=" << orderId << " " << (side == Side::BUY ? "BUY" : "SELL") << " "
              << (quantityRaw / 1'000'000.0) << " @ " << (priceRaw / 1'000'000.0) << "\n";
    ++_orderCount;
    _totalVolume += quantityRaw;
  }

  void cancel(OrderId orderId) override
  {
    std::cout << "  [" << _name << "] CANCEL: id=" << orderId << "\n";
  }

  int orderCount() const { return _orderCount; }
  int64_t totalVolume() const { return _totalVolume; }

 private:
  std::string _name;
  int _orderCount{0};
  int64_t _totalVolume{0};
};

void printSeparator(const std::string& title)
{
  std::cout << "\n"
            << std::string(60, '=') << "\n";
  std::cout << " " << title << "\n";
  std::cout << std::string(60, '=') << "\n\n";
}

void demoExchangeRegistration()
{
  printSeparator("1. Exchange Registration");

  SymbolRegistry registry;

  // Register exchanges
  ExchangeId binance = registry.registerExchange("Binance", VenueType::CentralizedExchange);
  ExchangeId bybit = registry.registerExchange("Bybit", VenueType::CentralizedExchange);
  ExchangeId kraken = registry.registerExchange("Kraken", VenueType::CentralizedExchange);

  std::cout << "Registered exchanges:\n";
  for (ExchangeId ex = 0; ex < registry.exchangeCount(); ++ex)
  {
    const auto* info = registry.getExchange(ex);
    std::cout << "  [" << static_cast<int>(ex) << "] " << info->nameView() << "\n";
  }

  // Register symbols with exchange association
  SymbolId btcBinance = registry.registerSymbol(binance, "BTCUSDT");
  SymbolId btcBybit = registry.registerSymbol(bybit, "BTCUSDT");
  SymbolId btcKraken = registry.registerSymbol(kraken, "XBTUSDT");

  std::cout << "\nRegistered symbols:\n";
  std::cout << "  BTC on Binance: SymbolId=" << btcBinance << "\n";
  std::cout << "  BTC on Bybit:   SymbolId=" << btcBybit << "\n";
  std::cout << "  BTC on Kraken:  SymbolId=" << btcKraken << " (XBTUSDT)\n";

  // Map equivalent symbols
  std::array<SymbolId, 3> equivalents = {btcBinance, btcBybit, btcKraken};
  registry.mapEquivalentSymbols(equivalents);

  std::cout << "\nEquivalent symbols for BTC (from Binance):\n";
  auto eqs = registry.getEquivalentSymbols(btcBinance);
  for (SymbolId eq : eqs)
  {
    ExchangeId exId = registry.getExchangeForSymbol(eq);
    const auto* exInfo = registry.getExchange(exId);
    std::cout << "  SymbolId=" << eq << " on " << exInfo->nameView() << "\n";
  }
}

void demoClockSync()
{
  printSeparator("2. Clock Synchronization");

  ExchangeClockSync<4> clockSync;

  std::cout << "Simulating RTT measurements to exchanges...\n\n";

  // Simulate RTT measurements
  // Binance: ~5ms latency
  for (int i = 0; i < 10; ++i)
  {
    int64_t localSend = i * 100'000'000LL;                     // 100ms apart
    int64_t exchangeTs = localSend + 5'000'000LL + 100'000;    // Exchange is ~5ms ahead
    int64_t localRecv = localSend + 10'000'000LL + i * 10000;  // 10ms RTT with jitter
    clockSync.recordSample(0, localSend, exchangeTs, localRecv);
  }

  // Bybit: ~15ms latency
  for (int i = 0; i < 10; ++i)
  {
    int64_t localSend = i * 100'000'000LL;
    int64_t exchangeTs = localSend + 2'000'000LL;  // Exchange is ~2ms ahead
    int64_t localRecv = localSend + 30'000'000LL;  // 30ms RTT
    clockSync.recordSample(1, localSend, exchangeTs, localRecv);
  }

  // Kraken: ~25ms latency
  for (int i = 0; i < 10; ++i)
  {
    int64_t localSend = i * 100'000'000LL;
    int64_t exchangeTs = localSend - 1'000'000LL;  // Exchange is ~1ms behind
    int64_t localRecv = localSend + 50'000'000LL;  // 50ms RTT
    clockSync.recordSample(2, localSend, exchangeTs, localRecv);
  }

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "Exchange clock estimates:\n";
  for (ExchangeId ex = 0; ex < 3; ++ex)
  {
    auto est = clockSync.estimate(ex);
    const char* names[] = {"Binance", "Bybit", "Kraken"};
    std::cout << "  " << names[ex] << ":\n";
    std::cout << "    Offset:     " << (est.offsetNs / 1'000'000.0) << " ms\n";
    std::cout << "    Latency:    " << (est.latencyNs / 1'000'000.0) << " ms\n";
    std::cout << "    Confidence: ±" << (est.confidenceNs / 1'000'000.0) << " ms\n";
    std::cout << "    Samples:    " << est.sampleCount << "\n";
  }
}

void demoCompositeBook()
{
  printSeparator("3. Composite Order Book");

  CompositeBookMatrix<4> matrix;
  std::pmr::monotonic_buffer_resource pool(64 * 1024);

  std::cout << "Simulating book updates from 3 exchanges...\n\n";

  // Binance: bid 50000, ask 50001
  {
    BookUpdateEvent ev(&pool);
    ev.update.symbol = 1;
    ev.sourceExchange = 0;
    ev.update.bids.emplace_back(Price::fromRaw(50000'000'000LL), Quantity::fromRaw(10'000'000LL));
    ev.update.asks.emplace_back(Price::fromRaw(50001'000'000LL), Quantity::fromRaw(5'000'000LL));
    matrix.onBookUpdate(ev);
    std::cout << "  Binance:  bid=50000.00 (10) / ask=50001.00 (5)\n";
  }

  // Bybit: bid 49999, ask 50000.50
  {
    BookUpdateEvent ev(&pool);
    ev.update.symbol = 1;
    ev.sourceExchange = 1;
    ev.update.bids.emplace_back(Price::fromRaw(49999'000'000LL), Quantity::fromRaw(15'000'000LL));
    ev.update.asks.emplace_back(Price::fromRaw(50000'500'000LL), Quantity::fromRaw(8'000'000LL));
    matrix.onBookUpdate(ev);
    std::cout << "  Bybit:    bid=49999.00 (15) / ask=50000.50 (8)\n";
  }

  // Kraken: bid 50001, ask 50002
  {
    BookUpdateEvent ev(&pool);
    ev.update.symbol = 1;
    ev.sourceExchange = 2;
    ev.update.bids.emplace_back(Price::fromRaw(50001'000'000LL), Quantity::fromRaw(5'000'000LL));
    ev.update.asks.emplace_back(Price::fromRaw(50002'000'000LL), Quantity::fromRaw(3'000'000LL));
    matrix.onBookUpdate(ev);
    std::cout << "  Kraken:   bid=50001.00 (5) / ask=50002.00 (3)\n";
  }

  std::cout << "\nComposite best quotes:\n";
  auto bestBid = matrix.bestBid(1);
  auto bestAsk = matrix.bestAsk(1);

  const char* names[] = {"Binance", "Bybit", "Kraken"};
  std::cout << "  Best Bid: " << (bestBid.priceRaw / 1'000'000.0) << " on " << names[bestBid.exchange]
            << " (qty=" << (bestBid.qtyRaw / 1'000'000.0) << ")\n";
  std::cout << "  Best Ask: " << (bestAsk.priceRaw / 1'000'000.0) << " on " << names[bestAsk.exchange]
            << " (qty=" << (bestAsk.qtyRaw / 1'000'000.0) << ")\n";
  std::cout << "  Spread:   " << ((bestAsk.priceRaw - bestBid.priceRaw) / 1'000'000.0) << "\n";

  // Check for arbitrage
  std::cout << "\nArbitrage opportunity: ";
  if (matrix.hasArbitrageOpportunity(1))
  {
    std::cout << "YES! Buy on " << names[bestAsk.exchange] << " @ " << (bestAsk.priceRaw / 1'000'000.0) << ", sell on "
              << names[bestBid.exchange] << " @ " << (bestBid.priceRaw / 1'000'000.0) << "\n";
  }
  else
  {
    std::cout << "No (spread is positive)\n";
  }

  // Mark Kraken as stale
  std::cout << "\nMarking Kraken as stale...\n";
  matrix.markStale(2, 1);

  bestBid = matrix.bestBid(1);
  bestAsk = matrix.bestAsk(1);
  std::cout << "  Best Bid after staleness: " << (bestBid.priceRaw / 1'000'000.0) << " on " << names[bestBid.exchange]
            << "\n";
}

void demoPositionTracking()
{
  printSeparator("4. Aggregated Position Tracking");

  AggregatedPositionTracker<4> tracker;

  std::cout << "Simulating fills across exchanges...\n\n";

  // Buy 100 @ 50000 on Binance
  tracker.onFill(0, 1, Quantity::fromDouble(100), Price::fromDouble(50000));
  std::cout << "  Binance: BUY  100 @ 50000\n";

  // Buy 50 @ 50001 on Bybit
  tracker.onFill(1, 1, Quantity::fromDouble(50), Price::fromDouble(50001));
  std::cout << "  Bybit:   BUY   50 @ 50001\n";

  // Sell 30 @ 50002 on Kraken
  tracker.onFill(2, 1, Quantity::fromDouble(-30), Price::fromDouble(50002));
  std::cout << "  Kraken:  SELL  30 @ 50002\n";

  std::cout << "\nPositions by exchange:\n";
  const char* names[] = {"Binance", "Bybit", "Kraken"};
  for (ExchangeId ex = 0; ex < 3; ++ex)
  {
    auto pos = tracker.position(ex, 1);
    std::cout << "  " << names[ex] << ": qty=" << pos.quantity.toDouble()
              << ", avg_entry=" << pos.avgEntryPrice.toDouble() << "\n";
  }

  auto total = tracker.totalPosition(1);
  std::cout << "\nAggregated position:\n";
  std::cout << "  Total qty: " << total.quantity.toDouble() << "\n";
  std::cout << "  Avg entry: " << total.avgEntryPrice.toDouble() << "\n";

  // Calculate unrealized PnL at current price
  Price currentPrice = Price::fromDouble(50100);
  Volume unrealizedPnl = tracker.unrealizedPnl(1, currentPrice);
  std::cout << "\nUnrealized PnL @ 50100: " << unrealizedPnl.toDouble() << "\n";
}

void demoOrderRouting()
{
  printSeparator("5. Smart Order Routing");

  OrderRouter<4> router;

  // Create simulated executors
  SimulatedExecutor binanceExec("Binance");
  SimulatedExecutor bybitExec("Bybit");
  SimulatedExecutor krakenExec("Kraken");

  router.registerExecutor(0, &binanceExec);
  router.registerExecutor(1, &bybitExec);
  router.registerExecutor(2, &krakenExec);

  // Set up clock sync for latency-based routing
  ExchangeClockSync<4> clockSync;
  clockSync.recordSample(0, 0, 5000, 10000);   // 5ms latency
  clockSync.recordSample(1, 0, 15000, 30000);  // 15ms latency
  clockSync.recordSample(2, 0, 25000, 50000);  // 25ms latency
  router.setClockSync(&clockSync);

  std::cout << "Routing strategies demo:\n\n";

  // Round-robin routing
  std::cout << "1. Round-Robin routing (3 orders):\n";
  router.setRoutingStrategy(RoutingStrategy::RoundRobin);
  for (int i = 0; i < 3; ++i)
  {
    ExchangeId routedTo;
    router.route(1, Side::BUY, 50000'000'000LL, 10'000'000LL, 100 + i, &routedTo);
  }

  // Lowest latency routing
  std::cout << "\n2. Lowest-Latency routing:\n";
  router.setRoutingStrategy(RoutingStrategy::LowestLatency);
  ExchangeId routedTo;
  router.route(1, Side::BUY, 50000'000'000LL, 10'000'000LL, 200, &routedTo);
  std::cout << "  -> Routed to exchange " << static_cast<int>(routedTo) << " (lowest latency)\n";

  // Explicit routing
  std::cout << "\n3. Explicit routing to Kraken:\n";
  router.routeTo(2, 1, Side::SELL, 50001'000'000LL, 5'000'000LL, 300);

  // Failover demo
  std::cout << "\n4. Failover demo (Binance disabled):\n";
  router.setEnabled(0, false);
  router.setFailoverPolicy(FailoverPolicy::FailoverToBest);
  router.setRoutingStrategy(RoutingStrategy::LowestLatency);
  auto err = router.route(1, Side::BUY, 50000'000'000LL, 10'000'000LL, 400, &routedTo);
  if (err == RoutingError::Success)
  {
    std::cout << "  -> Failed over to exchange " << static_cast<int>(routedTo) << "\n";
  }

  std::cout << "\nOrder counts:\n";
  std::cout << "  Binance: " << binanceExec.orderCount() << " orders\n";
  std::cout << "  Bybit:   " << bybitExec.orderCount() << " orders\n";
  std::cout << "  Kraken:  " << krakenExec.orderCount() << " orders\n";
}

void demoSplitOrders()
{
  printSeparator("6. Split Order Tracking");

  SplitOrderTracker tracker;

  std::cout << "Splitting order 1000 into 3 child orders:\n";
  std::cout << "  Parent order: 1000 qty\n";

  std::array<OrderId, 3> children = {1001, 1002, 1003};
  tracker.registerSplit(1000, children, 1000'000'000LL, 0);

  std::cout << "  Child orders: 1001, 1002, 1003\n\n";

  // Simulate fills
  std::cout << "Simulating partial fills:\n";
  tracker.onChildFill(1001, 300'000'000LL);
  std::cout << "  Child 1001 filled: 300 qty\n";

  tracker.onChildFill(1002, 400'000'000LL);
  std::cout << "  Child 1002 filled: 400 qty\n";

  auto* state = tracker.getState(1000);
  std::cout << "\nSplit order status:\n";
  std::cout << "  Total qty:  " << (state->totalQtyRaw / 1'000'000.0) << "\n";
  std::cout << "  Filled qty: " << (state->filledQtyRaw / 1'000'000.0) << "\n";
  std::cout << "  Fill ratio: " << (state->fillRatio() * 100) << "%\n";
  std::cout << "  Complete:   " << (tracker.isComplete(1000) ? "YES" : "NO") << "\n";

  // Complete children
  tracker.onChildComplete(1001, true);
  tracker.onChildComplete(1002, true);
  tracker.onChildComplete(1003, true);
  tracker.onChildFill(1003, 300'000'000LL);

  std::cout << "\nAfter all children complete:\n";
  state = tracker.getState(1000);
  std::cout << "  Fill ratio: " << (state->fillRatio() * 100) << "%\n";
  std::cout << "  Complete:   " << (tracker.isComplete(1000) ? "YES" : "NO") << "\n";
  std::cout << "  Successful: " << (tracker.isSuccessful(1000) ? "YES" : "NO") << "\n";
}

void demoArbitrage()
{
  printSeparator("7. Arbitrage Detection & Execution");

  CompositeBookMatrix<4> matrix;
  std::pmr::monotonic_buffer_resource pool(64 * 1024);

  std::cout << "Setting up arbitrage scenario:\n";
  std::cout << "  (Kraken bid > Binance ask = profit opportunity)\n\n";

  // Binance: ask 49999 (cheap to buy)
  {
    BookUpdateEvent ev(&pool);
    ev.update.symbol = 1;
    ev.sourceExchange = 0;
    ev.update.bids.emplace_back(Price::fromRaw(49998'000'000LL), Quantity::fromRaw(10'000'000LL));
    ev.update.asks.emplace_back(Price::fromRaw(49999'000'000LL), Quantity::fromRaw(5'000'000LL));
    matrix.onBookUpdate(ev);
    std::cout << "  Binance: ask=49999 (can buy here)\n";
  }

  // Kraken: bid 50001 (expensive to sell)
  {
    BookUpdateEvent ev(&pool);
    ev.update.symbol = 1;
    ev.sourceExchange = 2;
    ev.update.bids.emplace_back(Price::fromRaw(50001'000'000LL), Quantity::fromRaw(5'000'000LL));
    ev.update.asks.emplace_back(Price::fromRaw(50002'000'000LL), Quantity::fromRaw(3'000'000LL));
    matrix.onBookUpdate(ev);
    std::cout << "  Kraken:  bid=50001 (can sell here)\n";
  }

  auto bestBid = matrix.bestBid(1);
  auto bestAsk = matrix.bestAsk(1);

  std::cout << "\nArbitrage analysis:\n";
  std::cout << "  Buy on Binance @ " << (bestAsk.priceRaw / 1'000'000.0) << "\n";
  std::cout << "  Sell on Kraken @ " << (bestBid.priceRaw / 1'000'000.0) << "\n";

  if (matrix.hasArbitrageOpportunity(1))
  {
    int64_t profit = bestBid.priceRaw - bestAsk.priceRaw;
    int64_t maxQty = std::min(bestBid.qtyRaw, bestAsk.qtyRaw);

    std::cout << "\n  *** ARBITRAGE OPPORTUNITY DETECTED ***\n";
    std::cout << "  Profit per unit:    " << (profit / 1'000'000.0) << "\n";
    std::cout << "  Max executable qty: " << (maxQty / 1'000'000.0) << "\n";
    std::cout << "  Max profit:         " << ((profit * maxQty) / 1'000'000'000'000.0) << "\n";

    // Execute arbitrage
    OrderRouter<4> router;
    SimulatedExecutor binanceExec("Binance");
    SimulatedExecutor krakenExec("Kraken");
    router.registerExecutor(0, &binanceExec);
    router.registerExecutor(2, &krakenExec);

    std::cout << "\nExecuting arbitrage:\n";
    router.routeTo(0, 1, Side::BUY, bestAsk.priceRaw, maxQty, 1);
    router.routeTo(2, 1, Side::SELL, bestBid.priceRaw, maxQty, 2);
  }
}

int main()
{
  std::cout << "Cross-Exchange Coordination Demo\n";

  demoExchangeRegistration();
  demoClockSync();
  demoCompositeBook();
  demoPositionTracking();
  demoOrderRouting();
  demoSplitOrders();
  demoArbitrage();

  printSeparator("Demo Complete");
  std::cout << "All CEX components demonstrated successfully!\n\n";

  return 0;
}
