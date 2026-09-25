# Bar aggregation

All functions take `(timestamps, prices, quantities, isBuy, param)` where `prices` and
`quantities` are `Float64Array` and `isBuy` is `Uint8Array`. `timestamps` is a
`Float64Array` or a `BigInt64Array` — pass the latter for real wall-clock nanoseconds,
which a double cannot hold exactly. Return an array of bar objects.

`aggregateRenkoBars` can return more bar objects than there were input trades: a trade that
gaps past more than one brick width closes the brick that was forming and synthesizes the
bricks in between (see [bar types](../../explanation/bar-types.md#renko-bars)). Every other
function here closes at most one bar per input trade.

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
| `startTimeNs` | `bigint` | Open time (ns) |
| `endTimeNs` | `bigint` | Close time (ns) |
| `open` | `number` | Open price |
| `high` | `number` | High price |
| `low` | `number` | Low price |
| `close` | `number` | Close price |
| `volume` | `number` | Total volume |
| `buyVolume` | `number` | Buy-side volume |
| `tradeCount` | `number` | Number of trades |
| `closeReason` | `number` | Why the bar closed: `0` threshold, `2` forced, `3` warmup. Every bar this batch path returns closed on its own threshold, so `0` here. |

Only fully closed bars are returned -- a trailing bar still open when the
trade array ends is dropped, so the bar count depends only on the input.
Python, QuickJS, and Codon return the same count on identical input.
