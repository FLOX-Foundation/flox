# __PROJECT_NAME__

A FLOX research scaffold. Generated with `flox new __PROJECT_NAME__`.

## Quickstart

```bash
pip install -r requirements.txt
python main.py
```

By default `main.py` runs the bundled SMA(10/30) crossover against a
synthetic price path. To run it as a backtest, point it at a CSV with
columns `timestamp_ms,price,qty,is_buyer_maker`:

```bash
__PROJECT_ENV__=/path/to/btcusdt_1m.csv python main.py
```

## Layout

- `main.py` — single-file entry point. Edit the `__PROJECT_SLUG___strategy`
  class to change indicators and signal logic.
- `requirements.txt` — minimal pin: `flox-py` plus numpy for offline
  analysis.

## Next steps

- Swap the SMA crossover for your own indicator stack
  (`flox.SMA`, `flox.EMA`, `flox.ADX`, `flox.ATR`, ...).
- Wire a live data feed via `flox_py.ccxt.CcxtFeed` to drive the same
  strategy class against an exchange WebSocket.
- Run sweeps via `flox.Optimizer` to grid-search parameters.

See https://flox-foundation.github.io/flox/ for the full reference.
