# CandleAggregator

`CandleAggregator` transforms incoming `TradeEvent`s into time-aligned OHLCV candles and broadcasts them via `CandleBus`.

```cpp
class CandleAggregator : public ISubsystem, public IMarketDataSubscriber {
public:
  CandleAggregator(std::chrono::seconds interval, CandleBus* bus);
  void start() override;
  void stop() override;

  SubscriberId id() const override;
  SubscriberMode mode() const override;

  void onTrade(const TradeEvent& trade) override;

private:
  struct PartialCandle {
    Candle candle;
    bool initialized = false;
  };

  std::chrono::seconds _interval;
  CandleBus* _bus;
  std::vector<std::optional<PartialCandle>> _candles;

  TimePoint alignToInterval(TimePoint tp);
};
```

## Purpose

* Buffer and roll trades into interval-based candles, suitable for downstream analytics or strategy inputs.

## Responsibilities

| Aspect        | Details                                                        |
| ------------- | -------------------------------------------------------------- |
| Interval      | Candle size defined by `_interval`, validated at construction. |
| Event input   | Consumes only `TradeEvent`; no handling for books or candles.  |
| Event output  | Emits `CandleEvent` to all subscribers via `CandleBus`.        |
| Lifecycle     | Hooks into engine via `ISubsystem::start()` and `stop()`.      |
| Subscriber ID | Uses object pointer as a unique `SubscriberId`.                |
| Mode          | Operates in `PUSH` mode for direct event delivery.             |

## Internal Behavior

1. **Time Slot Alignment**
   Trade timestamps are aligned using `alignToInterval()` to find the start of the containing interval.

2. **Per-Symbol Buffering**
   `_candles` is a `std::vector<std::optional<PartialCandle>>`, indexed by `SymbolId`, pre-sized for all known symbols.

3. **Candle Rollover**
   If a trade belongs to a new interval, the previous candle is finalized and sent; a new `PartialCandle` is started.

4. **No Hot Allocations**
   Once the vector is initialized, the hot path is allocation-free; avoids `unordered_map` lookup cost.

## Notes

* Designed for maximum cache-friendliness and fan-out throughput.
* Ignores out-of-order or backdated trades — assumes input stream is clean and ordered.
* Fully decoupled via `CandleBus`; downstream consumers remain unaware of source logic.
