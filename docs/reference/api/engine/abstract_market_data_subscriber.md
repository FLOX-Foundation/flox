# IMarketDataSubscriber

`IMarketDataSubscriber` is a unified interface for components that consume real-time market data events. It supports optional handling of order book updates, trades, bars, and error notifications.

```cpp
enum class MarketDataErrorCode {
  UNKNOWN = 0,
  CONNECTION_LOST,
  CONNECTION_TIMEOUT,
  INVALID_MESSAGE,
  RATE_LIMITED,
  SUBSCRIPTION_FAILED,
  STALE_DATA
};

struct MarketDataError {
  MarketDataErrorCode code{MarketDataErrorCode::UNKNOWN};
  SymbolId symbol{0};
  std::string message;
  int64_t timestampNs{0};
};

class IMarketDataSubscriber : public ISubscriber {
public:
  virtual ~IMarketDataSubscriber() = default;

  virtual void onBookUpdate(const BookUpdateEvent& ev) {}
  virtual void onTrade(const TradeEvent& ev) {}
  virtual void onBar(const BarEvent& ev) {}
  virtual void onMarketDataError(const MarketDataError& error) {}
};
```

## Purpose

* Serve as a polymorphic listener for all market-facing event types across the system.
* Provide error notification mechanism for market data issues.

## Responsibilities

| Method | Description |
|--------|-------------|
| `onBookUpdate` | Receives `BookUpdateEvent` from `BookUpdateBus`. |
| `onTrade` | Receives `TradeEvent` from `TradeBus`. |
| `onBar` | Receives `BarEvent` from `BarBus`. |
| `onMarketDataError` | Receives error notifications for market data issues. |

## MarketDataErrorCode Values

| Code | Description |
|------|-------------|
| `UNKNOWN` | Unspecified error. |
| `CONNECTION_LOST` | WebSocket or network connection dropped. |
| `CONNECTION_TIMEOUT` | Connection attempt timed out. |
| `INVALID_MESSAGE` | Received malformed or unparseable message. |
| `RATE_LIMITED` | Exchange rate limit exceeded. |
| `SUBSCRIPTION_FAILED` | Failed to subscribe to symbol/stream. |
| `STALE_DATA` | Data hasn't updated within expected interval. |

## MarketDataError Fields

| Field | Type | Description |
|-------|------|-------------|
| `code` | `MarketDataErrorCode` | Type of error that occurred. |
| `symbol` | `SymbolId` | Affected symbol (0 if global error). |
| `message` | `std::string` | Human-readable error description. |
| `timestampNs` | `int64_t` | When the error occurred (nanoseconds). |

## Example Usage

```cpp
class MyStrategy : public IMarketDataSubscriber {
public:
  void onBookUpdate(const BookUpdateEvent& ev) override {
    // Process order book update
    updateBook(ev);
  }

  void onMarketDataError(const MarketDataError& error) override {
    if (error.code == MarketDataErrorCode::CONNECTION_LOST) {
      // Handle reconnection logic
      log("Connection lost for symbol " + std::to_string(error.symbol));
      pauseTrading();
    } else if (error.code == MarketDataErrorCode::STALE_DATA) {
      // Data quality issue
      markSymbolStale(error.symbol);
    }
  }
};
```

## Notes

* Default implementations are no-ops — subscribers override only what they care about.
* Always used in conjunction with `EventBus<T>` and its `Policy` (sync or async).
* Inherits from `ISubscriber`, which provides `id()` and `mode()` for routing.
* Error handling is optional but recommended for production systems.

## See Also

* [ISubscriber](abstract_subscriber.md) — Base subscriber interface
* [BookUpdateEvent](../../book/events/book_update_event.md) — Order book update event
* [TradeEvent](../../book/events/trade_event.md) — Trade event
* [BarEvent](../../aggregator/events/bar_event.md) — Bar event
