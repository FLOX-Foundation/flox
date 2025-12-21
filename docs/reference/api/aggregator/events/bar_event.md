# BarEvent

`BarEvent` is the event wrapper for delivering completed bars through the event bus system.

```cpp
struct BarEvent {
  using Listener = IMarketDataSubscriber;

  SymbolId symbol{};
  InstrumentType instrument = InstrumentType::Spot;
  BarType barType{};
  uint64_t barTypeParam{};  // interval in nanoseconds, tick count, volume threshold, etc.
  Bar bar{};

  uint64_t tickSequence = 0;  // internal, set by bus
};
```

## Fields

| Field | Type | Description |
|-------|------|-------------|
| `symbol` | `SymbolId` | Symbol identifier for this bar. |
| `instrument` | `InstrumentType` | Instrument type (Spot, Perpetual, etc.). |
| `barType` | `BarType` | Type of bar (Time, Tick, Volume, etc.). |
| `barTypeParam` | `uint64_t` | Bar-type-specific parameter (see below). |
| `bar` | `Bar` | The actual OHLCV bar data. |
| `tickSequence` | `uint64_t` | Internal sequence number, set by bus. |

## barTypeParam Interpretation

The meaning of `barTypeParam` depends on `barType`:

| BarType | barTypeParam Meaning |
|---------|---------------------|
| `Time` | Interval in nanoseconds |
| `Tick` | Number of trades per bar |
| `Volume` | Volume threshold (raw) |
| `Renko` | Brick size (raw price) |
| `Range` | Range threshold (raw price) |
| `HeikinAshi` | Interval in nanoseconds |

## Example Usage

```cpp
void onBar(const BarEvent& ev) override {
  if (ev.barType == BarType::Time) {
    auto intervalNs = ev.barTypeParam;
    auto intervalSec = intervalNs / 1'000'000'000;
    // Handle time bar
  }

  // Access bar data
  Price close = ev.bar.close;
  Volume delta = ev.bar.buyVolume;
}
```

## See Also

* [Bar](../bar.md) — Bar data structure
* [BarBus](../bus/bar_bus.md) — Event bus for bar distribution
* [IMarketDataSubscriber](../../engine/abstract_market_data_subscriber.md) — Listener interface
