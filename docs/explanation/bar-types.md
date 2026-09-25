# Understanding Bar Types

This document explains the different bar types available in Flox and when to use each one. Examples are shown in C++ because that's where the aggregator templates live, but every binding can produce and consume each bar type — see [Bar aggregation](../how-to/bar-aggregation.md) for the binding-level API.

## The Problem with Time Bars

Traditional time-based bars (1-minute, hourly, daily) have one problem: **information content varies with market activity**.

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
| **Range** | High-low exceeds an absolute price threshold | Volatility-based analysis |
| **BpsRange** | High-low exceeds a threshold in basis points of the open | Volatility-based analysis across instruments and price levels |
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

**Late trades**: a time bar is a clock construct, so a trade can arrive for a bucket that is no longer open — routine when several venue feeds are merged, or when a feed reconnects and replays. A trade whose aligned bucket is *earlier* than the live bar's `startTime` is **dropped**, and the drop is counted in `BarAggregator::lateTradeCount()` (and `MultiTimeframeAggregator::lateTradeCount()`). It is not folded into the live bar: doing that made the trade's price the live bar's close, and possibly its high or low, so the bar published for an interval reported a price that never traded inside it. Nor does it reopen the bucket it belongs to: that bar has already been published, and reopening it would break the monotonicity of `bar.startTime` that `BarSeries` and `BarMatrix` rely on.

The rule is about the *bucket*, not about arrival order. A trade that arrives out of order but still falls inside the live bucket is ordinary data and is folded in like any other; only a trade from an earlier bucket is dropped. If the count keeps climbing, the feed merge upstream is delivering trades later than one bar interval and the interval — or the merge — needs to change.

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

**Example**: A 100-tick bar during high volatility might span 1 second; during quiet periods, 10 minutes. But each bar represents the same amount of "market activity."

**Publication lag**: a tick bar's *contents* are always exactly N trades, but the aggregator only recognizes a bar as complete when it sees a trade that doesn't belong to it -- so a 100-tick bar is emitted on the 101st trade, not the 100th. This is the same structural lag every closing rule in this aggregator has (a time bar likewise cannot close without a trade from the next interval); it is not data loss or corruption, only a one-event delay before the already-correct bar is handed to a subscriber.

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

`param()` reports the threshold in the same units you passed to `fromDouble()` (e.g. `1000000` for the example above), which is also what `TimeframeId::volume(threshold)` expects -- the two must agree for `BarMatrix` to find the bars this policy emits.

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
- The synthesized bricks from a gap (see "Gaps" below) carry no volume or trade-count data of their own, since no trade actually happened at those prices

**Use when**:

- Trend following strategies
- Support/resistance identification
- Filtering out market noise

**Unique property**: Renko bars only move one direction until reversal. A series of up-bricks means consistent upward movement without significant pullbacks.

**Where a brick closes**: a brick closes when a trade's price is at least one brick size away from the brick's open. The brick is a price construct, so both ends of the close are fixed by the grid rather than by the trade that happened to cross it:

- The crossing trade is applied **first** and is counted in the brick it completes — its notional lands in that brick's `volume` and its tick in that brick's `tradeCount`.
- The brick closes **at the boundary**, exactly one brick size from its open and in the direction of the move.
- The next brick **opens at that same boundary**, not at the crossing trade's price, so bricks chain (`bricks[i].open == bricks[i-1].close`) and the grid never drifts.
- The brick's extreme on the side of the move is the boundary; the crossing trade's overshoot past it belongs to the brick that opens next, which is where it is reported. The opposite side keeps whatever retracement the trades actually made, so a brick can have a wick against the move but not with it.

Brick size 10 with trades 100, 95, 111 therefore produces one brick, `open 100 → close 110`, `low 95`, `tradeCount 3`; the brick that opens next runs from 110 and stands at 111. The brick that opens after a close starts empty (`tradeCount 0`, `volume 0`) — the trade that opened it was already counted — while its `close` tracks the current price, so a `stop()` flush still reports where the price stands.

**Gaps**: if a single trade jumps several brick sizes at once — a real gap, or just a thin book — Flox fills in the intermediate bricks a continuous price path would have produced. The brick that was forming closes at the first boundary the trade crossed, then one synthetic brick per additional whole brick width walks the price toward the trade in clean steps of exactly one brick size, and the new brick opens at the last boundary reached. A trade that jumps from 100 to 155 with a brick size of 10 spans 5.5 bricks: the real brick (100 → 110) plus 4 synthesized bricks (110 → 120, 120 → 130, 130 → 140, 140 → 150) account for the 5 complete bricks the move spans, leaving a new brick open at 150 — the boundary — with the leftover 5 points showing as its close. The synthesized bricks have no volume or trade count of their own — no trade happened at those prices — so treat them as a price-path marker, not as a record of activity.

