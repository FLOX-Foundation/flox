# Bar

`Bar` is the core OHLCV data structure representing aggregated price action over a defined interval.

```cpp
enum class BarType : uint8_t {
  Time,       // Time-based intervals (e.g., 1 minute, 1 hour)
  Tick,       // Fixed number of trades
  Volume,     // Fixed notional volume
  Renko,      // Fixed price movement (bricks)
  Range,      // Fixed high-low range
  HeikinAshi, // Smoothed time-based bars
  BpsRange    // Fixed high-low range in basis points of the bar open
};

enum class BarCloseReason : uint8_t {
  Threshold = 0,  // Normal close: interval/count/volume reached
  Gap = 1,        // A price jump too wide to walk brick by brick -- see RenkoBarPolicy::kMaxGapBricks
  Forced = 2,     // Forced close: stop() called or manual flush
  Warmup = 3      // Set by the caller, not the engine -- see BarMatrix::warmup()
};

struct Bar {
  Price open{};
  Price high{};
  Price low{};
  Price close{};
  Volume volume{};
  Volume buyVolume{};     // For delta calculation
  Quantity tradeCount{};  // Number of trades in this bar
  TimePoint startTime{};
  TimePoint endTime{};
  BarCloseReason reason{BarCloseReason::Threshold};
};
```

## Fields

| Field | Type | Description |
|-------|------|-------------|
| `open` | `Price` | Opening price of the bar. |
| `high` | `Price` | Highest price during the bar. |
| `low` | `Price` | Lowest price during the bar. |
| `close` | `Price` | Closing price of the bar. |
| `volume` | `Volume` | Total notional volume (price × quantity). |
| `buyVolume` | `Volume` | Buy-side notional volume for delta calculation. |
| `tradeCount` | `Quantity` | Number of trades aggregated into this bar. |
| `startTime` | `TimePoint` | Bar open timestamp. |
| `endTime` | `TimePoint` | Bar close timestamp. |
| `reason` | `BarCloseReason` | Why this bar was closed. `BarAggregator` and `MultiTimeframeAggregator` set `Threshold` for a normal close and `Forced` for a `stop()` flush. `Gap` marks the one bar that absorbs the remainder of a Renko price jump too wide to walk brick by brick -- it is taller than one brick and closes on the far boundary; see [Bar types](../../../explanation/bar-types.md) and `RenkoBarPolicy::kMaxGapBricks`. `Warmup` is never assigned by the engine itself -- set it on the `Bar` objects you build from historical data before calling `BarMatrix::warmup()`, and it carries through unchanged. |

## Delta Calculation

The buy/sell delta can be calculated as:

```cpp
Volume sellVolume = bar.volume - bar.buyVolume;
int64_t delta = bar.buyVolume.raw() - sellVolume.raw();
```

## BarType Values

| Type | Closes When | Use Case |
|------|-------------|----------|
| `Time` | Fixed time interval elapsed | Traditional charting |
| `Tick` | N trades occurred | HFT, activity-based |
| `Volume` | Notional volume threshold reached | Flow analysis |
| `Renko` | Price moved by brick size | Trend following |
| `Range` | High-low exceeded threshold | Volatility analysis |
| `HeikinAshi` | Time interval (smoothed OHLC) | Trend clarity |
| `BpsRange` | `(high - low) / open >= bps threshold` | Volatility analysis, scale-free across price levels |

## See Also

* [BarEvent](events/bar_event.md) — Event wrapper for bar delivery
* [BarBus](bus/bar_bus.md) — Event bus for bar distribution
* [Bar Types Guide](../../../explanation/bar-types.md) — Detailed bar type comparison
