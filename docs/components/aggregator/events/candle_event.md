# CandleEvent

`CandleEvent` represents a finalized OHLCV candle for a specific symbol and time interval, emitted by `CandleAggregator` and delivered via `CandleBus`.

```cpp
struct CandleEvent {
  using Listener = IMarketDataSubscriber;

  SymbolId symbol{};
  Candle   candle{};
  uint64_t tickSequence = 0;
};
```

## Purpose

* Encapsulate a single time-aggregated candle (`Candle`) with metadata for delivery.

## Responsibilities

| Aspect       | Details                                                                |
| ------------ | ---------------------------------------------------------------------- |
| Symbol       | `symbol` identifies the market instrument.                             |
| Payload      | `candle` contains OHLCV data for the interval.                         |
| Sequencing   | `tickSequence` ensures deterministic delivery in sync-mode processing. |
| Subscription | Defines `IMarketDataSubscriber` as listener type for `CandleBus`.      |

## Notes

* `tickSequence` enables strict event ordering under `SyncPolicy`.
* Used in both live and simulated environments.
* `CandleEvent` is a plain data object; no logic or ownership semantics.
