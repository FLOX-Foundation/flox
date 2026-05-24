# Node.js Bindings

Event-driven live trading and backtesting from Node.js using `@flox-foundation/flox`.

## Install / Build

```bash
npm install @flox-foundation/flox
# or build from source:
cmake -B build -DFLOX_ENABLE_NODE=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

```javascript
const flox = require('@flox-foundation/flox');
// or, if building from source:
const flox = require('../node');
```

## SymbolRegistry and Symbol

```javascript
const registry = new flox.SymbolRegistry();
const btc = registry.addSymbol('binance', 'BTCUSDT', 0.01);

btc.id        // 1
btc.name      // "BTCUSDT"
btc.exchange  // "binance"
btc.tickSize  // 0.01

Number(btc)   // 1
btc.valueOf() // 1
btc + 0       // 1
btc.toString() // "Symbol(binance:BTCUSDT, id=1)"
// btc works as a number wherever a symbol ID is expected
```

## Strategy

A strategy is a plain JavaScript object with callback properties.

```javascript
const strategy = {
    symbols: [btc],

    onStart() {},
    onStop() {},

    onTrade(ctx, trade, emit) {
        // ctx.position      — current position quantity
        // ctx.symbolId      — symbol ID (int)
        // ctx.lastTradePrice
        // ctx.bestBid
        // ctx.bestAsk
        // ctx.midPrice

        // trade.price
        // trade.qty
        // trade.isBuy       — boolean
        // trade.side        — "buy" | "sell"
        // trade.timestampNs — BigInt nanoseconds

        if (ctx.position === 0) {
            emit.marketBuy(0.01);
        }
    },

    onBookUpdate(ctx, emit) {},
};
```

### emit methods

| Method | Description |
|--------|-------------|
| `emit.marketBuy(qty)` | Market buy |
| `emit.marketSell(qty)` | Market sell |
| `emit.limitBuy(price, qty)` | Limit buy |
| `emit.limitSell(price, qty)` | Limit sell |
| `emit.cancel(orderId)` | Cancel order |
| `emit.closePosition()` | Close position (reduce-only) |

## Runner

```javascript
function onSignal(sig) {
    // sig.side       — "buy" | "sell"
    // sig.quantity
    // sig.price      — 0 for market orders
    // sig.orderType  — "market" | "limit" | ...
    // sig.orderId
}

const runner = new flox.Runner(registry, onSignal);        // synchronous
const runner = new flox.Runner(registry, onSignal, true);  // Disruptor background thread

runner.addStrategy(strategy);
runner.start();

// Inject market data from your feed:
runner.onTrade(btc, price, qty, isBuy, tsNs);
runner.onBookSnapshot(btc, bidPrices, bidQtys, askPrices, askQtys, tsNs);

runner.stop();
```

`btc` in feed methods accepts a `Symbol` object or a raw number.

## Backtest

Two paths. The realistic one is one extra call; pick it by default.

### Realistic (venue stack)

```javascript
const stack = flox.VenueStack.binanceUmFutures(42, 10_000);

const bt = new flox.BacktestRunner(registry, { executor: stack.executor(), account: stack.account() });
bt.setStrategy(strategy);

const stats = bt.runCsv('/path/to/data.csv', 'BTCUSDT');
```

`VenueStack.binanceUmFutures` wires the venue physics in one call — cross-margin account, MM tiers and ADL, the VIP fee schedule (bound to the account so realized notional moves the tier), funding settlement, rate limits, and the venue-availability hook. Other factories: `bybitLinear`, `okxSwap`, `deribit`.

Full pattern and pieces: [Realistic backtest in one call](../how-to/realistic-backtest.md), [Cross-margin accounts](../how-to/cross-margin.md), [Liquidation and ADL](../how-to/liquidation-and-adl.md).

### Bare (flat fee, nothing else)

```javascript
const bt = new flox.BacktestRunner(registry, 0.0004, 10_000);
bt.setStrategy(strategy);

const stats = bt.runCsv('/path/to/data.csv', 'BTCUSDT');
```

Flat fee rate, no funding, no liquidation, no rate limits, no queue position. Good for an indicator sanity check; not enough before live.

### Stats object

| Key | Description |
|-----|-------------|
| `returnPct` | Net return percentage |
| `netPnl` | Net P&L after fees |
| `totalTrades` | Round-trip trade count |
| `winRate` | Winning trade fraction |
| `sharpeRatio` | Annualized Sharpe ratio |
| `maxDrawdownPct` | Peak-to-trough drawdown (%) |

## Paper trading

Same strategy object, live feed, simulated fills. `PaperBroker` wraps the same `SimulatedExecutor` and `Account` you used for the realistic backtest.

```javascript
const broker = new flox.PaperBroker(stack.executor(), stack.account());
const runner = new flox.Runner(registry, broker.onSignal);
runner.addStrategy(strategy);
runner.start();

// Forward trades from your live feed:
// runner.onTrade(btc, price, qty, isBuy, tsNs)
```

See [Paper trading](../how-to/paper-trading.md).

## Live

`CcxtBroker` has the same shape as `PaperBroker` but routes through a [ccxt.pro](https://github.com/ccxt/ccxt) exchange. The strategy object is unchanged.

```javascript
const ccxt = require('ccxt');
const exchange = new ccxt.pro.binanceusdm({ apiKey: '...', secret: '...' });

const broker = new flox.CcxtBroker(exchange, registry);
const runner = new flox.Runner(registry, broker.onSignal);
runner.addStrategy(strategy);
runner.start();
```

One strategy class runs backtest, paper, and live. See [Connect FLOX to a CCXT exchange](../how-to/ccxt-adapter.md).

## Full Example — SMA Crossover

```javascript
const flox = require('@flox-foundation/flox');

const registry = new flox.SymbolRegistry();
const btc = registry.addSymbol('binance', 'BTCUSDT', 0.01);

// Simple SMA helper
function makeSMA(period) {
    const buf = [];
    return {
        update(price) {
            buf.push(price);
            if (buf.length > period) buf.shift();
            if (buf.length < period) return null;
            return buf.reduce((a, b) => a + b, 0) / period;
        },
    };
}

const fast = makeSMA(10);
const slow = makeSMA(30);

const strategy = {
    symbols: [btc],

    onTrade(ctx, trade, emit) {
        const f = fast.update(trade.price);
        const s = slow.update(trade.price);
        if (f === null || s === null) return;

        if (f > s && ctx.position === 0) {
            emit.marketBuy(0.01);
        } else if (f < s && ctx.position > 0) {
            emit.closePosition();
        }
    },
};

// --- Live ---
function onSignal(sig) {
    console.log(sig.side, sig.orderType, sig.quantity);
    // forward to exchange
}

const runner = new flox.Runner(registry, onSignal);
runner.addStrategy(strategy);
runner.start();
// runner.onTrade(btc, price, qty, isBuy, tsNs)  ← from your market data feed
// runner.stop()

// --- Backtest ---
const bt = new flox.BacktestRunner(registry, 0.0004, 10_000);
bt.setStrategy(strategy);
const stats = bt.runCsv('./data/btcusdt_trades.csv', 'BTCUSDT');
console.log(stats);
```
