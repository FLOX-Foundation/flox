# IMarketDataSubscriber

`IMarketDataSubscriber` is a unified interface for components that consume real-time market data events. It supports optional handling of order book updates, trades, and candles.

```cpp
class IMarketDataSubscriber : public ISubscriber {
public:
  virtual ~IMarketDataSubscriber() = default;

  virtual void onBookUpdate(const BookUpdateEvent& ev) {}
  virtual void onTrade(const TradeEvent& ev) {}
  virtual void onCandle(const CandleEvent& ev) {}
};
```

## Purpose

* Serve as a polymorphic listener for all market-facing event types across the system.

## Responsibilities

| Method       | Description                                      |
| ------------ | ------------------------------------------------ |
| onBookUpdate | Receives `BookUpdateEvent` from `BookUpdateBus`. |
| onTrade      | Receives `TradeEvent` from `TradeBus`.           |
| onCandle     | Receives `CandleEvent` from `CandleBus`.         |

## Notes

* Default implementations are no-ops — subscribers override only what they care about.
* Always used in conjunction with `EventBus<T>` and its `Policy` (sync or async).
* Inherits from `ISubscriber`, which provides `id()` and `mode()` for routing.
