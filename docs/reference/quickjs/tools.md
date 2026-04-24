# Order books, positions, profiles, statistics

---

## Order book

```javascript
const book = new OrderBook(0.01);  // tick size
book.applySnapshot([50000, 49999], [1.5, 2.0], [50001, 50002], [0.5, 1.0]);
book.bestBid();    // 50000
book.bestAsk();    // 50001
book.mid();
book.spread();
book.getBids(5);   // [[price, qty], ...]
book.getAsks(5);

// L3 (order-level)
const l3 = new L3Book();
l3.addOrder(1, 50000, 1.5, 'buy');
l3.removeOrder(1);
l3.bestBid();
```

---

## Position tracking

```javascript
const tracker = new PositionTracker();
tracker.onFill(symbolId, 'buy', 50000, 1.0);
tracker.onFill(symbolId, 'sell', 50100, 1.0);
tracker.position(symbolId);        // 0
tracker.realizedPnl(symbolId);     // 100
tracker.totalRealizedPnl();

// Group tracking
const groups = new PositionGroupTracker();
const pid = groups.openPosition(symbolId, groupId, 'buy', 50000, 1.0);
groups.closePosition(pid, 50500);
groups.totalRealizedPnl();
```

---

## Profiles

```javascript
// Volume profile
const vp = new VolumeProfile(0.01);
vp.addTrade(50000, 1.0, true);
vp.poc();
vp.valueAreaHigh();
vp.valueAreaLow();

// Market profile
const mp = new MarketProfile(0.01, 30, 0);
mp.addTrade(Date.now() * 1e6, 50000, 1.0, true);
mp.poc();
mp.initialBalanceHigh();
mp.isPoorHigh();

// Footprint
const fp = new FootprintBar(0.01);
fp.addTrade(50000, 1.0, true);
fp.totalDelta();
fp.totalVolume();
```

---

## Statistics

```javascript
flox.correlation([1, 2, 3], [1, 2, 3]);
flox.profitFactor([100, -50, 200, -30]);
flox.winRate([100, -50, 200, -30]);
flox.bootstrapCI([1, 2, 3, 4, 5], 0.95, 10000);  // { lower, median, upper }
flox.permutationTest([1, 2, 3], [4, 5, 6], 10000); // p-value
```

---

## Segment operations

```javascript
flox.validateSegment('/path/to/segment.flx');
flox.mergeSegments('/path/to/input_dir', '/path/to/output_dir');
```
