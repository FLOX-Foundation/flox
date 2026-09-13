# Strategy, Runner

```javascript
const { SymbolRegistry, Runner } = require('@flox-foundation/flox');
```

---

## SymbolRegistry

```javascript
const registry = new flox.SymbolRegistry();
```

| Method | Returns | Description |
|--------|---------|-------------|
| `addSymbol(exchange, name, tickSize)` | `Symbol` | Register a symbol |
| `symbolCount()` | `number` | Number of registered symbols |

### Symbol

Returned by `addSymbol`. Coerces to a number wherever a symbol ID is expected.

| Property | Type | Description |
|----------|------|-------------|
| `id` | `number` | Numeric symbol ID |
| `name` | `string` | Symbol name |
| `exchange` | `string` | Exchange name |
| `tickSize` | `number` | Tick size |

```javascript
const btc = registry.addSymbol('binance', 'BTCUSDT', 0.01);

btc.id         // 1
btc.name       // "BTCUSDT"
Number(btc)    // 1
btc + 0        // 1
btc.toString() // "Symbol(binance:BTCUSDT, id=1)"
```

---

## Strategy object

A plain JavaScript object with callback properties.

```javascript
const strategy = {
    symbols: [btc],   // array of Symbol or number

    onStart() {},
    onStop() {},

    onTrade(ctx, trade, emit) { ... },
    onBookUpdate(ctx, emit) { ... },
    onBar(ctx, bar, emit) { ... },

    onFill(ctx, ev, emit) { ... },
    onOrderUpdate(ctx, ev, emit) { ... },
    onQueuePositionChange(ctx, ev, emit) { ... },
    onMarketPositionChange(ctx, ev, emit) { ... },
};
```

All callbacks are optional.

### Order-event callbacks

| Callback | Fires on |
|----------|----------|
| `onFill(ctx, ev, emit)` | Every fill this strategy's own orders produce (status `PARTIALLY_FILLED` or `FILLED`) |
| `onOrderUpdate(ctx, ev, emit)` | Every order-lifecycle status change: `NEW` / `ACCEPTED` / `CANCELED` / `REJECTED` / `REPLACED` / `TRIGGERED` / `TRAILING_UPDATED`. Fills included — use `onFill` if you only want those |
| `onQueuePositionChange(ctx, ev, emit)` | A resting limit order's queue position moved with no other lifecycle transition. `ev.queueAhead` / `ev.queueTotal` carry the snapshot. Backtest only |
| `onMarketPositionChange(ctx, ev, emit)` | A resting limit order's categorical market position transitioned (best / behind_best / mid_spread / level_empty / crossed). Backtest only |

`ev` is an `OrderEventData`: `orderId`, `symbolId`, `side`, `orderType`,
`status`, `fillQty`, `fillPrice`, `exchangeTsNs`, `rejectReason`,
`queueAhead`, `queueTotal`, the per-stage timestamps `submittedAtNs` /
`acceptedAtNs` / `firstFillAtNs` / `lastFillAtNs` / `canceledAtNs` /
`rejectedAtNs` / `triggeredAtNs` / `expiredAtNs`, plus `isMaker`,
`fillRole`, `marketPosition` and `distanceToBestTicks`.

### BarData (`bar`)

| Property | Type | Description |
|----------|------|-------------|
| `open`, `high`, `low`, `close` | `number` | OHLC prices |
| `volume`, `buyVolume` | `number` | Total / buy-side volume |
| `startTimeNs`, `endTimeNs` | `number` | Bar window timestamps (nanoseconds) |
| `barType`, `barTypeParam` | `number` | 0=Time, 1=Tick, ... + interval/threshold |
| `closeReason` | `number` | 0=Threshold, 1=Gap, 2=Forced, 3=Warmup. The engine currently emits only `Threshold` (a bar closed the normal way) and `Forced` (flushed by `stop()`); `Gap` is reserved and not yet distinguished from `Threshold`, and `Warmup` is never assigned by the engine itself -- it exists for callers who construct their own `Bar` objects from historical data before calling `BarMatrix::warmup()`. |

### SymbolContext (`ctx`)

| Property | Type | Description |
|----------|------|-------------|
| `position` | `number` | Current position quantity |
| `symbolId` | `number` | Symbol ID |
| `lastTradePrice` | `number` | Last trade price |
| `bestBid` | `number` | Best bid |
| `bestAsk` | `number` | Best ask |
| `midPrice` | `number` | Mid price |

