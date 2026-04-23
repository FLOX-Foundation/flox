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
```

| Method / Property | Returns | Description |
|-------------------|---------|-------------|
| `count` | `number` | Total event count (property) |
| `summary()` | `{ firstEventNs, lastEventNs, totalEvents, segmentCount, totalBytes, durationSeconds }` | Dataset summary |
| `stats()` | `{ filesRead, eventsRead, tradesRead, bookUpdatesRead, bytesRead, crcErrors }` | Read statistics |
| `readTrades(max?)` | trade record array | Read up to `max` trades (all if omitted) |

Each trade record: `{ exchangeTsNs, recvTsNs, price, qty, tradeId, symbolId, side }`.

---

## DataRecorder

Records live market data to disk.

```javascript
const recorder = new flox.DataRecorder(outputDir, exchangeName?, maxSegmentMb?);
recorder.addSymbol(symbolId, name, base?, quote?, pricePrecision?, qtyPrecision?);
recorder.start();
// feed data...
recorder.stop();
```

| Method / Property | Description |
|-------------------|-------------|
| `addSymbol(symbolId, name, base?, quote?, pricePrecision?, qtyPrecision?)` | Register a symbol for recording |
| `start()` | Start recording |
| `stop()` | Stop recording |
| `flush()` | Flush buffers to disk |
| `isRecording` | `boolean` property |

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
