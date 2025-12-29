# How to Use Volume Profile

This guide shows practical applications of VolumeProfile for trading.

## Basic Setup

```cpp
#include "flox/aggregator/custom/volume_profile.h"

VolumeProfile<256> profile;
profile.setTickSize(Price::fromDouble(0.50));  // Aggregate to $0.50 levels
```

## Building a Session Profile

Reset at session start, build throughout the day:

```cpp
class SessionProfileStrategy : public IMarketDataSubscriber
{
 public:
  void onSessionStart()
  {
    _profile.clear();
  }

  void onTrade(const TradeEvent& ev) override
  {
    _profile.addTrade(ev);
  }

 private:
  VolumeProfile<256> _profile;
};
```

## Finding Key Levels

### Point of Control (POC)

The price with highest volume - acts as a magnet:

```cpp
Price poc = profile.poc();

// Trade toward POC
if (currentPrice < poc) {
  // Expect price to rise toward POC
} else if (currentPrice > poc) {
  // Expect price to fall toward POC
}
```

### Value Area

Where 70% of volume traded - represents "fair value":

```cpp
Price vaLow = profile.valueAreaLow();
Price vaHigh = profile.valueAreaHigh();

// Fade extremes
if (currentPrice < vaLow) {
  // Below value = potential long
} else if (currentPrice > vaHigh) {
  // Above value = potential short
}
```

## Trading Strategies

### Mean Reversion to POC

```cpp
void onTrade(const TradeEvent& ev) override
{
  _profile.addTrade(ev);

  Price poc = _profile.poc();
  Price price = ev.trade.price;

  // Entry: 0.5% away from POC
  Price distance = Price::fromRaw(std::abs(price.raw() - poc.raw()));
  Price threshold = Price::fromRaw(poc.raw() / 200);  // 0.5%

  if (distance > threshold) {
    if (price < poc) {
      // BUY - expect reversion to POC
      std::cout << "BUY @ " << price.toDouble()
                << " target POC @ " << poc.toDouble() << std::endl;
    } else {
      // SELL - expect reversion to POC
      std::cout << "SELL @ " << price.toDouble()
                << " target POC @ " << poc.toDouble() << std::endl;
    }
  }
}
```

### Value Area Breakout

```cpp
void onBar(const BarEvent& ev) override
{
  Price vaLow = _profile.valueAreaLow();
  Price vaHigh = _profile.valueAreaHigh();
  Price close = ev.bar.close;

  // Breakout above value area
  if (_prevClose <= vaHigh && close > vaHigh) {
    std::cout << "BREAKOUT LONG @ " << close.toDouble() << std::endl;
  }

  // Breakdown below value area
  if (_prevClose >= vaLow && close < vaLow) {
    std::cout << "BREAKDOWN SHORT @ " << close.toDouble() << std::endl;
  }

  _prevClose = close;
}
```

### Delta Confirmation

Use delta to confirm direction:

```cpp
void analyzeProfile()
{
  Volume delta = _profile.totalDelta();
  Price poc = _profile.poc();

  if (delta.raw() > 0) {
    // Net buying - bullish bias
    std::cout << "Bullish bias, buy pullbacks to POC @ "
              << poc.toDouble() << std::endl;
  } else {
    // Net selling - bearish bias
    std::cout << "Bearish bias, sell rallies to POC @ "
              << poc.toDouble() << std::endl;
  }
}
```

## Multi-Session Analysis

Combine profiles from multiple days:

```cpp
class CompositeProfile
{
 public:
  void addSession(const VolumeProfile<256>& session)
  {
    // Merge session POC into composite
    Price poc = session.poc();
    _pocLevels.push_back(poc);

    // Track recurring high-volume levels
    for (size_t i = 0; i < session.numLevels(); ++i) {
      const auto* lvl = session.level(i);
      if (lvl && lvl->volume.raw() > _minVolume) {
        _significantLevels[lvl->price.raw()]++;
      }
    }
  }

  std::vector<Price> getKeyLevels() const
  {
    std::vector<Price> result;
    for (const auto& [priceRaw, count] : _significantLevels) {
      if (count >= 3) {  // Appeared in 3+ sessions
        result.push_back(Price::fromRaw(priceRaw));
      }
    }
    return result;
  }

 private:
  std::vector<Price> _pocLevels;
  std::map<int64_t, int> _significantLevels;
  int64_t _minVolume = Volume::fromDouble(100000).raw();
};
```

## Volume at Price Queries

Check volume at specific levels:

```cpp
// Check if current price is at high volume node
Volume volAtPrice = profile.volumeAt(currentPrice);
Volume avgVol = Volume::fromRaw(profile.totalVolume().raw() / profile.numLevels());

if (volAtPrice.raw() > avgVol.raw() * 2) {
  // High Volume Node (HVN) - expect consolidation
} else if (volAtPrice.raw() < avgVol.raw() / 2) {
  // Low Volume Node (LVN) - expect fast move through
}
```

## Combining with Other Tools

### With Footprint

```cpp
VolumeProfile<256> profile;
FootprintBar<64> footprint;

void onTrade(const TradeEvent& ev) override
{
  profile.addTrade(ev);
  footprint.addTrade(ev);
}

void analyze()
{
  Price poc = profile.poc();

  // Check order flow at POC
  const auto* lvl = footprint.levelAt(poc);
  if (lvl) {
    if (lvl->imbalanceRatio() > 0.7) {
      // Strong buying at POC - support confirmed
    } else if (lvl->imbalanceRatio() < 0.3) {
      // Strong selling at POC - resistance forming
    }
  }
}
```

### With Bar Data

```cpp
void onBar(const BarEvent& ev) override
{
  Price poc = _profile.poc();
  Price vaLow = _profile.valueAreaLow();
  Price vaHigh = _profile.valueAreaHigh();

  // Bar closes at POC
  if (std::abs(ev.bar.close.raw() - poc.raw()) < _tickSize.raw()) {
    // Consolidation expected
  }

  // Bar closes outside value
  if (ev.bar.close < vaLow || ev.bar.close > vaHigh) {
    // Trend move or potential reversal
  }
}
```

## Performance Tips

1. **Tick size**: Larger tick = fewer levels = faster lookups
2. **MaxLevels**: Size appropriately (128-256 typical for crypto)
3. **Clear regularly**: Reset at session boundaries to avoid stale data

```cpp
// Appropriate sizing
VolumeProfile<128> cryptoProfile;   // Volatile, fewer levels
VolumeProfile<256> stockProfile;    // More granular
VolumeProfile<512> futuresProfile;  // High precision needed
```

## See Also

- [Volume Profile Reference](../reference/api/aggregator/volume_profile.md)
- [Footprint Chart](../reference/api/aggregator/footprint_chart.md)
- [Bar Types Explained](../explanation/bar-types.md)
