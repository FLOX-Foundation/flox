# First Strategy

Write a simple trading strategy that reacts to market data.

## Prerequisites

- Completed [Quickstart](quickstart.md)
- Basic C++ knowledge

## 1. Strategy Interface

All strategies implement `IStrategy`, which combines:

- `ISubsystem` — lifecycle (`start()`, `stop()`)
- `IMarketDataSubscriber` — market data callbacks

```cpp
#include "flox/strategy/abstract_strategy.h"

class MyStrategy : public flox::IStrategy
{
public:
  // Required: unique identifier for event routing
  flox::SubscriberId id() const override {
    return reinterpret_cast<flox::SubscriberId>(this);
  }

  // Lifecycle
  void start() override { /* initialization */ }
  void stop() override { /* cleanup */ }

  // Market data callbacks
  void onTrade(const flox::TradeEvent& ev) override { /* react to trades */ }
  void onBookUpdate(const flox::BookUpdateEvent& ev) override { /* react to book changes */ }
  void onBar(const flox::BarEvent& ev) override { /* react to bars */ }
  void onMarketDataError(const flox::MarketDataError& error) override { /* handle errors */ }
};
```

## 2. Simple Example

A strategy that prints trades and tracks best bid/ask:

```cpp
#include "flox/strategy/abstract_strategy.h"
#include "flox/book/events/trade_event.h"
#include "flox/book/events/book_update_event.h"
#include "flox/log/log.h"

using namespace flox;

class PrintingStrategy : public IStrategy
{
public:
  explicit PrintingStrategy(SymbolId symbol) : _symbol(symbol) {}

  SubscriberId id() const override {
    return reinterpret_cast<SubscriberId>(this);
  }

  void start() override {
    FLOX_LOG("[PrintingStrategy] Started for symbol " << _symbol);
  }

  void stop() override {
    FLOX_LOG("[PrintingStrategy] Stopped. Trades seen: " << _tradeCount);
  }

  void onTrade(const TradeEvent& ev) override {
    // Filter by symbol
    if (ev.trade.symbol != _symbol) return;

    ++_tradeCount;
    FLOX_LOG("Trade: " << ev.trade.price.toDouble()
             << " x " << ev.trade.quantity.toDouble()
             << " (" << (ev.trade.isBuy ? "BUY" : "SELL") << ")");
  }

  void onBookUpdate(const BookUpdateEvent& ev) override {
    if (ev.update.symbol != _symbol) return;

    if (!ev.update.bids.empty()) {
      _bestBid = ev.update.bids[0].price;
    }
    if (!ev.update.asks.empty()) {
      _bestAsk = ev.update.asks[0].price;
    }

    FLOX_LOG("Book: " << _bestBid.toDouble() << " / " << _bestAsk.toDouble());
  }

private:
  SymbolId _symbol;
  uint64_t _tradeCount{0};
  Price _bestBid{};
  Price _bestAsk{};
};
```

## 3. Strategy with Order Execution

A strategy that submits orders based on trades:

```cpp
#include "flox/strategy/abstract_strategy.h"
#include "flox/execution/abstract_executor.h"
#include "flox/execution/order.h"

using namespace flox;

class TradingStrategy : public IStrategy
{
public:
  TradingStrategy(SymbolId symbol, IOrderExecutor* executor)
    : _symbol(symbol), _executor(executor) {}

  SubscriberId id() const override {
    return reinterpret_cast<SubscriberId>(this);
  }

  void onTrade(const TradeEvent& ev) override {
    if (ev.trade.symbol != _symbol) return;

    // Simple logic: buy after every 10th trade
    if (++_tradeCount % 10 != 0) return;

    Order order{};
    order.id = _nextOrderId++;
    order.symbol = _symbol;
    order.side = Side::BUY;
    order.price = ev.trade.price - Price::fromDouble(0.01);  // 1 cent below
    order.quantity = Quantity::fromDouble(1.0);
    order.type = OrderType::LIMIT;

    _executor->submitOrder(order);
  }

private:
  SymbolId _symbol;
  IOrderExecutor* _executor;
  uint64_t _tradeCount{0};
  OrderId _nextOrderId{1};
};
```

## 4. Wiring the Strategy

Connect your strategy to the engine:

```cpp
#include "flox/book/bus/trade_bus.h"
#include "flox/book/bus/book_update_bus.h"
#include "flox/engine/engine.h"

// Create buses
auto tradeBus = std::make_unique<TradeBus>();
auto bookBus = std::make_unique<BookUpdateBus>();

// Create strategy
auto strategy = std::make_unique<PrintingStrategy>(/*symbolId=*/0);

// Subscribe to buses
tradeBus->subscribe(strategy.get());
bookBus->subscribe(strategy.get());

// Add to subsystems
std::vector<std::unique_ptr<ISubsystem>> subsystems;
subsystems.push_back(std::move(tradeBus));
subsystems.push_back(std::move(bookBus));
subsystems.push_back(std::move(strategy));

// Create and run engine
EngineConfig config{};
Engine engine(config, std::move(subsystems), std::move(connectors));
engine.start();
```

## 5. Best Practices

**Do:**

- Keep callbacks fast and non-blocking
- Filter events by symbol early (first line of callback)
- Use `Price::fromDouble()` and `Quantity::fromDouble()` for conversions
- Implement `id()` to return a unique value

**Don't:**

- Block in callbacks (no I/O, no locks, no allocations)
- Store pointers to events (they're recycled)
- Throw exceptions from callbacks

## Key Types

| Type | Description |
|------|-------------|
| `SymbolId` | `uint32_t` identifier for an instrument |
| `Price` | Fixed-point price (use `fromDouble()`, `toDouble()`) |
| `Quantity` | Fixed-point quantity |
| `Side` | `BUY` or `SELL` |
| `OrderType` | `LIMIT`, `MARKET`, etc. |

## Next Steps

- [Recording Data](recording-data.md) — Capture market data to disk
- [Architecture Overview](../explanation/architecture.md) — Understand the event flow
