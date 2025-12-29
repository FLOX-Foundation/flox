# Understanding Bar Types

This document explains the different bar types available in Flox and when to use each one.

## The Problem with Time Bars

Traditional time-based bars (1-minute, hourly, daily) have a fundamental issue: **information content varies with market activity**.

- During high activity: bars pack lots of information
- During low activity: bars contain little information (noise)

This inconsistency creates problems:

- Indicators behave differently at different times
- Backtests may not reflect live performance
- Overnight gaps distort analysis

Alternative bar types address this by normalizing **what** closes a bar rather than **when**.

## Bar Types Overview

| Type | Closes When | Best For |
|------|-------------|----------|
| **Time** | Fixed time interval | Traditional analysis, backtesting |
| **Tick** | N trades occur | HFT, eliminating time bias |
| **Volume** | Notional volume threshold | Volume-weighted analysis |
| **Renko** | Price moves by brick size | Trend following, noise elimination |
| **Range** | High-low exceeds threshold | Volatility-based analysis |
| **Heikin-Ashi** | Fixed time interval (smoothed) | Trend clarity, noise reduction |

## Time Bars

```cpp
TimeBarAggregator aggregator(TimeBarPolicy(std::chrono::seconds(60)), &bus);
```

**How it works**: Close after a fixed time interval (e.g., 1 minute).

**Pros**:

- Familiar, widely used
- Easy to compare across instruments
- Works with most existing tools

**Cons**:

- Information content varies
- Overnight gaps create distortions
- Low-activity periods add noise

**Use when**:

- Backtesting strategies designed for time bars
- Comparing to external data sources
- Building indicators that expect regular intervals

## Tick Bars

```cpp
TickBarAggregator aggregator(TickBarPolicy(100), &bus);  // 100 trades per bar
```

**How it works**: Close after N trades occur, regardless of time.

**Pros**:

- Consistent information per bar
- No time-based distortions
- Better for statistical analysis

**Cons**:

- Bar duration varies wildly
- Can't easily compare across instruments
- May produce many bars during high activity

**Use when**:

- High-frequency strategies
- Statistical arbitrage
- Eliminating time-of-day effects

**Example insight**: A 100-tick bar during high volatility might span 1 second; during quiet periods, 10 minutes. But each bar represents the same amount of "market activity."

## Volume Bars

```cpp
VolumeBarAggregator aggregator(VolumeBarPolicy::fromDouble(1000000.0), &bus);
```

**How it works**: Close after notional volume (price × quantity) reaches threshold.

**Pros**:

- Normalizes for trade size variation
- Better represents institutional activity
- Consistent economic significance per bar

**Cons**:

- Threshold needs tuning per instrument
- Price changes affect bar frequency

**Use when**:

- Analyzing institutional flow
- Volume-weighted strategies
- Markets with varying trade sizes

**Example**: $1M volume bars on BTC might close every few seconds during active trading, but take hours overnight.

## Renko Bars

```cpp
RenkoBarAggregator aggregator(RenkoBarPolicy::fromDouble(10.0), &bus);
```

**How it works**: New bar only when price moves by "brick size" from previous close.

**Pros**:

- Eliminates noise
- Clear trend visualization
- No time or volume dependency

**Cons**:

- Loses timing information
- Can miss reversals within brick
- Gaps create multiple bricks

**Use when**:

- Trend following strategies
- Support/resistance identification
- Filtering out market noise

**Unique property**: Renko bars only move one direction until reversal. A series of up-bricks means consistent upward movement without significant pullbacks.

## Range Bars

```cpp
RangeBarAggregator aggregator(RangeBarPolicy::fromDouble(5.0), &bus);
```

**How it works**: Close when high-low range exceeds threshold.

**Pros**:

- Consistent volatility per bar
- Adapts to market conditions
- Good for breakout detection

**Cons**:

- Can produce many small bars in trending markets
- Range threshold needs tuning

**Use when**:

- Volatility-based strategies
- Breakout trading
- Options-related strategies

**Example**: $5 range bars will close quickly during volatile periods (many bars) and slowly during consolidation (fewer bars).

## Heikin-Ashi Bars

