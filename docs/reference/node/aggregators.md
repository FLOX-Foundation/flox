# Bar aggregation

All functions take `(timestamps, prices, quantities, isBuy, param)` where `timestamps`, `prices`, `quantities` are `Float64Array` and `isBuy` is `Uint8Array`. Return an array of bar objects.

| Function | Param | Description |
|----------|-------|-------------|
| `aggregateTimeBars(ts, px, qty, ib, intervalSeconds)` | seconds | Time bars |
| `aggregateTickBars(ts, px, qty, ib, tickCount)` | ticks | Tick bars |
| `aggregateVolumeBars(ts, px, qty, ib, threshold)` | volume | Volume bars |
| `aggregateRangeBars(ts, px, qty, ib, rangeSize)` | price range | Range bars |
| `aggregateRenkoBars(ts, px, qty, ib, brickSize)` | brick size | Renko bars |
| `aggregateHeikinAshiBars(ts, px, qty, ib, intervalSeconds)` | seconds | Heikin-Ashi |

Each returned bar object:

| Key | Type | Description |
|-----|------|-------------|
| `startTimeNs` | `number` | Open time (ns) |
| `endTimeNs` | `number` | Close time (ns) |
| `open` | `number` | Open price |
| `high` | `number` | High price |
| `low` | `number` | Low price |
| `close` | `number` | Close price |
| `volume` | `number` | Total volume |
| `buyVolume` | `number` | Buy-side volume |
| `tradeCount` | `number` | Number of trades |
