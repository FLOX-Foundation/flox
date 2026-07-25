# IMarketDataSubscriber

`IMarketDataSubscriber` is a unified interface for components that consume real-time market data events. It supports optional handling of order book updates, trades, and bars.

```cpp
enum class MarketDataErrorCode
{
  UNKNOWN = 0,
  CONNECTION_LOST,
  CONNECTION_TIMEOUT,
  INVALID_MESSAGE,
  RATE_LIMITED,
  SUBSCRIPTION_FAILED,
  STALE_DATA
};

struct MarketDataError
{
  MarketDataErrorCode code{MarketDataErrorCode::UNKNOWN};
  SymbolId symbol{0};
  std::string message;
  int64_t timestampNs{0};
};

class IMarketDataSubscriber : public ISubscriber
{
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

## Methods

| Method       | Description                                      |
| ------------ | ------------------------------------------------ |
| onBookUpdate | Receives `BookUpdateEvent` from `BookUpdateBus`. |
| onTrade      | Receives `TradeEvent` from `TradeBus`.           |
| onBar        | Receives `BarEvent` from `BarBus`.               |
| onMarketDataError | Receives a `MarketDataError` from the connector: connection loss or timeout, malformed message, rate limiting, failed subscription, stale data. Default no-op, so a subscriber that ignores feed health keeps compiling. |

### MarketDataError

| Field | Type | Description |
|-------|------|-------------|
| `code` | `MarketDataErrorCode` | What went wrong |
| `symbol` | `SymbolId` | Affected symbol, `0` when the error is connection-wide |
| `message` | `std::string` | Human-readable detail |
| `timestampNs` | `int64_t` | When the error was observed (ns) |

## Notes

* Default implementations are no-ops — subscribers override only what they care about.
* Used with `EventBus<T>` which delivers events via `EventDispatcher`.
* Inherits from `ISubscriber`, which provides `id()` for routing.
