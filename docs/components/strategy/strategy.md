# Strategy

`Strategy` is the unified base class for implementing trading strategies. It supports both single-symbol and multi-symbol strategies through a single, consistent API.

```cpp
class Strategy : public IStrategy
{
public:
  // Multi-symbol constructor
  Strategy(SubscriberId id, std::vector<SymbolId> symbols,
           const SymbolRegistry& registry);

  // Single-symbol convenience constructor
  Strategy(SubscriberId id, SymbolId symbol,
           const SymbolRegistry& registry);
};
```

The strategy requires a `SymbolRegistry` reference to look up per-symbol metadata (tick size, instrument type, etc.).

## Purpose

- Provide a base class for all trading strategies
- Automatically manage per-symbol state (order books, positions, prices)
- Route market data events to symbol-specific handlers
- Emit trading signals to an executor

## Per-Symbol Context

Each symbol has a `SymbolContext` that aggregates all relevant state:

```cpp
struct SymbolContext
{
  NLevelOrderBook<512> book;     // Order book
  Quantity position{};           // Net position
  Price avgEntryPrice{};         // VWAP entry price
  Price lastTradePrice{};        // Last trade price
  int64_t lastUpdateNs{0};       // Last update timestamp
  SymbolId symbolId{0};          // Symbol identifier

  std::optional<Price> mid() const noexcept;      // Mid price
  std::optional<Price> bookSpread() const noexcept; // Bid-ask spread
  double unrealizedPnl(Price markPrice) const noexcept;
  double unrealizedPnl() const noexcept;          // Uses mid()

  bool isLong() const noexcept;
  bool isShort() const noexcept;
  bool isFlat() const noexcept;
};
```

## Event Handlers

Override these methods to receive per-symbol market data:

```cpp
protected:
  // Called on trade events for subscribed symbols
  virtual void onSymbolTrade(SymbolContext& ctx, const TradeEvent& ev) {}

  // Called on book updates for subscribed symbols
  virtual void onSymbolBook(SymbolContext& ctx, const BookUpdateEvent& ev) {}
```

The base class automatically:
1. Filters events by subscription
2. Updates `SymbolContext` (book, prices, timestamps)
3. Dispatches to the appropriate handler

## Context Access

```cpp
protected:
  // Access context by symbol ID
  SymbolContext& ctx(SymbolId sym) noexcept;
  const SymbolContext& ctx(SymbolId sym) const noexcept;

  // Single-symbol convenience (returns first symbol's context)
  SymbolContext& ctx() noexcept;
  const SymbolContext& ctx() const noexcept;

  // Get primary symbol ID (for single-symbol strategies)
  SymbolId symbol() const noexcept;

  // All subscribed symbols
  const std::vector<SymbolId>& symbols() const noexcept;

  // Check if symbol is subscribed
  bool isSubscribed(SymbolId sym) const noexcept;
```

## Signal Emission

All order methods return an `OrderId` for tracking:

```cpp
protected:
  void emit(const Signal& signal);

  // Returns OrderId for tracking
  OrderId emitMarketBuy(SymbolId symbol, Quantity qty);
  OrderId emitMarketSell(SymbolId symbol, Quantity qty);
  OrderId emitLimitBuy(SymbolId symbol, Price price, Quantity qty);
  OrderId emitLimitSell(SymbolId symbol, Price price, Quantity qty);

  void emitCancel(OrderId orderId);
  void emitCancelAll(SymbolId symbol);
  void emitModify(OrderId orderId, Price newPrice, Quantity newQty);
```

## Order and Position Tracking

Query order status and positions (requires `setOrderTracker` / `setPositionManager`):

```cpp
protected:
  // Position queries
  Quantity position(SymbolId sym) const;  // Net position for symbol
  Quantity position() const;              // Primary symbol position

  // Order status queries
  std::optional<OrderEventStatus> getOrderStatus(OrderId orderId) const;
  std::optional<OrderState> getOrder(OrderId orderId) const;
```

Connect trackers:

```cpp
strategy.setOrderTracker(&orderTracker);
strategy.setPositionManager(&positionTracker);
```

## Cross-Symbol Helpers

Free functions in `symbol_context.h` for multi-symbol strategies:

```cpp
// Price spread between two symbols
std::optional<Price> spread(const SymbolContext& a, const SymbolContext& b);

// Price ratio between two symbols
std::optional<double> ratio(const SymbolContext& a, const SymbolContext& b);
```

## Examples

### Single-Symbol Strategy

```cpp
class MomentumStrategy : public Strategy
{
public:
  MomentumStrategy(SymbolId sym, const SymbolRegistry& registry)
    : Strategy(1, sym, registry) {}

  void start() override { _running = true; }
  void stop() override { _running = false; }

protected:
  void onSymbolTrade(SymbolContext& c, const TradeEvent& ev) override
  {
    if (!_running) return;

    // Access order book
    auto bid = c.book.bestBid();
    auto ask = c.book.bestAsk();
    if (!bid || !ask) return;

    // Check position via tracker
    if (position().isZero() && shouldBuy(ev.trade.price))
    {
      // Returns OrderId for tracking
      OrderId id = emitMarketBuy(c.symbolId, Quantity::fromDouble(1.0));
      _pendingOrder = id;
    }

    // Check order status
    if (_pendingOrder)
    {
      auto status = getOrderStatus(*_pendingOrder);
      if (status && *status == OrderEventStatus::FILLED)
      {
        _pendingOrder = std::nullopt;
      }
    }
  }

private:
  bool _running{false};
  std::optional<OrderId> _pendingOrder;
};
```

### Multi-Symbol Pairs Strategy

```cpp
class PairsStrategy : public Strategy
{
public:
  PairsStrategy(SymbolId leg1, SymbolId leg2, const SymbolRegistry& registry)
    : Strategy(1, {leg1, leg2}, registry), _leg1(leg1), _leg2(leg2) {}

  void start() override {}
  void stop() override {}

protected:
  void onSymbolBook(SymbolContext& c, const BookUpdateEvent& ev) override
  {
    // Check spread between legs
    auto spreadOpt = spread(ctx(_leg1), ctx(_leg2));
    if (!spreadOpt) return;

    double z = zscore(*spreadOpt);

    if (ctx(_leg1).isFlat() && std::abs(z) > 2.0)
    {
      // Open spread position
      if (z > 0)
      {
        emitMarketSell(_leg1, _size);
        emitMarketBuy(_leg2, _size);
      }
      else
      {
        emitMarketBuy(_leg1, _size);
        emitMarketSell(_leg2, _size);
      }
    }
  }

private:
  SymbolId _leg1, _leg2;
  Quantity _size{Quantity::fromDouble(1.0)};
};
```

## Integration

Connect strategy to backtest or live execution:

```cpp
// Create registry
SymbolRegistry registry;
SymbolInfo info;
info.exchange = "BINANCE";
info.symbol = "BTCUSDT";
info.tickSize = Price::fromDouble(0.01);
SymbolId symbolId = registry.registerSymbol(info);

// Backtest
BacktestRunner runner(config);
MyStrategy strategy(symbolId, registry);
runner.setStrategy(&strategy);
auto result = runner.run(*reader);

// Live (with signal handler)
strategy.setSignalHandler(&executor);
engine.addSubscriber(&strategy);
engine.start();
```

## See Also

- [PositionTracker](../position/position_tracker.md) - Track realized PnL with FIFO/LIFO/AVERAGE
- [SymbolContext](symbol_context.md) - Per-symbol state details
- [BacktestRunner](../backtest/backtest_runner.md) - Backtesting framework
