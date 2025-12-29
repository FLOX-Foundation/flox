# IMarketDataSubscriber

`IMarketDataSubscriber` is a unified interface for components that consume real-time market data events. It supports optional handling of order book updates, trades, and bars.

```cpp
class IMarketDataSubscriber : public ISubscriber
{
public:
  virtual ~IMarketDataSubscriber() = default;

  virtual void onBookUpdate(const BookUpdateEvent& ev) {}
  virtual void onTrade(const TradeEvent& ev) {}
  virtual void onBar(const BarEvent& ev) {}
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

## Notes

* Default implementations are no-ops — subscribers override only what they care about.
* Used with `EventBus<T>` which delivers events via `EventDispatcher`.
* Inherits from `ISubscriber`, which provides `id()` for routing.
