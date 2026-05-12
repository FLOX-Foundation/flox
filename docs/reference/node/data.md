# Data I/O

---

## DataWriter

Writes trades to binary log segments.

```javascript
const writer = new flox.DataWriter(outputDir, maxSegmentMb?, exchangeId?);
writer.writeTrade(exchangeTsNs, recvTsNs, price, qty, tradeId, symbolId, side);
writer.flush();
writer.close();
```

| Method | Returns | Description |
|--------|---------|-------------|
| `writeTrade(exchangeTsNs, recvTsNs, price, qty, tradeId, symbolId, side)` | `boolean` | Write a trade record |
| `flush()` | `void` | Flush to disk |
| `close()` | `void` | Close and finalize |
| `stats()` | `{ bytesWritten, eventsWritten, segmentsCreated, tradesWritten }` | Write statistics |

---

## DataReader

Reads binary log segments.

```javascript
const reader = new flox.DataReader(dataDir, fromNs?, toNs?);
const trades = reader.readTrades();
const bbos = reader.readBBO();
const events = reader.readBookUpdates();
```

| Method / Property | Returns | Description |
|-------------------|---------|-------------|
| `count` | `number` | Total event count (property) |
| `summary()` | `{ firstEventNs, lastEventNs, totalEvents, segmentCount, totalBytes, durationSeconds }` | Dataset summary |
| `stats()` | `{ filesRead, eventsRead, tradesRead, bookUpdatesRead, bytesRead, crcErrors }` | Read statistics |
| `readTrades(max?)` | trade record array | Read up to `max` trades (all if omitted) |
| `readTradesFrom(startTsNs, max?)` | trade record array | Same as `readTrades` starting from `startTsNs` |
| `readBBO(max?)` | BBO record array | Top of book per book update event |
| `readBBOFrom(startTsNs, max?)` | BBO record array | Same as `readBBO` starting from `startTsNs` |
| `readBookUpdates()` | book update array | Full depth: each event includes `bids` and `asks` arrays |
| `readBookUpdatesFrom(startTsNs)` | book update array | Same as `readBookUpdates` starting from `startTsNs` |

Record shapes:

- **Trade**: `{ exchangeTsNs, recvTsNs, price, qty, tradeId, symbolId, side }`
- **BBO**: `{ exchangeTsNs, recvTsNs, seq, symbolId, eventType, bidPrice, bidQty, askPrice, askQty }`
- **Book update**: `{ exchangeTsNs, recvTsNs, seq, symbolId, eventType, bids: [{price, qty}, ...], asks: [{price, qty}, ...] }`

`eventType` is `2` for a snapshot, `3` for a delta.

`startTsNs`, `fromNs`, `toNs` accept either a `number` or a `bigint`. Pass
`bigint` for real nanosecond timestamps — JS `number` is float64 and silently
rounds values past 2^53 (e.g. `1765615835519000000` becomes
`1765615835519000064`), which can shift seek boundaries off by one event.

---

## BinaryLogRecorderHook

Built-in `.floxlog` recorder. Plug into a `Runner` via
`runner.setMarketDataRecorder(hook)` and the engine drives `start` /
`stop` for you.

```javascript
const hook = new flox.BinaryLogRecorderHook(
  outputDir, maxSegmentMb /*=256*/, exchangeId /*=0*/, compression /*"none"|"lz4"*/);
hook.addSymbol(symbolId, name, base, quote, pricePrecision, qtyPrecision);

runner.setMarketDataRecorder(hook);
runner.start();
// ... events flow ...
runner.stop();
console.log(hook.stats());
```

| Method | Description |
|--------|-------------|
| `addSymbol(symbolId, name, base, quote, pricePrecision, qtyPrecision)` | Register a symbol in the recording metadata. |
| `flush()` | Flush buffered bytes to disk. |
| `stats()` | Returns `{ tradesWritten, bookUpdatesWritten, bytesWritten, segmentsCreated, errors }` as BigInts. |
| `destroy()` | Release the underlying C-API handle. |

---

## Partitioner

Splits a dataset into partitions for parallel backtesting.

```javascript
const partitioner = new flox.Partitioner(dataDir);
const partitions = partitioner.byTime(numPartitions, warmupNs);
```

All methods return an array of partition objects `{ partitionId, fromNs, toNs, warmupFromNs, estimatedEvents, estimatedBytes }`. Pass `0` as `warmupNs` if no warmup period is needed.

| Method | Description |
|--------|-------------|
| `byTime(numPartitions, warmupNs)` | Split into N equal time slices |
| `byDuration(durationNs, warmupNs)` | Split by fixed duration |
| `byCalendar(unit, warmupNs)` | Split by calendar unit (0=day, 1=week, 2=month) |
| `bySymbol(numPartitions)` | Split by symbol group |
| `perSymbol()` | One partition per symbol |
| `byEventCount(numPartitions)` | Split by event count |
