# __PROJECT_NAME__

A FLOX research scaffold. Generated with `flox new __PROJECT_NAME__`.

## Quickstart

```bash
pip install -r requirements.txt
python main.py
```

`main.py` runs an SMA(10/30) backtest on the bundled
`data/btcusdt_sample.csv` (500 real BTC 1m bars), prints a summary,
and writes an HTML report next to `main.py`:

```
backtest on btcusdt_sample.csv
  return : -1.2103%
  trades : 187  win=66.3%
  sharpe : -4.5746
  max DD : 1.5030%
  net PnL: -121.0296
  report : /path/to/__PROJECT_NAME__/report.html
```

Open `report.html` in a browser for the equity curve, drawdown chart,
and trade table.

To run on your own data, set the env var to a CSV with columns
`timestamp,open,high,low,close,volume`:

```bash
__PROJECT_ENV__=/path/to/btcusdt_1h.csv python main.py
```

## Layout

- `main.py` — single-file entry point. Edit the `__PROJECT_SLUG___strategy`
  class to change indicators and signal logic.
- `main.ipynb` — Jupyter variant of the same strategy, split into cells
  for interactive iteration. Open with `jupyter lab main.ipynb`.
- `requirements.txt` — `flox-py` plus numpy for offline analysis.
- `data/btcusdt_sample.csv` — 500 BTC/USDT 1m bars.
  Replace or delete once you have your own data.

## Next steps

- Swap the SMA crossover for your own indicator stack
  (`flox.SMA`, `flox.EMA`, `flox.ADX`, `flox.ATR`, ...).
- Run the same strategy class against a live exchange via
  `flox_py.ccxt.CcxtBroker`.
- Parameter sweep via `flox.Optimizer`.

See https://flox-foundation.github.io/flox/ for the full reference.
