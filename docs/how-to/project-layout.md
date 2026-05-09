# Project layout

A flox project has four moving parts: the **strategy** (your decision logic), the **data** (recorded tapes the backtest runs on), the **backtest harness** (the driver that feeds data into the strategy), and the **tests** (regression / sanity checks). The framework ships scaffolding for all four — you do not need to invent a directory structure or a config format.

## Pick a starting template

`flox new` is the canonical scaffolder. Three templates ship with `flox-py`:

| Template | When to pick |
|----------|--------------|
| `research` | Notebooks, parameter sweeps, exploratory backtest. Default. |
| `live` | Strategy that runs on a real exchange (sandbox first, live with explicit env flag). |
| `indicator-library` | Standalone indicator package with its own pyproject.toml and tests. |

```bash
flox new my-project --template=live
```

See [`flox new`](flox-new.md) for placeholder substitution and full CLI surface.

## What you get

The `live` template after `flox new my-project --template=live` produces:

```
my-project/
├── main.py              # entry point: loads config, wires strategy + broker
├── config.py            # config loader (typed)
├── config.toml.example  # template config — copy to config.toml
├── requirements.txt     # flox-py + ccxt[pro]
├── .gitignore
└── README.md
```

The `research` template adds `data/` for sample tapes and `main.ipynb` for exploratory work.

The `indicator-library` template adds `tests/` and a per-indicator file under `<project_slug>/`.

The framework does not prescribe one canonical layout beyond these templates. There is no `flox.toml`, no opinionated dependency-manager picker. If you need a layout that does not match a template, hand-roll one — the engine itself only cares about the strategy class and the `Runner`.

## How the slots compose

The four parts wire together in a fixed order:

1. **Record** — capture market data into a `.floxlog` tape. See [Record and replay market data](tape-record.md). For historical data, the same page covers the `ccxt.fetch_ohlcv` + recorder pattern.
2. **Backtest** — drive the strategy off the recorded tape. See [Backtest](backtest.md).
3. **Trace** — `.floxrun` captures the strategy's signals, orders, and fills during the backtest. See [Strategy trace auto-capture](floxrun-auto-capture.md).
4. **Review** — diff traces, render reports, sweep parameters.

A live deployment swaps step 2 for the `live` template's `main.py`, which feeds the strategy off a CCXT broker instead of a tape. See [CCXT adapter](ccxt-adapter.md).

## What the framework does NOT scaffold

These are deliberately not in any template:

- **A `flox.toml` project file.** The framework does not own your project's metadata. Use `pyproject.toml` (Python) or `package.json` (Node) — both already work.
- **A bundled sample tape.** The templates ship pointers at how to record data, not the data itself. Redistributing exchange data crosses TOS lines.
- **An opinionated stack picker.** `requirements.txt` vs `pyproject.toml` vs `uv` is your choice; the templates assume `pip install -r requirements.txt` because it is the universal floor.

If you find yourself wanting one of these, that is a signal you are stretching the framework beyond what it intends to own.

## See also

- [`flox new`](flox-new.md) — full CLI surface and placeholder rules.
- [Record and replay market data](tape-record.md) — recording and historical backfill.
- [CCXT adapter](ccxt-adapter.md) — the live-feed source for the `live` template.
- [Backtest](backtest.md) — driving a strategy off a recorded tape.
- [Strategy trace auto-capture](floxrun-auto-capture.md) — `.floxrun` records signals/orders/fills.
