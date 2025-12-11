# CandleEvent

`CandleEvent` represents a finalized OHLCV candle for a specific instrument and time interval, emitted by `CandleAggregator` and delivered via `CandleBus`.

```cpp
struct CandleEvent {
    using Listener = IMarketDataSubscriber;

    SymbolId       symbol{};                             // instrument identifier
    InstrumentType instrument = InstrumentType::Spot;    // Spot | Future | Option
    Candle         candle{};                             // aggregated OHLCV data
    uint64_t       tickSequence = 0;                     // global sequencing
};
```

## Purpose

* Encapsulate a single time-aggregated candle with instrument metadata for downstream processing and filtering.

## Responsibilities

| Field / Aspect   | Description                                                                  |
| ---------------- | ---------------------------------------------------------------------------- |
| **symbol**       | Unique `SymbolId` of the instrument.                                         |
| **instrument**   | Instrument class (`Spot`, `Future`, or `Option`) for fast filtering.         |
| **candle**       | Aggregated OHLCV data (`open`, `high`, `low`, `close`, `volume`, timeframe). |
| **tickSequence** | Monotonic sequence number for deterministic ordering (sync mode).            |
| **Listener**     | Defines `IMarketDataSubscriber` as the subscriber interface for `CandleBus`. |

## Notes

* `instrument` removes the need for a registry lookup in hot paths.
* `tickSequence` guarantees strict ordering when `CandleBus` runs in synchronous mode.
* Used identically in live trading and back-testing environments.