### TradeData (`trade`)

| Property | Type | Description |
|----------|------|-------------|
| `price` | `number` | Trade price |
| `qty` | `number` | Trade quantity |
| `isBuy` | `boolean` | Buy-side aggressor |
| `side` | `string` | `"buy"` or `"sell"` |
| `timestampNs` | `BigInt` | Timestamp (nanoseconds) |

### emit methods

| Method | Description |
|--------|-------------|
| `emit.marketBuy(qty)` | Market buy |
| `emit.marketSell(qty)` | Market sell |
| `emit.limitBuy(price, qty)` | Limit buy |
| `emit.limitSell(price, qty)` | Limit sell |
| `emit.provideLiquidity(priceLower, priceUpper, liquidity)` | Provide AMM liquidity in a price range |
| `emit.withdrawLiquidity(liquidity)` | Withdraw AMM liquidity |
| `emit.cancel(orderId)` | Cancel order |
| `emit.closePosition()` | Close position (reduce-only) |

---

## Runner

Synchronous strategy host. Strategy callbacks fire in the caller's thread before the push call returns.

```javascript
const runner = new flox.Runner(registry, onSignal);        // synchronous
const runner = new flox.Runner(registry, onSignal, true);  // Disruptor background thread
```

In threaded mode, events are published to a lock-free ring buffer and callbacks fire in a background C++ thread.

| Method | Description |
|--------|-------------|
| `addStrategy(strategy)` | Register a strategy object |
| `replaceStrategy(index, strategy)` | Atomically swap the strategy at `index`. The old strategy's `onStop` fires before the swap, the new one's `onStart` after; bus subscriptions, in-flight orders and connections are untouched. Must be invoked on the V8 thread |
| `start()` | Start the runner |
| `stop()` | Stop and clean up |
| `onTrade(symbol, price, qty, isBuy, tsNs)` | Inject a trade tick |
| `onBookSnapshot(symbol, bidPrices, bidQtys, askPrices, askQtys, tsNs)` | Inject an L2 snapshot |
| `onBar(symbol, { open, high, low, close, volume?, ... })` | Inject a closed OHLC bar |

`symbol` accepts a `Symbol` object or a raw number. `tsNs` accepts a number or a `bigint`.
`bidPrices` / `bidQtys` / `askPrices` / `askQtys` are `Float64Array`, one entry per book
level; each price array must be the same length as its matching quantity array.

Exceptions behave differently in the two modes. In sync mode, a throw from
`onTrade` / `onBookSnapshot` / `onBar` propagates straight out of the call,
so a `try`/`catch` around it works normally. In threaded mode there's no
caller waiting on the call (it runs on a libuv-scheduled turn), so Node
just logs a one-time `DEP0168` deprecation warning and drops the
exception; the strategy quietly misses that event. Put the `try`/`catch`
inside the callback itself if a threaded strategy needs to see its own
exceptions.

### Hook setters

Each takes a plain JS object, or `null` to detach.

| Method | Description |
|--------|-------------|
| `setPnlTracker(tracker)` | Attach a PnL tracker |
| `setStorageSink(sink)` | Attach a signal storage sink |
| `setRiskManager(rm)` | Attach a pre-trade risk manager. Sync only — `allow` is read inline; throws when `threaded` |
| `setKillSwitch(ks)` | Attach a kill switch. Sync only |
| `setOrderValidator(ov)` | Attach an order validator. Sync only |
| `setMarketDataRecorder(recorder)` | Attach a `MarketDataRecorderHook` or `BinaryLogRecorderHook` |
| `setExecutor(executor)` | Replace the executor. Sync only — `capabilities()` is read inline |

### Trace recording

| Method | Description |
|--------|-------------|
| `attachTraceRecorder(recorder)` | Auto-capture every signal into a `.floxrun` recorder. Sync mode only; throws otherwise |
| `setTraceFeedTsNs(feedTsNs)` | Stamp every recorded signal with this `feed_ts_ns` until the next call |
| `traceOrderEvent(opts)` | Mirror an order event into the attached recorder. No-op with no recorder attached |
| `traceFill(opts)` | Mirror a fill into the attached recorder |

### Signal callback

```javascript
function onSignal(sig) {
    // sig.side       — "buy" | "sell"
    // sig.quantity
    // sig.price      — 0 for market orders
    // sig.orderType  — "market" | "limit" | "stop_market" | ...
    // sig.orderId
}
```
