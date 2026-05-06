# @flox-foundation/flox

Node.js bindings for the [FLOX](https://github.com/FLOX-Foundation/flox) trading engine. Event-driven live strategy execution and backtesting via a native C++ addon.

Prebuilt binaries included for Linux x64, macOS arm64, Windows x64.

## Install

```bash
npm install @flox-foundation/flox
```

## Quickstart

```javascript
const flox = require('@flox-foundation/flox');

const registry = new flox.SymbolRegistry();
const btc = registry.addSymbol('binance', 'BTCUSDT', 0.01);

const fast = makeSMA(10), slow = makeSMA(30);

const strategy = {
    symbols: [btc],
    onTrade(ctx, trade, emit) {
        const f = fast.update(trade.price);
        const s = slow.update(trade.price);
        if (f === null || s === null) return;
        if (f > s && ctx.position === 0) emit.marketBuy(0.01);
        else if (f < s && ctx.position > 0) emit.closePosition();
    },
};

// Backtest
const bt = new flox.BacktestRunner(registry, 0.0004, 10_000);
bt.setStrategy(strategy);
const stats = bt.runCsv('./data/btcusdt_trades.csv', 'BTCUSDT');
console.log(stats);

// Live
const runner = new flox.Runner(registry, sig => console.log(sig));
runner.addStrategy(strategy);
runner.start();
// runner.onTrade(btc, price, qty, isBuy, tsNs)
// runner.stop()
```

## Backtest analytics

```javascript
// Walk-forward
const wf = new flox.WalkForwardRunner(registry, {
  mode: 'anchored', trainPeriods: 180, testPeriods: 30,
});
const folds = wf.run(strategyFactory, 'data.csv', 'BTCUSDT');

// Grid search
const grid = new flox.GridSearch(registry, params => buildStrategy(params));
grid.addAxis('fast', [5, 10, 20]);
grid.addAxis('slow', [30, 50, 100]);
const results = grid.run('data.csv', 'BTCUSDT');

// HTML heatmap (e.g. for the grid above)
const html = flox.report.heatmapHtml(matrix, {
  rowLabels: ['fast=5', 'fast=10', 'fast=20'],
  colLabels: ['slow=30', 'slow=50', 'slow=100'],
  metricName: 'Sharpe',
});

// White's reality check — significance test for the best of K strategies
const wrc = flox.whitesRealityCheck(flatReturns, K, T, 10000);
```

## Docs

Full API reference: https://flox-foundation.github.io/flox/reference/node/
