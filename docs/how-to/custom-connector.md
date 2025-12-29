# Write a Custom Connector

Connect FLOX to a new exchange or data source.

## Overview

A connector:

1. Connects to an exchange API (WebSocket, REST, FIX, etc.)
2. Parses incoming messages
3. Converts to FLOX event types
4. Emits events to the engine

## 1. Implement IExchangeConnector

**Header:** `flox/connector/abstract_exchange_connector.h`

```cpp
#include "flox/connector/abstract_exchange_connector.h"
#include "flox/book/events/trade_event.h"
#include "flox/book/events/book_update_event.h"

class MyExchangeConnector : public flox::IExchangeConnector
{
public:
  MyExchangeConnector(const std::string& symbol, flox::SymbolId symbolId)
    : _symbol(symbol), _symbolId(symbolId) {}

  std::string exchangeId() const override {
    return "myexchange";
  }

  void start() override {
    _running = true;
    _thread = std::thread(&MyExchangeConnector::run, this);
  }

  void stop() override {
    _running = false;
    if (_thread.joinable()) {
      _thread.join();
    }
  }

private:
  void run() {
    // Connect to exchange
    connect();

    while (_running) {
      // Receive and parse messages
      auto msg = receiveMessage();
      handleMessage(msg);
    }

    disconnect();
  }

  void handleMessage(const Message& msg) {
    if (msg.type == MessageType::Trade) {
      handleTrade(msg);
    } else if (msg.type == MessageType::BookUpdate) {
      handleBookUpdate(msg);
    }
  }

  void handleTrade(const Message& msg) {
    flox::TradeEvent ev;
    ev.trade.symbol = _symbolId;
    ev.trade.price = flox::Price::fromDouble(msg.price);
    ev.trade.quantity = flox::Quantity::fromDouble(msg.qty);
    ev.trade.isBuy = msg.side == "buy";
    ev.trade.exchangeTsNs = msg.timestamp;
    ev.recvNs = flox::nowNsMonotonic();

    // Emit via base class method
    emitTrade(ev);
  }

  void handleBookUpdate(const Message& msg) {
    // For book updates, create event directly (simplified)
    // In practice, you might use a pool for large events

    flox::BookUpdateEvent ev(/* pmr allocator */);
    ev.update.symbol = _symbolId;
    ev.update.type = flox::BookUpdateType::SNAPSHOT;

    for (const auto& bid : msg.bids) {
      ev.update.bids.push_back({
        flox::Price::fromDouble(bid.price),
        flox::Quantity::fromDouble(bid.qty)
      });
    }
    for (const auto& ask : msg.asks) {
      ev.update.asks.push_back({
        flox::Price::fromDouble(ask.price),
        flox::Quantity::fromDouble(ask.qty)
      });
    }

    ev.recvNs = flox::nowNsMonotonic();

    emitBookUpdate(ev);
  }

  std::string _symbol;
  flox::SymbolId _symbolId;
  std::atomic<bool> _running{false};
  std::thread _thread;
};
```

## 2. Wire Callbacks

Connect the connector to your buses:

```cpp
#include "flox/book/bus/trade_bus.h"
#include "flox/book/bus/book_update_bus.h"
#include "flox/util/memory/pool.h"

// Create buses
auto tradeBus = std::make_unique<TradeBus>();
auto bookBus = std::make_unique<BookUpdateBus>();

// Create pool for book events
pool::Pool<BookUpdateEvent, 128> bookPool;

// Create connector
auto connector = std::make_shared<MyExchangeConnector>("BTCUSD", symbolId);

// Wire callbacks
connector->setCallbacks(
  // Book update callback
  [&bookBus, &bookPool](const BookUpdateEvent& ev) {
    // Acquire from pool for variable-size events
    if (auto handle = bookPool.acquire()) {
      (*handle)->update = ev.update;
      (*handle)->recvNs = ev.recvNs;
      bookBus->publish(std::move(handle));
    }
  },
  // Trade callback
  [&tradeBus](const TradeEvent& ev) {
    tradeBus->publish(ev);
  }
);
```

## 3. Register with Engine

```cpp
std::vector<std::shared_ptr<IExchangeConnector>> connectors;
connectors.push_back(connector);

std::vector<std::unique_ptr<ISubsystem>> subsystems;
subsystems.push_back(std::move(tradeBus));
subsystems.push_back(std::move(bookBus));
// ... add strategies, etc.

EngineConfig config{};
Engine engine(config, std::move(subsystems), std::move(connectors));
engine.start();
```

## 4. Use ConnectorFactory (Optional)

For dynamic connector creation:

```cpp
#include "flox/connector/connector_factory.h"

// Register factory
ConnectorFactory::instance().registerConnector("myexchange",
  [registry](const std::string& symbol) {
    auto symbolId = registry->getSymbolId("myexchange", symbol);
    return std::make_shared<MyExchangeConnector>(symbol, *symbolId);
  }
);

// Create connector
auto conn = ConnectorFactory::instance().createConnector("myexchange", "BTCUSD");
```

## 5. Best Practices

### Thread Safety

- Connector runs its own thread(s)
- `emitTrade()` and `emitBookUpdate()` are thread-safe
- Callbacks may execute on connector thread

### Timestamps

Capture timestamps at the right points:
```cpp
void handleMessage(const RawMessage& raw) {
  MonoNanos recvNs = nowNsMonotonic();  // Capture immediately

  // Parse message...
  auto parsed = parse(raw);

  TradeEvent ev;
  ev.trade.exchangeTsNs = parsed.exchangeTimestamp;  // From exchange
  ev.recvNs = recvNs;                                 // When we received it
  ev.publishTsNs = nowNsMonotonic();                  // Right before emit

  emitTrade(ev);
}
```

### Error Handling

```cpp
void run() {
  while (_running) {
    try {
      if (!_connected) {
        connect();
      }
      auto msg = receiveMessage();
      handleMessage(msg);
    } catch (const ConnectionError& e) {
      FLOX_LOG("Connection lost: " << e.what());
      _connected = false;
      reconnect();
    }
  }
}
```

### Sequence Numbers

Track exchange sequence numbers for gap detection:

```cpp
void handleBookUpdate(const Message& msg) {
  if (_lastSeq > 0 && msg.seq != _lastSeq + 1) {
    FLOX_LOG("Gap detected: " << _lastSeq << " -> " << msg.seq);
    requestSnapshot();  // Request full book snapshot
  }
  _lastSeq = msg.seq;

  // ... create and emit event
}
```

### Pooled Book Events

For high-frequency book updates, use pooled events:

```cpp
class MyExchangeConnector : public IExchangeConnector
{
  pool::Pool<BookUpdateEvent, 64> _bookPool;

  void handleBookUpdate(const Message& msg) {
    auto evOpt = _bookPool.acquire();
    if (!evOpt) {
      FLOX_LOG("Pool exhausted, dropping book update");
      return;
    }

    auto& ev = *evOpt;
    // ... populate ev

    emitBookUpdate(*ev);
    // Handle is moved to bus, returns to pool when all consumers done
  }
};
```

## 6. Complete Example

```cpp
#include "flox/connector/abstract_exchange_connector.h"
#include "flox/book/events/trade_event.h"
#include "flox/book/events/book_update_event.h"
#include "flox/util/memory/pool.h"
#include "flox/util/base/time.h"
#include "flox/log/log.h"

#include <thread>
#include <atomic>

namespace myexchange
{

using namespace flox;

class MyConnector : public IExchangeConnector
{
public:
  MyConnector(SymbolId symbol, TradeBus& tradeBus, BookUpdateBus& bookBus)
    : _symbol(symbol)
    , _tradeBus(tradeBus)
    , _bookBus(bookBus)
  {}

  std::string exchangeId() const override { return "myexchange"; }

  void start() override {
    if (_running.exchange(true)) return;
    _thread = std::thread(&MyConnector::run, this);
  }

  void stop() override {
    if (!_running.exchange(false)) return;
    if (_thread.joinable()) _thread.join();
  }

private:
  void run() {
    FLOX_LOG("[myexchange] Connecting...");
    // ... connect to exchange

    while (_running) {
      // ... receive and process messages
    }

    FLOX_LOG("[myexchange] Disconnected");
  }

  void onTradeMessage(/* ... */) {
    TradeEvent ev;
    ev.trade.symbol = _symbol;
    ev.trade.price = Price::fromDouble(/* ... */);
    ev.trade.quantity = Quantity::fromDouble(/* ... */);
    ev.trade.isBuy = /* ... */;
    ev.trade.exchangeTsNs = /* ... */;
    ev.recvNs = nowNsMonotonic();

    _tradeBus.publish(ev);
  }

  void onBookMessage(/* ... */) {
    auto evOpt = _bookPool.acquire();
    if (!evOpt) return;

    auto& ev = *evOpt;
    ev->update.symbol = _symbol;
    ev->update.type = BookUpdateType::SNAPSHOT;
    // ... populate bids/asks

    ev->recvNs = nowNsMonotonic();
    _bookBus.publish(std::move(ev));
  }

  SymbolId _symbol;
  TradeBus& _tradeBus;
  BookUpdateBus& _bookBus;

  pool::Pool<BookUpdateEvent, 64> _bookPool;

  std::atomic<bool> _running{false};
  std::thread _thread;
};

}  // namespace myexchange
```

## See Also

- [Architecture Overview](../explanation/architecture.md) — How connectors fit
- [First Strategy Tutorial](../tutorials/first-strategy.md) — Consuming events