```cpp
HeikinAshiBarAggregator aggregator(HeikinAshiBarPolicy(std::chrono::seconds(60)), &bus);
```

**How it works**: Uses smoothed OHLC calculations based on previous bar:

- HA_Close = (Open + High + Low + Close) / 4
- HA_Open = (prev_HA_Open + prev_HA_Close) / 2
- HA_High = max(High, HA_Open, HA_Close)
- HA_Low = min(Low, HA_Open, HA_Close)

**Pros**:

- Smoother trends, easier to identify
- Reduces noise from individual bars
- Bullish bars always have close > open
- Great for visual trend analysis

**Cons**:

- Loses exact price information
- Not suitable for precise entries
- Lags behind actual price
- Requires previous bar for calculation

**Use when**:

- Trend following strategies
- Visual trend confirmation
- Reducing false signals in choppy markets
- Swing trading with trend filters

**Unique property**: In a strong uptrend, Heikin-Ashi bars will show no lower wicks (or very small ones). Conversely, strong downtrends show no upper wicks. This makes trend strength immediately visible.

**Multi-symbol support**: The Heikin-Ashi aggregator maintains independent state per symbol, so a single aggregator instance can correctly handle multiple symbols simultaneously.

## Choosing the Right Bar Type

### Decision Framework

```
What matters most for your strategy?

├── Time consistency?
│   └── Use TIME bars
│
├── Trade activity?
│   └── Use TICK bars
│
├── Dollar volume?
│   └── Use VOLUME bars
│
├── Price movement?
│   ├── Trend direction → Use RENKO bars
│   └── Volatility → Use RANGE bars
```

### By Strategy Type

| Strategy | Recommended Bar Type |
|----------|---------------------|
| Mean reversion | Time or Volume |
| Momentum | Time, Renko, or Heikin-Ashi |
| Scalping/HFT | Tick |
| Trend following | Renko, Heikin-Ashi, or Time |
| Volatility trading | Range |
| Statistical arb | Tick or Volume |
| Swing trading | Time (H1, D1) or Heikin-Ashi |

### By Market Condition

| Condition | Better Choice |
|-----------|--------------|
| High volatility | Range or Renko |
| Low liquidity | Volume |
| 24/7 markets | Tick or Volume |
| Session-based | Time |
| Trending | Renko or Heikin-Ashi |
| Ranging | Time or Range |
| Noisy markets | Heikin-Ashi |

## Multi-Timeframe with Mixed Types

Combine bar types for better analysis:

```cpp
MultiTimeframeAggregator<4> aggregator(&bus);
aggregator.addTimeInterval(std::chrono::seconds(60));   // M1 for timing
aggregator.addTimeInterval(std::chrono::seconds(3600)); // H1 for trend
aggregator.addTickInterval(100);                         // Tick for activity
aggregator.addVolumeInterval(1000000.0);                 // Volume for flow
```

**Strategy example**:

- H1 time bars for trend direction
- Volume bars for institutional activity
- Tick bars for precise entry timing

## Performance Comparison

All bar types have similar computational cost:

| Operation | Time | Tick | Volume | Renko | Range | Heikin-Ashi |
|-----------|------|------|--------|-------|-------|-------------|
| shouldClose() | O(1) | O(1) | O(1) | O(1) | O(1) | O(1) |
| update() | O(1) | O(1) | O(1) | O(1) | O(1) | O(1) |

The main difference is **bar frequency**, not computational overhead.

## Summary

- **Time bars**: Traditional, familiar, but information-inconsistent
- **Tick bars**: Consistent activity, good for HFT
- **Volume bars**: Consistent economic significance
- **Renko bars**: Noise-free trend visualization
- **Range bars**: Volatility-normalized
- **Heikin-Ashi bars**: Smoothed trends, noise reduction

Choose based on what your strategy needs to hold constant: **time**, **activity**, **volume**, **price movement**, **volatility**, or **trend clarity**.

## See Also

- [Bar Aggregator Reference](../reference/api/aggregator/bar_aggregator.md)
- [How to Create Custom Bar Policy](../how-to/custom-bar-policy.md)
- [Multi-Timeframe Strategy Tutorial](../tutorials/multi-timeframe-strategy.md)
