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

Segment directories use the `.floxlog` extension.

```javascript
flox.validateSegment('/path/to/segment.floxlog');

// Second argument is an OUTPUT FILE PATH, not a directory.
flox.mergeSegments('/path/to/input_dir', '/path/to/merged.floxlog');
```

Extended operations:

| Call | Description |
|------|-------------|
| `flox.mergeDir(inputDir, outputDir)` | Merge a directory into an output directory |
| `flox.splitSegment(inputPath, outputDir, mode, timeIntervalNs, eventsPerFile)` | Split one segment into many |
| `flox.exportSegment(inputPath, outputPath, format, fromNs, toNs, symbols)` | Export a range to another format |
| `flox.validateSegmentFull(path, verifyCrc, verifyTimestamps)` | Full validation, optional CRC and timestamp checks |
| `flox.validateDataset(dataDir)` | Validate every segment in a dataset directory |
| `flox.recompressSegment(inputPath, outputPath, level)` | Recompress at a different level |
| `flox.extractSymbols(inputPath, outputDir, symbols)` | Extract selected symbols |
| `flox.extractTimeRange(inputPath, outputPath, fromNs, toNs)` | Extract a time window |

---

## Bar aggregation

Each helper takes `(timestamps, prices, quantities, sides, param)`:

| Call | `param` |
|------|---------|
| `flox.timeBars(ts, px, qty, sides, intervalNs)` | Interval in nanoseconds |
| `flox.tickBars(ts, px, qty, sides, ticksPerBar)` | Trades per bar |
| `flox.volumeBars(ts, px, qty, sides, volumePerBar)` | Volume per bar |
| `flox.rangeBars(ts, px, qty, sides, rangeSize)` | High-low range |
| `flox.renkoBars(ts, px, qty, sides, brickSize)` | Brick size |
| `flox.heikinBars(ts, px, qty, sides, intervalNs)` | Interval in nanoseconds |

---

## Data reader / writer / recorder

Read trades and book updates from binary log segments, or record live data.

```javascript
const reader = new DataReader('./data');
// Filtered reader:
//   new DataReader({ dir: './data', fromNs, toNs, symbols })

reader.count;            // total events
reader.summary();        // { firstEventNs, lastEventNs, totalEvents, segmentCount, totalBytes, durationSeconds }
reader.stats();          // { filesRead, eventsRead, tradesRead, bookUpdatesRead, bytesRead, crcErrors }

const trades = reader.readTrades(maxTrades = 0);   // 0 = all
const bbos = reader.readBBO(maxEvents = 0);
const events = reader.readBookUpdates();

// Mid-stream seek: start from a given timestamp
const tradesFrom = reader.readTradesFrom(startTsNs, maxTrades = 0);
const bbosFrom = reader.readBBOFrom(startTsNs, maxEvents = 0);
const eventsFrom = reader.readBookUpdatesFrom(startTsNs);

reader.destroy();
```

Record shapes:

- **Trade**: `{ exchangeTsNs, recvTsNs, price, qty, tradeId, symbolId, side }`
- **BBO**: `{ exchangeTsNs, recvTsNs, seq, symbolId, eventType, bidPrice, bidQty, askPrice, askQty }`
- **Book update**: `{ exchangeTsNs, recvTsNs, seq, symbolId, eventType, bids: [{price, qty}, ...], asks: [{price, qty}, ...] }`

`eventType` is `2` for a snapshot, `3` for a delta.

```javascript
const writer = new DataWriter('./out', maxSegmentMb, exchangeId);
writer.writeTrade(exchangeTsNs, recvTsNs, price, qty, tradeId, symbolId, side);
// Raw int64 book levels — flat BigInt64Array as [price_raw, qty_raw, ...]
writer.writeBook(exchangeTsNs, recvTsNs, seqNs, symbolId, isSnapshot, bidsBuf, asksBuf);
writer.flush();
writer.close();
writer.stats();          // { bytesWritten, eventsWritten, segmentsCreated, tradesWritten }

const hook = new BinaryLogRecorderHook('./out', maxSegmentMb, exchangeId, 'none');
hook.addSymbol(symbolId, name, base, quote, pricePrecision, qtyPrecision);
// attach via the runner's market-data-recorder slot; lifecycle is engine-driven.
hook.destroy();
```
