# TradeEvent

`TradeEvent` represents a single trade tick — a filled transaction between counterparties — and is broadcast across the system for aggregation, analytics, and strategy input.

```cpp
struct TradeEvent
{
  using Listener = IMarketDataSubscriber;

  Trade trade{};

  int64_t seq = 0;        // Exchange sequence number
  uint64_t trade_id = 0;  // Exchange-assigned trade ID

  uint64_t tickSequence = 0;  // Internal, set by bus

  MonoNanos recvNs{0};         // Local receive time (monotonic)
  MonoNanos publishTsNs{0};    // Bus publish time (monotonic)
  UnixNanos exchangeMsgTsNs{0}; // Exchange message timestamp

  ExchangeId sourceExchange{InvalidExchangeId};  // Source exchange for CEX coordination
};
```

## Purpose

* Encapsulate trade prints received from exchanges for delivery via `TradeBus`.

## Fields

| Field             | Description                                                      |
|-------------------|------------------------------------------------------------------|
| `trade`           | `Trade` payload with symbol, price, quantity, side, timestamp.   |
| `seq`             | Exchange-assigned sequence number.                               |
| `trade_id`        | Exchange-assigned trade ID.                                      |
| `tickSequence`    | Internal bus sequence for ordered delivery.                      |
| `recvNs`          | Local receive timestamp (monotonic nanoseconds).                 |
| `publishTsNs`     | Bus publish timestamp (monotonic nanoseconds).                   |
| `exchangeMsgTsNs` | Exchange message timestamp (Unix nanoseconds).                   |
| `sourceExchange`  | Source exchange ID for CEX coordination.                         |

## Responsibilities

| Aspect       | Details                                                                |
| ------------ | ---------------------------------------------------------------------- |
| Payload      | `trade` holds symbol, price, quantity, timestamp, and taker direction. |
| Sequencing   | `tickSequence` guarantees strict event order for replay and backtests. |
| Latency      | `recvNs` / `publishTsNs` / `exchangeMsgTsNs` enable latency analysis.  |
| Subscription | Targets `IMarketDataSubscriber` interface for generic event delivery.  |

## Notes

* Used by `BarAggregator`, PnL trackers, and all signal generation components.
* Designed for ultra-low-latency delivery; no heap allocation involved.
* Stateless container — no logic beyond encapsulation.
* `sourceExchange` enables cross-exchange trade aggregation in CEX mode.
