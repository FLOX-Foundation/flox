# Market Profile (TPO)

Market Profile tracks price activity across time periods using TPO (Time Price Opportunity) analysis. Each time period is assigned a letter (A, B, C...) and the profile shows which prices were visited during each period.

## Overview

```cpp
#include "flox/aggregator/custom/market_profile.h"

MarketProfile<256, 26> profile;  // 256 price levels, 26 periods (A-Z)
profile.setTickSize(Price::fromDouble(1.0));
profile.setPeriodDuration(std::chrono::minutes(30));
profile.setSessionStart(sessionOpenNanoseconds);

// Add trades
profile.addTrade(tradeEvent);

// Analyze
Price poc = profile.poc();              // Point of Control
Price vaLow = profile.valueAreaLow();   // Value Area Low
Price vaHigh = profile.valueAreaHigh(); // Value Area High
Price ibLow = profile.initialBalanceLow();   // Initial Balance Low
Price ibHigh = profile.initialBalanceHigh(); // Initial Balance High
```

## Key Concepts

### TPO (Time Price Opportunity)

Each time price visits a level during a period, it creates a TPO. A price with multiple TPOs indicates acceptance; single TPOs indicate rejection.

```cpp
const auto* level = profile.levelAt(price);
uint32_t tpoCount = level->tpoCount;    // Number of periods at this price
bool singlePrint = level->isSinglePrint();  // Only one period
```

### Point of Control (POC)

The price with the most TPOs (not volume, unlike Volume Profile):

```cpp
Price poc = profile.poc();
```

### Value Area (VA)

Price range containing 70% of all TPOs:

```cpp
Price vaLow = profile.valueAreaLow();
Price vaHigh = profile.valueAreaHigh();
```

### Initial Balance (IB)

The price range established in the first hour of trading (periods A + B):

```cpp
Price ibLow = profile.initialBalanceLow();
Price ibHigh = profile.initialBalanceHigh();

// IB range predicts day type
Price ibRange = Price::fromRaw(ibHigh.raw() - ibLow.raw());
```

### Single Prints

Prices visited only once (single TPO). Often act as support/resistance:

```cpp
auto [count, prices] = profile.singlePrints();
for (size_t i = 0; i < count; ++i) {
  // prices[i] is a potential S/R level
}
```

### Poor Highs/Lows

When the session high or low is a single print, it indicates weak rejection:

```cpp
if (profile.isPoorHigh()) {
  // Session high only visited once - weak resistance
}
if (profile.isPoorLow()) {
  // Session low only visited once - weak support
}
```

## API Reference

### Configuration

```cpp
MarketProfile<MaxLevels, MaxPeriods> profile;

profile.setTickSize(Price::fromDouble(0.25));
profile.setPeriodDuration(std::chrono::minutes(30));
profile.setSessionStart(sessionStartNs);  // Unix nanoseconds
```

### Adding Trades

```cpp
profile.addTrade(tradeEvent);
```

The period is calculated from `(tradeTs - sessionStart) / periodDuration`.

### Key Levels

```cpp
Price poc = profile.poc();
Price vaLow = profile.valueAreaLow();
Price vaHigh = profile.valueAreaHigh();
Price ibLow = profile.initialBalanceLow();
Price ibHigh = profile.initialBalanceHigh();
Price high = profile.highPrice();
Price low = profile.lowPrice();
```

### Structure Analysis

```cpp
bool poorHigh = profile.isPoorHigh();   // Single TPO at high
bool poorLow = profile.isPoorLow();     // Single TPO at low

auto [count, singles] = profile.singlePrints();  // All single prints
```

### Level Access

```cpp
// By index
const Level* lvl = profile.level(0);

// By price
const Level* lvl = profile.levelAt(price);
uint32_t tpos = profile.tpoCountAt(price);

// Level structure
struct Level {
  Price price;
  std::bitset<MaxPeriods> tpos;  // Which periods traded here
  uint32_t tpoCount;             // Number of active periods

  bool hasPeriod(size_t period) const;  // Did period trade here?
  bool isSinglePrint() const;           // Only one TPO
};
```

### Period Info

```cpp
size_t currentPeriod = profile.currentPeriod();  // Latest active period
char letter = MarketProfile<>::periodLetter(0);  // 'A'
```

### Reset

```cpp
profile.clear();
```

## Trading Strategies

### Value Area Trading

```cpp
Price vaLow = profile.valueAreaLow();
Price vaHigh = profile.valueAreaHigh();
Price close = currentPrice;

if (close < vaLow) {
  // Below value - expect rotation back to POC
  // Look for long entries
} else if (close > vaHigh) {
  // Above value - expect rotation back to POC
  // Look for short entries
}
```

### IB Breakout

```cpp
Price ibLow = profile.initialBalanceLow();
Price ibHigh = profile.initialBalanceHigh();

if (currentPrice > ibHigh) {
  // IB breakout up - trend day potential
} else if (currentPrice < ibLow) {
  // IB breakout down - trend day potential
}
```

### Single Print Fade

```cpp
auto [count, singles] = profile.singlePrints();

for (size_t i = 0; i < count; ++i) {
  if (currentPrice > singles[i]) {
    // Price above single print - potential support
  }
}
```

### Poor High/Low Continuation

```cpp
if (profile.isPoorHigh() && currentPrice > profile.highPrice()) {
  // Poor high broken - continuation likely
}
```

## Profile Visualization

```
 Price  | TPOs              | Markers
--------+-------------------+---------
 105.0  | D                 | <HIGH>
 104.0  | BD                | <IBH>
 103.0  | ABCDEF            | <POC> <VAH>
 102.0  | ABCDEF            |
 101.0  | ABCEF             |
 100.0  | ABCF              | <VAL>
  99.0  | A                 |
  98.0  | A                 | <IBL> <LOW>
```

## Day Types

Based on IB extension and value area:

| Day Type | Characteristic | Trading Approach |
|----------|---------------|------------------|
| Normal | Most volume in middle, balanced | Fade extremes to POC |
| Normal Variation | Slight extension one side | Trade with extension |
| Trend | Strong extension, poor high/low | Trade breakouts |
| Double Distribution | Two value areas | Trade between them |
| P-Shape | Wide base, narrow top | Shorts at top |
| b-Shape | Narrow base, wide top | Longs at bottom |

## Performance

| Operation | Complexity |
|-----------|------------|
| addTrade | O(n) worst case |
| poc | O(n) |
| valueAreaLow/High | O(n log n) |
| singlePrints | O(n) |
| isPoorHigh/Low | O(n) |

Where n = number of active price levels (≤ MaxLevels).

## Example Output

```
=== Profile Analysis ===
Session High: $105
Session Low:  $98
POC:          $103
Value Area:   $100 - $103
Initial Balance: $98 - $104

=== Structure Analysis ===
[WARNING] Poor high detected - weak resistance
[OK] Strong low - good support

=== Single Prints ===
  $98
  $99
  $105

=== Trading Signals ===
[STRATEGY] Fade extremes, trade to value
  - If price above VAH ($103): Look for shorts to POC
  - If price below VAL ($100): Look for longs to POC
```

## Files

| File | Description |
|------|-------------|
| `aggregator/custom/market_profile.h` | MarketProfile template |

## See Also

- [Volume Profile](volume_profile.md) - Volume-based distribution
- [Footprint Chart](footprint_chart.md) - Order flow analysis
- [Bar Aggregator](bar_aggregator.md) - OHLCV bar generation
