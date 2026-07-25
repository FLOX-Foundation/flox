# Backtest components

```javascript
const { BacktestRunner, SimulatedExecutor, BacktestResult, Engine, SignalBuilder } = require('@flox-foundation/flox');
```

---

## BacktestRunner

Replays OHLCV data through a strategy. Emitted orders go to a `SimulatedExecutor`; statistics are returned at the end.

```javascript
const bt = new flox.BacktestRunner(registry, feeRate, initialCapital);
bt.setStrategy(strategy);

const stats = bt.runCsv('/path/to/data.csv', 'BTCUSDT');
const stats = bt.runOhlcv(timestampsNs, closePrices, 'BTCUSDT');
```

| Method | Returns | Description |
|--------|---------|-------------|
| `setStrategy(strategy)` | `void` | Attach a strategy |
| `runCsv(path, symbol)` | stats object | Replay a CSV file (timestamp, open, high, low, close, volume) |
| `runOhlcv(timestamps, closes, symbol)` | stats object | Replay raw arrays (timestamps as Float64Array, closes as Float64Array) |
| `runTape(path)` | stats object | Replay a `.floxlog` tape directory — the canonical recording written by `flox tape record` or `scripts/backfill_to_tape.py` |
| `runTapes(paths)` | stats object | Merge N `.floxlog` tape directories on read and replay the merged stream. Symbols are rekeyed by `(metadata.exchange, name)` so two venues stay distinct. `runTapes([t])` equals `runTape(t)`; throws on bad paths or overlapping book streams |
| `setExecutor(executor)` | `void` | Replace the built-in `SimulatedExecutor`. `null` detaches |
| `addExecutionListener(listener)` | `void` | Attach an order-lifecycle listener. Multiple listeners may be attached; each fires for every order event |
| `equityCurve()` | `EquityCurve` | Equity curve from the most recent run |
| `trades()` | `BacktestTrades` | Closed trades from the most recent run |

`equityCurve()` and `trades()` throw `FloxError(E_RUN_002)` if no run has completed.

### Stats object

| Key | Type | Description |
|-----|------|-------------|
| `totalTrades` | `number` | Round-trip trade count |
| `winningTrades` | `number` | Winning trades |
| `losingTrades` | `number` | Losing trades |
| `initialCapital` | `number` | Starting capital |
| `finalCapital` | `number` | Ending capital |
| `netPnl` | `number` | Net P&L after fees |
| `totalPnl` | `number` | Gross P&L |
| `totalFees` | `number` | Total fees paid |
| `grossProfit` | `number` | Sum of winning trades |
| `grossLoss` | `number` | Sum of losing trades |
| `maxDrawdown` | `number` | Max drawdown (absolute) |
| `maxDrawdownPct` | `number` | Max drawdown (%) |
| `winRate` | `number` | Winning trade ratio |
| `profitFactor` | `number` | Gross profit / gross loss |
| `avgWin` | `number` | Average winning trade |
| `avgLoss` | `number` | Average losing trade |
| `sharpe` | `number` | Annualized Sharpe ratio |
| `sortino` | `number` | Sortino ratio |
| `calmar` | `number` | Calmar ratio |
| `returnPct` | `number` | Net return (%) |

---

## SimulatedExecutor

Lower-level backtest executor. Use directly when you need more control over fill logic than `BacktestRunner` provides.

```javascript
const exec = new flox.SimulatedExecutor();
exec.setDefaultSlippage('fixed_bps', 0, 0, 2.0, 0);
exec.setQueueModel('tob', 1);
```

| Method / Property | Description |
|-------------------|-------------|
| `submitOrder(id, side, price, qty, type, symbol, opts?)` | Submit an order (`side`: `"buy"`/`"sell"`) |
| `cancelOrder(orderId)` | Cancel an order |
| `cancelAll(symbol)` | Cancel all orders for a symbol |
| `onBar(symbol, closePrice)` | Feed a bar close |
| `onTrade(symbol, price, isBuy)` | Feed a trade |
| `advanceClock(timestampNs)` | Advance simulated time |
| `setDefaultSlippage(model, ticks, tickSize, bps, impactCoeff)` | Configure slippage. `model` is one of `"none"`, `"fixed_ticks"`, `"fixed_bps"`, `"volume_impact"` |
| `setQueueModel(model, depth)` | Configure limit order queue. `model` is one of `"none"`, `"tob"`, `"full"`, `"pro_rata"`, `"pro_rata_with_fifo"`, `"top_pro_lmm"`, `"pro_rata_with_priority"` |
| `fillCount` | Number of fills (property) |

`submitOrder`'s `type` is one of `"market"`, `"limit"`, `"stop_market"`,
`"stop_limit"`, `"trailing_stop"`. The optional seventh argument carries
`{ tif?: 'gtc' | 'ioc' | 'fok' | 'gtd' | 'post_only', reduceOnly?: boolean, expiresAtNs?: number }`.

---

## BacktestResult

Computes statistics and equity curve from a `SimulatedExecutor`'s fills.

```javascript
const result = new flox.BacktestResult(initialCapital, feeRate);
result.ingestExecutor(exec);
const stats = result.stats();
```

| Method | Returns | Description |
|--------|---------|-------------|
| `recordFill(orderId, symbol, side, price, qty, timestampNs)` | `void` | Record a single fill |
| `ingestExecutor(exec)` | `void` | Drain all fills from a `SimulatedExecutor` |
| `stats()` | stats object | Same fields as `BacktestRunner` stats |

---

## Engine

Bulk backtesting engine. Loads OHLCV data once, then runs strategies against it.

```javascript
const engine = new flox.Engine(registry);
engine.loadCsv('/path/to/data.csv', 'BTCUSDT');
const stats = engine.run(strategy);
```

| Method / Property | Returns | Description |
|-------------------|---------|-------------|
| `loadCsv(path, symbol)` | `void` | Load OHLCV CSV |
| `loadOhlcv(timestamps, opens, highs, lows, closes, volumes, symbol)` | `void` | Load raw OHLCV arrays |
| `resample(intervalSeconds)` | `void` | Resample loaded data |
| `run(strategyOrSignals)` | stats object | Run a strategy or `SignalBuilder` |
| `barCount()` | `number` | Number of bars loaded |
| `ts()` | `Float64Array` | Timestamps |
| `open()` | `Float64Array` | Open prices |
| `high()` | `Float64Array` | High prices |
| `low()` | `Float64Array` | Low prices |
| `close()` | `Float64Array` | Close prices |
| `volume()` | `Float64Array` | Volumes |
| `symbols` | `string[]` | Registered symbol names (property) |

---

## SignalBuilder

Builds a signal array to pass to `engine.run()`.

```javascript
const signals = new flox.SignalBuilder();
signals.buy(i);     // enter long at bar i
signals.sell(i);    // enter short at bar i
const stats = engine.run(signals);
```

| Method / Property | Description |
|-------------------|-------------|
| `buy(index)` | Add a long entry signal at bar `index` |
| `sell(index)` | Add a short entry signal at bar `index` |
| `limitBuy(index, price)` | Limit long entry |
| `limitSell(index, price)` | Limit short entry |
| `clear()` | Clear all signals |
| `length` | Number of signals (property) |
