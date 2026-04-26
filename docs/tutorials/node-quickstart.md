# Node.js quickstart

## Requirements

- Node.js 20+

## 1. Install

```bash
npm install @flox-foundation/flox
```

## 2. First indicator

```javascript
const flox = require('@flox-foundation/flox');

const ema = new flox.EMA(20);

const prices = Array.from({ length: 50 }, (_, i) => 100 + i * 0.1);
for (const price of prices) {
    const value = ema.update(price);
    if (value !== null) {
        console.log(`EMA(20): ${value.toFixed(4)}`);
    }
}
```

All streaming indicators return `null` during warmup and the current value once ready. Use `.ready` to check without calling update.

## 3. First strategy

```javascript
const flox = require('@flox-foundation/flox');

const registry = new flox.SymbolRegistry();
const btc = registry.addSymbol('binance', 'BTCUSDT', 0.01);

const fast = new flox.SMA(10);
const slow = new flox.SMA(30);

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

function onSignal(sig) {
    console.log(sig.side, sig.orderType, sig.quantity);
}

const runner = new flox.Runner(registry, onSignal);
runner.addStrategy(strategy);
runner.start();

// Feed market data from your source:
// runner.onTrade(btc, price, qty, isBuy, tsNs)

runner.stop();
```

## 4. Backtest

```javascript
const bt = new flox.BacktestRunner(registry, 0.0004, 10_000);
bt.setStrategy(strategy);

const stats = bt.runCsv('./data/btcusdt_trades.csv', 'BTCUSDT');
console.log(stats.returnPct, stats.sharpeRatio, stats.maxDrawdownPct);
```

---

## Building from source

Use this if you need the current `main` branch before a release is published.

### Requirements

- GCC 14+ or Clang 18+
- CMake 3.22+
- Node.js 20+

```bash
git clone https://github.com/FLOX-Foundation/flox.git
cd flox

cmake -B build \
  -DFLOX_ENABLE_CAPI=ON \
  -DFLOX_ENABLE_BACKTEST=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build

cd node && npm install && npm run build
```

Then require the local build instead of the npm package:

```javascript
const flox = require('./node/build/Release/flox_node');
```

---

## Next steps

- [Indicators reference](../reference/node/indicators.md) — full indicator API
- [Node.js bindings guide](../bindings/node.md) — runner, backtest runner, order types
