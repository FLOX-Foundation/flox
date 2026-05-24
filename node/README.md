# @flox-foundation/flox

Node.js bindings for [FLOX](https://github.com/FLOX-Foundation/flox) — an AI-native framework for building trading systems.

AI agents discover the surface and drive it end-to-end through an MCP control plane. One strategy class runs backtest, paper, and live.

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

const fast = new flox.SMA(10);
const slow = new flox.SMA(30);

const strategy = {
    symbols: [Number(btc)],
    onTrade(ctx, trade, emit) {
        const f = fast.update(trade.price);
        const s = slow.update(trade.price);
        if (f === null || s === null) return;
        if (f > s && ctx.position === 0) emit.marketBuy(0.01);
        else if (f < s && ctx.position > 0) emit.closePosition();
    },
    // Order-event hooks fire on the strategy's own emitted orders.
    onFill(ctx, ev, emit) { /* status: 'FILLED' / 'PARTIALLY_FILLED' */ },
    onOrderUpdate(ctx, ev, emit) { /* every status change */ },
};

// Backtest a CSV
const bt = new flox.BacktestRunner(registry, 0.0004, 10_000);
bt.setStrategy(strategy);
const stats = bt.runCsv('./data/btcusdt_trades.csv', 'BTCUSDT');
console.log(stats);

// Or replay a recorded `.floxlog` tape
// bt.runTape('./tape')

// Live runner — feed market data via runner.onTrade(...)
const runner = new flox.Runner(registry, sig => console.log(sig));
runner.addStrategy(strategy);
runner.start();
// runner.onTrade(btc, price, qty, isBuy, tsNs)
runner.stop();
```

## Backtest with the same risk gates as live

The `BacktestRunner` accepts the same pre-trade gate stack as the live `Runner`. Reduce-only / flatten orders bypass the gate by design (so a tightening cap cannot strand a position):

```javascript
bt.setRiskManager(myRiskManager);     // .allow(order) → bool
bt.setKillSwitch(myKillSwitch);       // .check(order) + .isTriggered()
bt.setOrderValidator(myValidator);    // .validate(order) → bool
bt.setPnlTracker(myPnlTracker);       // .onSignal(signal)
```

## Backtest analytics

```javascript
// Walk-forward
const wf = new flox.WalkForwardRunner(registry, 0.0004, 10_000, {
  mode: 'anchored', testSize: 100, step: 100, minTrainSize: 100,
});
wf.setStrategyFactory(foldIndex => makeStrategy());
const folds = wf.runCsv('data.csv', 'BTCUSDT');

// Grid search
const grid = new flox.GridSearch();
grid.addAxis([5, 10, 20]);     // fast period
grid.addAxis([30, 50, 100]);   // slow period
grid.setFactory(params => {
  const [fast, slow] = params;
  const r = new flox.BacktestRunner(registry, 0.0004, 10_000);
  r.setStrategy(buildStrategy(fast, slow));
  return r.runCsv('data.csv', 'BTCUSDT');
});
const results = grid.run();    // [{ index, params, stats }, ...]

// HTML heatmap (e.g. for the grid above)
const html = flox.report.heatmapHtml(matrix, {
  rowLabels: ['fast=5', 'fast=10', 'fast=20'],
  colLabels: ['slow=30', 'slow=50', 'slow=100'],
  metricName: 'Sharpe',
});

// White's reality check — significance test for the best of K strategies
const wrc = flox.whitesRealityCheck(flatReturns, K, T, 10_000);
```

## AI companion

[`flox-mcp`](https://github.com/FLOX-Foundation/flox/tree/main/mcp) is a Model Context Protocol server that gives coding agents (Claude Code, Cursor, Cline) grounded access to indicators, error codes, the C-API surface, and full-text doc search. Install once, point your agent at it, the same project.

```bash
pip install flox-mcp
flox-mcp init           # writes ./.mcp.json for the current project
```

## Docs

Full API reference: https://flox-foundation.github.io/flox/reference/node/
