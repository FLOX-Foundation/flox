# How to Create a Custom Bar Policy

This guide shows how to create your own bar closing policy for the BarAggregator.

## When to Use

Create a custom policy when built-in types (Time, Tick, Volume, Renko, Range) don't fit your needs:

- Dollar bars (close after fixed dollar volume)
- Volatility bars (close based on ATR)
- Session bars (close at specific times)
- Hybrid bars (combine multiple conditions)

## The BarPolicy Concept

Your policy must satisfy this interface:

```cpp
template <typename T>
concept BarPolicy = requires(T& p, const TradeEvent& t, Bar& b) {
  { p.shouldClose(t, b) } noexcept -> std::same_as<bool>;
  { p.update(t, b) } noexcept -> std::same_as<void>;
  { p.initBar(t, b) } noexcept -> std::same_as<void>;
  { T::kBarType } -> std::convertible_to<BarType>;
};
```

## Example: Dollar Bar Policy

Closes when notional volume reaches a threshold:

```cpp
#include "flox/aggregator/aggregation_policy.h"
#include "flox/aggregator/bar.h"
#include "flox/book/events/trade_event.h"

namespace flox
{

class DollarBarPolicy
{
 public:
  static constexpr BarType kBarType = BarType::Volume;

  explicit DollarBarPolicy(Volume threshold) : _threshold(threshold) {}

  static DollarBarPolicy fromDouble(double dollars)
  {
    return DollarBarPolicy(Volume::fromDouble(dollars));
  }

  bool shouldClose(const TradeEvent& trade, const Bar& bar) noexcept
  {
    return bar.volume.raw() >= _threshold.raw();
  }

  void update(const TradeEvent& trade, Bar& bar) noexcept
  {
    updateOHLCV(trade, bar);  // Use built-in helper
  }

  void initBar(const TradeEvent& trade, Bar& bar) noexcept
  {
    initializeBar(trade, bar);  // Use built-in helper
  }

 private:
  Volume _threshold;
};

}  // namespace flox
```

### Usage

```cpp
BarAggregator<DollarBarPolicy> aggregator(
  DollarBarPolicy::fromDouble(1000000.0),  // $1M bars
  &bus
);
```

## Example: Session Bar Policy

Closes at specific session boundaries:

```cpp
class SessionBarPolicy
{
 public:
  static constexpr BarType kBarType = BarType::Time;

  explicit SessionBarPolicy(std::vector<uint64_t> closeTimesNs)
      : _closeTimes(std::move(closeTimesNs))
  {
    std::sort(_closeTimes.begin(), _closeTimes.end());
  }

  bool shouldClose(const TradeEvent& trade, const Bar& bar) noexcept
  {
    uint64_t tradeNs = trade.trade.exchangeTsNs;
    uint64_t barStartNs = bar.startTime.time_since_epoch().count();

    for (uint64_t closeTime : _closeTimes)
    {
      if (tradeNs >= closeTime && barStartNs < closeTime)
      {
        return true;
      }
    }
    return false;
  }

  void update(const TradeEvent& trade, Bar& bar) noexcept
  {
    updateOHLCV(trade, bar);
  }

  void initBar(const TradeEvent& trade, Bar& bar) noexcept
  {
    initializeBar(trade, bar);
  }

 private:
  std::vector<uint64_t> _closeTimes;
};
```

## Example: Volatility Bar Policy

Closes when price range exceeds N times recent average:

```cpp
class VolatilityBarPolicy
{
 public:
  static constexpr BarType kBarType = BarType::Range;

  explicit VolatilityBarPolicy(double multiplier, Price baseRange)
      : _multiplier(multiplier), _threshold(baseRange)
  {
  }

  bool shouldClose(const TradeEvent& trade, const Bar& bar) noexcept
  {
    Price range = Price::fromRaw(bar.high.raw() - bar.low.raw());
    Price dynamicThreshold = Price::fromRaw(
      static_cast<int64_t>(_threshold.raw() * _multiplier)
    );
    return range.raw() >= dynamicThreshold.raw();
  }

  void update(const TradeEvent& trade, Bar& bar) noexcept
  {
    updateOHLCV(trade, bar);

    // Update rolling average for next bar
    Price range = Price::fromRaw(bar.high.raw() - bar.low.raw());
    _threshold = Price::fromRaw(
      (_threshold.raw() * 9 + range.raw()) / 10  // EMA-like
    );
  }

  void initBar(const TradeEvent& trade, Bar& bar) noexcept
  {
    initializeBar(trade, bar);
  }

 private:
  double _multiplier;
  Price _threshold;
};
```

## Helper Functions

Use these built-in helpers in your policy:

### updateOHLCV

Updates high, low, close, volume, tradeCount, buyVolume, endTime:

```cpp
void update(const TradeEvent& trade, Bar& bar) noexcept
{
  updateOHLCV(trade, bar);
  // Add custom logic here
}
```

### initializeBar

Initializes a new bar from the first trade:

```cpp
void initBar(const TradeEvent& trade, Bar& bar) noexcept
{
  initializeBar(trade, bar);
  // Add custom initialization here
}
```

## Adding Custom State

Store additional state in your policy:

```cpp
class ImbalanceBarPolicy
{
 public:
  static constexpr BarType kBarType = BarType::Volume;

  explicit ImbalanceBarPolicy(double imbalanceThreshold)
      : _threshold(imbalanceThreshold)
  {
  }

  bool shouldClose(const TradeEvent& trade, const Bar& bar) noexcept
  {
    if (bar.volume.raw() == 0) return false;

    double buyRatio = static_cast<double>(bar.buyVolume.raw()) /
                      static_cast<double>(bar.volume.raw());

    // Close when buy/sell imbalance exceeds threshold
    return buyRatio > _threshold || buyRatio < (1.0 - _threshold);
  }

  void update(const TradeEvent& trade, Bar& bar) noexcept
  {
    updateOHLCV(trade, bar);
  }

  void initBar(const TradeEvent& trade, Bar& bar) noexcept
  {
    initializeBar(trade, bar);
  }

 private:
  double _threshold;  // e.g., 0.7 = 70% imbalance
};
```

## Using with MultiTimeframeAggregator

Custom policies work with `MultiTimeframeAggregator` via the variant-based interface:

```cpp
// Currently supports Time, Tick, Volume via addXxxInterval()
// For custom policies, use separate BarAggregator instances:

BarAggregator<DollarBarPolicy> dollarAgg(
  DollarBarPolicy::fromDouble(1000000.0), &bus
);

BarAggregator<ImbalanceBarPolicy> imbalanceAgg(
  ImbalanceBarPolicy(0.7), &bus
);

// Feed trades to both
void onTrade(const TradeEvent& trade) {
  dollarAgg.onTrade(trade);
  imbalanceAgg.onTrade(trade);
}
```

## Testing Your Policy

```cpp
TEST(MyPolicyTest, ClosesAtThreshold)
{
  BarBus bus;
  bus.enableDrainOnStop();

  std::vector<Bar> bars;
  // ... set up test subscriber ...

  BarAggregator<MyPolicy> agg(MyPolicy(...), &bus);
  bus.subscribe(&subscriber);
  bus.start();
  agg.start();

  // Feed test trades
  agg.onTrade(makeTrade(...));

  agg.stop();
  bus.stop();

  EXPECT_EQ(bars.size(), expectedCount);
}
```

## See Also

- [Bar Aggregator Reference](../reference/api/aggregator/bar_aggregator.md)
- [Bar Types Explained](../explanation/bar-types.md)
