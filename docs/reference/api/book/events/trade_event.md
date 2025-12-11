# TradeEvent

`TradeEvent` represents a single trade tick — a filled transaction between counterparties — and is broadcast across the system for aggregation, analytics, and strategy input.

```cpp
struct TradeEvent {
  using Listener = IMarketDataSubscriber;

  Trade trade{};
  uint64_t tickSequence = 0;
};
```

## Purpose

* Encapsulate trade prints received from exchanges for delivery via `TradeBus`.

## Responsibilities

| Aspect       | Details                                                                |
| ------------ | ---------------------------------------------------------------------- |
| Payload      | `trade` holds symbol, price, quantity, timestamp, and taker direction. |
| Sequencing   | `tickSequence` guarantees strict event order for replay and backtests. |
| Subscription | Targets `IMarketDataSubscriber` interface for generic event delivery.  |

## Notes

* Used by `CandleAggregator`, PnL trackers, and all signal generation components.
* Designed for ultra-low-latency delivery; no heap allocation involved.
* Stateless container — no logic beyond encapsulation.