**The gap walk is bounded**: at most `RenkoBarPolicy::kMaxGapBricks` (1024) bars are published for any one trade, including the brick that was forming. Without the bound, the cost of a single print scales with how far the price moved: a brick size of 0.01 and a print from 1.00 to 100000.00 spans ten million bricks, which stalls the bus and exhausts memory inside one `onTrade()`. When the bound is hit, the last bar published absorbs the whole remainder of the move: it is taller than one brick, it closes on the far boundary so the grid stays aligned with the price, and it carries `BarCloseReason::Gap` so a consumer can tell a truncated walk from a complete one instead of silently seeing fewer bricks than the move spanned. `Gap` is the only reason the engine sets besides `Threshold` and `Forced`.

`param()` reports the brick size in the instrument's own price units (e.g. `10` for `RenkoBarPolicy::fromDouble(10.0)`), matching `TimeframeId::renko(brickSize)`.

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

The closed bar's own high-low range always reaches the threshold (the trade that pushes the accumulated range to the threshold is folded into that bar before it closes); as with Tick bars, the *close is only detected* on the following trade, which then opens the next bar.

`param()` reports the range size in the instrument's own price units (e.g. `5` for `RangeBarPolicy::fromDouble(5.0)`), matching `TimeframeId::range(rangeSize)`.

## BpsRange Bars

```cpp
BpsRangeBarAggregator aggregator(BpsRangeBarPolicy(20.0), &bus);  // 20 bps
```

**How it works**: Close when `(high − low) / open` reaches the threshold expressed in basis points. Same idea as Range bars, but relative instead of absolute.

**Pros**:

- One threshold works across instruments with different price levels
- Stays meaningful as an instrument's price drifts over a long backtest
- No re-tuning after a redenomination or a large price move

**Cons**:

- Needs a valid open price; a bar with `open <= 0` never closes on this rule
- Slightly less intuitive than an absolute range in the instrument's own units

**Use when**:

- Running the same strategy across a basket at different price levels
- Long backtests where an absolute range threshold would drift out of relevance

`param()` encodes the threshold as bps × 100, so a 20 bps policy reports `2000`.

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

**Unique property**: In a strong uptrend, Heikin-Ashi bars will show no lower wicks (or very small ones). Strong downtrends show no upper wicks.

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
│   ├── Volatility, absolute units → Use RANGE bars
│   └── Volatility, relative → Use BPSRANGE bars
```

### By Strategy Type

| Strategy | Recommended Bar Type |
|----------|---------------------|
| Mean reversion | Time or Volume |
| Momentum | Time, Renko, or Heikin-Ashi |
| Scalping/HFT | Tick |
| Trend following | Renko, Heikin-Ashi, or Time |
| Volatility trading | Range or BpsRange |
| Statistical arb | Tick or Volume |
| Swing trading | Time (H1, D1) or Heikin-Ashi |

### By Market Condition

| Condition | Better Choice |
|-----------|--------------|
| High volatility | Range, BpsRange, or Renko |
| Low liquidity | Volume |
| 24/7 markets | Tick or Volume |
| Session-based | Time |
| Trending | Renko or Heikin-Ashi |
| Ranging | Time or Range |
| Noisy markets | Heikin-Ashi |

## Multi-Timeframe with Mixed Types

One aggregator can carry several bar types at once:

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

| Operation | Time | Tick | Volume | Renko | Range | BpsRange | Heikin-Ashi |
|-----------|------|------|--------|-------|-------|----------|-------------|
| shouldClose() | O(1) | O(1) | O(1) | O(1) | O(1) | O(1) | O(1) |
| update() | O(1) | O(1) | O(1) | O(1) | O(1) | O(1) | O(1) |

The main difference is **bar frequency**, not computational overhead.

## Summary

Time bars are the familiar default, at the cost of inconsistent information per bar. Tick bars hold activity constant, which is what HFT work usually wants; volume bars hold economic significance constant instead. Renko strips noise out of the trend picture. Range bars normalize volatility in absolute price units and BpsRange in basis points of the open. Heikin-Ashi smooths the trend but gives up exact prices.

Choose based on what your strategy needs to hold constant: **time**, **activity**, **volume**, **price movement**, **volatility**, or **trend clarity**.

## See Also

- [Bar Aggregator Reference](../reference/api/aggregator/bar_aggregator.md)
- [How to Create Custom Bar Policy](../how-to/custom-bar-policy.md)
- [Multi-Timeframe Strategy Tutorial](../tutorials/multi-timeframe-strategy.md)
