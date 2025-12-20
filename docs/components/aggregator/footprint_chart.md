# Footprint Chart

Footprint charts track bid and ask volume separately at each price level, enabling detailed order flow analysis and imbalance detection.

## Overview

```cpp
#include "flox/aggregator/custom/footprint_bar.h"

FootprintBar<64> footprint;
footprint.setTickSize(Price::fromDouble(0.50));

// Add trades
footprint.addTrade(tradeEvent);

// Analyze
Quantity delta = footprint.totalDelta();
Price buyPressure = footprint.highestBuyingPressure();
Price sellPressure = footprint.highestSellingPressure();
Price imbalance = footprint.strongestImbalance(0.7);
```

## Key Concepts

### Bid vs Ask Volume

- **Ask Volume**: Buy market orders hitting the ask (aggressive buyers)
- **Bid Volume**: Sell market orders hitting the bid (aggressive sellers)

```cpp
const auto* level = footprint.levelAt(price);
Quantity askVol = level->askVolume;  // Buyers
Quantity bidVol = level->bidVolume;  // Sellers
```

### Delta

The difference between buying and selling aggression at each level:

```cpp
Quantity levelDelta = level->delta();      // askVolume - bidVolume
Quantity totalDelta = footprint.totalDelta();
```

Positive delta = more aggressive buyers.
Negative delta = more aggressive sellers.

### Imbalance

When one side dominates at a price level:

```cpp
double ratio = level->imbalanceRatio();  // askVolume / totalVolume

if (ratio > 0.7) {
  // Strong buying (70%+ ask volume)
} else if (ratio < 0.3) {
  // Strong selling (70%+ bid volume)
}
```

## API Reference

### Configuration

```cpp
FootprintBar<MaxLevels> footprint;
footprint.setTickSize(Price::fromDouble(0.25));  // Price quantization
```

### Adding Trades

```cpp
footprint.addTrade(tradeEvent);
```

Trade is classified as:
- `isBuy = true` → Ask volume (buyer aggression)
- `isBuy = false` → Bid volume (seller aggression)

### Aggregate Metrics

```cpp
Quantity delta = footprint.totalDelta();     // Sum of all level deltas
Quantity volume = footprint.totalVolume();   // Total volume (bid + ask)
size_t levels = footprint.numLevels();       // Active price levels
```

### Level Access

```cpp
// By index
const Level* lvl = footprint.level(0);

// By price
const Level* lvl = footprint.levelAt(Price::fromDouble(100.0));

// Level structure
struct Level {
  Price price;
  Quantity bidVolume;   // Sellers hitting bid
  Quantity askVolume;   // Buyers lifting ask

  Quantity totalVolume() const;    // bid + ask
  Quantity delta() const;          // ask - bid
  double imbalanceRatio() const;   // ask / total (0.0 to 1.0)
  bool isSinglePrint() const;      // Only one TPO (for market profile)
};
```

### Key Levels

```cpp
// Price with most aggressive buying
Price buyPressure = footprint.highestBuyingPressure();

// Price with most aggressive selling
Price sellPressure = footprint.highestSellingPressure();

// Strongest imbalance above threshold
Price imbalance = footprint.strongestImbalance(0.7);
```

### Reset

```cpp
footprint.clear();
```

## Trading Strategies

### Support/Resistance from Imbalances

```cpp
Price buyImbalance = footprint.strongestImbalance(0.7);
Price sellImbalance = footprint.strongestImbalance(0.3);

// Strong buying = potential support
// Strong selling = potential resistance
```

### Delta Divergence

```cpp
void onBar(const BarEvent& ev) {
  if (ev.bar.close > ev.bar.open) {  // Up bar
    if (footprint.totalDelta().raw() < 0) {
      // Price up but sellers dominate - bearish divergence
    }
  }
}
```

### Absorption Detection

```cpp
const auto* highLevel = footprint.levelAt(highPrice);
if (highLevel && highLevel->bidVolume.raw() > highLevel->askVolume.raw() * 2) {
  // Heavy selling absorbed at high - potential reversal
}
```

## Footprint Visualization

```
+--------+--------+--------+--------+
| Price  |  Bid   |  Ask   | Delta  |
+--------+--------+--------+--------+
|  101.0 |   18.0 |    2.0 | - 16.0 |  <- Heavy selling
|  100.5 |    4.0 |    5.0 | +  1.0 |
|  100.0 |    3.0 |   27.0 | + 24.0 |  <- Heavy buying
|   99.5 |    6.0 |    4.0 | -  2.0 |
|   99.0 |    8.0 |    3.0 | -  5.0 |
+--------+--------+--------+--------+
```

## Combining with Bars

Reset footprint at each bar close to get per-bar order flow:

```cpp
FootprintBar<64> currentFootprint;

void onTrade(const TradeEvent& ev) {
  currentFootprint.addTrade(ev);
}

void onBar(const BarEvent& ev) {
  // Analyze footprint for completed bar
  Quantity delta = currentFootprint.totalDelta();
  Price imbalance = currentFootprint.strongestImbalance();

  // Store for later analysis
  footprintHistory.push_back({ev.bar, currentFootprint});

  // Reset for next bar
  currentFootprint.clear();
}
```

## Performance

| Operation | Complexity |
|-----------|------------|
| addTrade | O(n) worst case |
| totalDelta | O(n) |
| highestBuyingPressure | O(n) |
| strongestImbalance | O(n) |
| levelAt | O(n) |

Where n = number of active price levels (≤ MaxLevels).

## Example Output

```
=== Order Flow Analysis ===
Total Volume: 80
Total Delta:  2

Highest buying pressure:  $100
Highest selling pressure: $101

Strong imbalance at $100 (ratio: 90% buy)

=== Trading Signals ===
[BIAS] Bullish - Net buying pressure
[SUPPORT] Strong buying at $100.0 - potential support
[RESISTANCE] Strong selling at $101.0 - potential resistance
```

## Files

| File | Description |
|------|-------------|
| `aggregator/custom/footprint_bar.h` | FootprintBar template |

## See Also

- [Volume Profile](volume_profile.md) - Aggregate volume distribution
- [Market Profile](market_profile.md) - Time-based analysis (TPO)
- [Bar Aggregator](bar_aggregator.md) - OHLCV bar generation
