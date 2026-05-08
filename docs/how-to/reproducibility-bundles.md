# Reproducibility bundles with `flox bundle`

A bundle is a single tarball that encodes everything needed to reproduce a backtest result byte-for-byte on another machine: strategy code, the captured tape it ran against, the engine version, and the expected output. `flox bundle pack` captures a run; `flox bundle validate` proves the next run on the same inputs matches.

## Why this exists

Backtest divergence is the most common bug in algorithmic trading. A strategy passes on your laptop, fails on someone else's machine, or behaves differently after an engine bump. A bundle pins all the inputs (strategy SHA, tape SHA, engine version, slippage config) and the recorded output, so divergence becomes a hard CI failure instead of a slow erosion of trust.

## Layout

```
bundle.tar
├── manifest.json          # versions + integrity hashes + config
├── strategy/
│   └── strategy.py        # the user's strategy module
├── config/
│   └── params.json        # runtime params (slippage, queue model, ...)
├── tape/                  # the W14 tape this run drove against
│   ├── manifest.json
│   └── trades-*.bin
├── expected_output.json   # fill sequence, totals, trade count (legacy JSON summary)
└── expected.floxrun/      # per-run trace: signals, orders, fills (W14-T008)
    ├── manifest.json
    ├── signals-*.bin
    ├── orders-*.bin
    └── fills-*.bin
```

Bundles produced before W14-T008 contain only `expected_output.json`. The replay path falls back to JSON-only diff for those; new bundles ship both, so a reader can do richer signal / order / fill comparisons against the bundled trace.

## Pack

```bash
flox bundle pack \
    --strategy ./my_strategy.py \
    --tape ./tapes/bybit-btc-2026-05-07 \
    --output ./bundles/strat-2026-05-07.tar \
    --slippage-model fixed_bps --slippage-bps 5
```

Pack runs the strategy through the supplied tape with the given config, captures the fill sequence as `expected_output.json`, and writes the bundle. The tarball is reproducible-friendly: same inputs produce a manifest with the same SHAs.

## Replay

```bash
flox bundle replay ./bundles/strat-2026-05-07.tar
```

Extracts the bundle, runs the strategy through the bundled tape with the bundled config, and prints actual versus expected counts. Replay does not assert; for the assertion use `validate`.

## Validate

```bash
flox bundle validate ./bundles/strat-2026-05-07.tar
```

Replay plus a strict comparison against the recorded `expected_output.json`. Exits `0` on match, `1` on divergence with a per-key diff printed to stderr. This is the form to wire into CI.

## What's compared

The validator checks `trade_count`, `fill_count`, the per-fill `(symbol, side, price, quantity)` tuples in order, and `total_filled_quantity`. Order IDs are intentionally excluded because the runner assigns them from a process-wide counter that is not stable between processes.

If you need stricter comparison (per-fill timestamps, intermediate equity curve), open an issue. The current set is the minimum that catches engine-level regressions; extending it is cheap once a real divergence shows up.

## Strategy file rules

The strategy file is a normal Python module. Pack expects exactly one `flox.Strategy` subclass; multiple subclasses fail with a clear error. The class is instantiated with the symbol list pack registered; if your `__init__` does not accept that, pack falls back to a no-arg constructor.

```python
import flox_py as flox


class MyStrategy(flox.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self._fired = False

    def on_trade(self, ctx, trade):
        if self._fired:
            return
        self._fired = True
        self.market_buy(0.01)
```

Strategy state should reset every run (no global side effects, no clock dependencies, no random-without-seed). If your strategy uses `random`, seed it from the trade timestamp or pin a seed in the strategy code.

## What's not handled

- Stop / take-profit / trailing-stop signals fall through to the simulator's no-op path. `SimulatedExecutor.submit_order` accepts `market` and `limit` only. If your strategy emits stops, the bundle still packs but the fills will not match. Design Phase 1 strategies with that constraint, or wait for the follow-up that extends `submit_order`.
- Multi-strategy ensembles. Pack supports one strategy per bundle.
- Network or exchange-stub bundles. The tape is the only input source.

## Versioning

`bundle_format_version` is recorded in the manifest. A reader from a newer flox release reads `v1` bundles; a `v1` reader rejects newer bundles with a clear error rather than parsing them wrong. Migration paths land alongside any future format bump.

## See also

* [Record and replay tapes](tape-record.md). Bundles consume the same `.floxlog` format.
* [Paper trading](paper-trading.md). The same `SimulatedExecutor` powers paper mode and bundle replay.
* [Backtest with realistic fills](backtest-realistic-fills.md). The slippage / queue knobs flow into bundles unchanged.
