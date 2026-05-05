# __PROJECT_NAME__

A FLOX live-trading scaffold. Generated with `flox new __PROJECT_NAME__ --template=live`.

## Read this first

This template can place real orders on a real exchange. Defaults are
conservative — `dry_run = true` and `sandbox = true` — but they are
**defaults**, not hard guards. Confirm what mode you are in before
running on production credentials. The first WARNING line printed at
start tells you the mode and the exchange.

Recommended path:

1. Verify the strategy on bundled `research` template's backtest
   (or your own backtest) first.
2. Create testnet credentials on your exchange. Export them as
   `__PROJECT_PREFIX___API_KEY` and `__PROJECT_PREFIX___SECRET`.
3. Copy `config.toml.example` to `config.toml`. Keep `dry_run = true`
   and `sandbox = true` for the first run.
4. `FLOX_LIVE=1 python main.py` — this overrides `dry_run` to false
   and connects to the testnet sandbox. Watch the logs.
5. Once you trust the behavior, set `sandbox = false` in
   `config.toml` and supply production credentials.

## Quickstart (dry-run)

```bash
pip install -r requirements.txt
cp config.toml.example config.toml
python main.py
```

Without `FLOX_LIVE=1`, the script logs candidate orders and never
calls the exchange. Useful for paper-trading-style verification.

## Layout

- `main.py` — entry point. Edit the `__PROJECT_SLUG___strategy` class.
- `config.py` — Pydantic config model with validation.
- `config.toml.example` — template. Copy to `config.toml`.
- `requirements.txt` — `flox-py`, `ccxt`, `pydantic`.

## Order types

Strategy emits `market_buy` / `market_sell`. `CcxtBroker` also routes
`limit_*`, `stop_*`, `take_profit_*`, `trailing_stop`, `cancel`,
`cancel_all`, `modify`, `close_position` if the strategy emits them.
See [CCXT how-to](https://flox-foundation.github.io/flox/how-to/ccxt-adapter/)
for the full mapping table.

## Health and observability

Logs are structured: `INFO` lines mark lifecycle events and tick
counts; `WARNING` for live-mode startup and unexpected initial
positions; `WARNING` from the broker on stream errors (then exponential
backoff and retry).

For metrics / traces / alerts, wrap the strategy callbacks with your
existing instrumentation. The strategy class is plain Python so any
APM library that monkeypatches methods works.

## Next steps

- Replace SMA with your own signal logic.
- Add a stop-loss via `self.stop_market(...)` from the strategy.
- For multiple strategies, instantiate `CcxtBroker` once and call
  `broker.add_strategy(...)` more than once.
- Run alongside `research` template — same strategy class can sit
  in both projects.

See https://flox-foundation.github.io/flox/ for the full reference.
