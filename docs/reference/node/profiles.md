# Profiles & statistics

---

## VolumeProfile

```javascript
const vp = new flox.VolumeProfile(tickSize);
vp.addTrade(price, qty, isBuy);
```

| Method | Returns | Description |
|--------|---------|-------------|
| `addTrade(price, qty, isBuy)` | `void` | Feed a trade |
| `poc()` | `number` | Point of control (highest volume price) |
| `valueAreaHigh()` | `number` | Value area high |
| `valueAreaLow()` | `number` | Value area low |
| `totalVolume()` | `number` | Total volume |
| `clear()` | `void` | Reset |

---

## MarketProfile

```javascript
const mp = new flox.MarketProfile(tickSize, periodMinutes, sessionStartNs);
mp.addTrade(timestampNs, price, qty, isBuy);
```

| Method | Returns | Description |
|--------|---------|-------------|
| `addTrade(tsNs, price, qty, isBuy)` | `void` | Feed a trade |
| `poc()` | `number` | Point of control |
| `valueAreaHigh()` | `number` | Value area high |
| `valueAreaLow()` | `number` | Value area low |
| `initialBalanceHigh()` | `number` | Initial balance high |
| `initialBalanceLow()` | `number` | Initial balance low |
| `isPoorHigh()` | `boolean` | Poor high |
| `isPoorLow()` | `boolean` | Poor low |
| `clear()` | `void` | Reset |

---

## FootprintBar

Buy/sell delta per price level.

```javascript
const fp = new flox.FootprintBar(tickSize);
fp.addTrade(price, qty, isBuy);
```

| Method / Property | Returns | Description |
|-------------------|---------|-------------|
| `addTrade(price, qty, isBuy)` | `void` | Feed a trade |
| `totalDelta()` | `number` | Net buy minus sell volume |
| `totalVolume()` | `number` | Total volume |
| `numLevels` | `number` | Number of price levels (property) |
| `clear()` | `void` | Reset |

---

## Statistics

All functions take `Float64Array` inputs.

| Function | Returns | Description |
|----------|---------|-------------|
| `correlation(x, y)` | `number` | Pearson correlation |
| `profitFactor(pnl)` | `number` | Gross profit / gross loss |
| `winRate(pnl)` | `number` | Fraction of positive values |
| `bootstrapCI(data, confidence?, samples?)` | `{ lower, median, upper }` | Bootstrap confidence interval (default: 0.95, 10000 samples) |
| `permutationTest(group1, group2, samples?)` | `number` | Two-sample permutation p-value (default: 10000) |
| `barReturns(longSignals, shortSignals, logReturns)` | `Float64Array` | Per-bar returns given signal arrays |
| `tradePnl(longSignals, shortSignals, logReturns)` | `Float64Array` | PnL per closed trade |
