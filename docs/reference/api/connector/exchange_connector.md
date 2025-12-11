# ExchangeConnector

`ExchangeConnector` is the abstract interface for real-time market data adapters. It provides lifecycle control and typed callback delivery for `BookUpdateEvent` and `TradeEvent`.

```cpp
class ExchangeConnector {
public:
  using BookUpdateCallback = std::move_only_function<void(const BookUpdateEvent&)>;
  using TradeCallback      = std::move_only_function<void(const TradeEvent&)>;

  virtual ~ExchangeConnector() = default;

  virtual void start() = 0;
  virtual void stop() = 0;

  virtual std::string exchangeId() const = 0;

  virtual void setCallbacks(BookUpdateCallback onBookUpdate, TradeCallback onTrade);

protected:
  void emitBookUpdate(const BookUpdateEvent& bu);
  void emitTrade(const TradeEvent& t);
};
```

## Purpose

* Abstract base for all exchange-specific connectors (e.g. Bybit, Mock, Replay), handling event emission and lifecycle.

## Responsibilities

| Aspect        | Details                                                                 |
| ------------- | ----------------------------------------------------------------------- |
| Lifecycle     | `start()` and `stop()` control external connectivity and data flow.     |
| Identity      | `exchangeId()` provides a stable identifier for the connector instance. |
| Callbacks     | `setCallbacks()` binds downstream handlers for book and trade events.   |
| Event Routing | `emitBookUpdate()` and `emitTrade()` dispatch data to subscribers.      |

## Notes

* Callbacks use `std::move_only_function` to avoid `std::function` overhead and enable capturing closures with ownership.
* Implementations must call `emit*()` manually from internal processing (e.g. websocket handler).
* The class is intentionally non-copyable and non-thread-safe — connectors are expected to run in isolated threads.
