# Candle

`Candle` represents a single OHLCV time-bar aggregated from trades within a fixed interval.

```cpp
struct Candle {
  Price open;
  Price high;
  Price low;
  Price close;
  Volume volume;
  TimePoint startTime;
  TimePoint endTime;

  Candle() = default;
  Candle(TimePoint ts, Price price, Volume qty);
};
```

## Purpose

* Store the full market state (OHLCV) over a defined time window for downstream analytics or strategy input.

## Responsibilities

| Field     | Description                                               |
| --------- | --------------------------------------------------------- |
| open      | Price of the first trade in the interval.                 |
| high/low  | Highest and lowest traded price during the interval.      |
| close     | Price of the last trade in the interval.                  |
| volume    | Total traded volume across all ticks in the window.       |
| startTime | Timestamp of the first trade in the interval.             |
| endTime   | Timestamp of the last trade in the interval (may evolve). |

## Notes

* Constructed with initial price/volume; high/low/close evolve with subsequent trades.
* Timestamps are `steady_clock`-based to support simulation and deterministic replay.
* Used exclusively by `CandleAggregator` and delivered via `CandleEvent`.
