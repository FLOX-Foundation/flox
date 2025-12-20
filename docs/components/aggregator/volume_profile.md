# Volume Profile

Volume Profile tracks the distribution of volume across price levels, providing insights into market structure and key support/resistance zones.

## Overview

```cpp
#include "flox/aggregator/custom/volume_profile.h"

VolumeProfile<256> profile;
profile.setTickSize(Price::fromDouble(0.50));  // Aggregate to $0.50 levels

// Add trades
profile.addTrade(tradeEvent);

// Analyze
Price poc = profile.poc();              // Point of Control
Price vaLow = profile.valueAreaLow();   // Value Area Low
Price vaHigh = profile.valueAreaHigh(); // Value Area High
Volume delta = profile.totalDelta();    // Buy - Sell volume
```

## Key Concepts

### Point of Control (POC)

The price level with the highest traded volume. Often acts as a magnet for price:

```cpp
Price poc = profile.poc();
if (currentPrice < poc) {
  // Expect price to move toward POC
}
```

### Value Area (VA)

The price range containing 70% of total volume. Represents "fair value":

```cpp
Price vaLow = profile.valueAreaLow();
Price vaHigh = profile.valueAreaHigh();

if (currentPrice < vaLow) {
  // Below value - potential buying opportunity
} else if (currentPrice > vaHigh) {
  // Above value - potential selling opportunity
}
```

### Delta

The difference between buy and sell volume. Indicates aggressor dominance:

```cpp
Volume delta = profile.totalDelta();

if (delta.raw() > 0) {
  // Net buying pressure - bullish
} else {
  // Net selling pressure - bearish
}
```

## API Reference

### Configuration

```cpp
VolumeProfile<MaxLevels> profile;

// Set tick size for price quantization
profile.setTickSize(Price::fromDouble(1.0));
```

### Adding Trades

```cpp
profile.addTrade(tradeEvent);
```

Volume is calculated as notional: `price * quantity`.

### Key Levels

```cpp
Price poc = profile.poc();              // Highest volume price
Price vaLow = profile.valueAreaLow();   // Lower bound of 70% volume
Price vaHigh = profile.valueAreaHigh(); // Upper bound of 70% volume
```

### Volume Metrics

```cpp
Volume total = profile.totalVolume();   // Total notional volume
Volume delta = profile.totalDelta();    // Buy volume - Sell volume
size_t levels = profile.numLevels();    // Number of active price levels
```

### Level Access

```cpp
// By index
const Level* lvl = profile.level(0);

// By price
Volume vol = profile.volumeAt(Price::fromDouble(100.0));

// Level structure
struct Level {
  Price price;
  Volume volume;      // Total volume at this price
  Volume buyVolume;   // Buy-side volume

  Volume sellVolume() const;  // volume - buyVolume
  Volume delta() const;       // buyVolume - sellVolume
};
```

### Reset

```cpp
profile.clear();  // Reset for new session
```

## Trading Strategies

### Mean Reversion to POC

```cpp
void onTrade(const TradeEvent& ev) {
  profile.addTrade(ev);

  Price poc = profile.poc();
  Price price = ev.trade.price;

  if (price.raw() < poc.raw() * 0.995) {  // 0.5% below POC
    // Buy signal - expect reversion to POC
  }
}
```

### Value Area Fade

```cpp
void onBar(const BarEvent& ev) {
  Price vaLow = profile.valueAreaLow();
  Price vaHigh = profile.valueAreaHigh();
  Price close = ev.bar.close;

  if (close < vaLow) {
    // Price below value - look for long entries
  } else if (close > vaHigh) {
    // Price above value - look for short entries
  }
}
```

### Delta Confirmation

```cpp
Volume delta = profile.totalDelta();
Volume volumeAtPrice = profile.volumeAt(currentPrice);

if (delta.raw() > 0 && volumeAtPrice.raw() > avgVolume) {
  // Bullish with high volume confirmation
}
```

## Multi-Session Profiles

Build composite profiles from multiple sessions:

```cpp
VolumeProfile<256> dailyProfile;
VolumeProfile<256> weeklyProfile;

void onSessionEnd() {
  // Transfer daily levels to weekly
  for (size_t i = 0; i < dailyProfile.numLevels(); ++i) {
    const auto* lvl = dailyProfile.level(i);
    // Add to weekly...
  }
  dailyProfile.clear();
}
```

## Performance

| Operation | Complexity |
|-----------|------------|
| addTrade | O(n) worst case, O(1) average |
| poc | O(n) |
| valueAreaLow/High | O(n log n) |
| volumeAt | O(n) |

Where n = number of active price levels (≤ MaxLevels).

## Example Output

```
=== Volume Profile Analysis ===
Total volume: $279786
Total delta:  $16549.9
POC:          $100
Value Area:   $97 - $103
Levels:       19

Top volume levels:
  $100 | Vol: $37405 | Delta: $4403
  $99  | Vol: $33570 | Delta: $4090
  $101 | Vol: $33311 | Delta: $2222
```

## Files

| File | Description |
|------|-------------|
| `aggregator/custom/volume_profile.h` | VolumeProfile template |

## See Also

- [Footprint Chart](footprint_chart.md) - Bid/ask volume at each level
- [Market Profile](market_profile.md) - Time-based distribution (TPO)
- [Bar Aggregator](bar_aggregator.md) - OHLCV bar generation
