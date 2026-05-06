# Scaffold a project with `flox new`

`flox-py` ships a small CLI that scaffolds new strategy projects from
bundled templates. It is registered as a console script, so after
`pip install flox-py` you have `flox` on your `PATH`.

## Usage

```bash
flox new my-strategy                      # default template (research)
flox new my-strategy --template=research  # explicit
flox new --here .                         # scaffold into current dir
flox templates                            # list available templates
```

`flox new <name>` creates a directory `<name>/` in the current working
directory and copies the chosen template into it. Two placeholders are
substituted in every text file:

| Placeholder           | Replaced with                                          |
|-----------------------|--------------------------------------------------------|
| `__PROJECT_NAME__`    | The name you passed (verbatim).                        |
| `__PROJECT_SLUG__`    | A snake_case slug derived from the name.               |
| `__PROJECT_PREFIX__`  | The slug, upper-cased — used as a generic env-var prefix. |
| `__PROJECT_ENV__`     | `<PREFIX>_DATA` — used by the research template's CSV-path env var. |

For example, `flox new Hedge-Bot` substitutes `Hedge-Bot`, `hedge_bot`,
`HEDGE_BOT`, and `HEDGE_BOT_DATA` respectively.

Substitution applies to file *contents* and to file/directory *names*.
The `indicator-library` template uses this to ship its package
directory as `__PROJECT_SLUG__/`, which becomes e.g. `hedge_bot/` in
the scaffolded project.

## Templates

### `research` (default)

A single-file SMA(10/30) crossover on BTCUSDT plus an optional
backtest path. After scaffolding:

```bash
cd my-strategy
pip install -r requirements.txt
python main.py
```

Without a CSV the script runs the strategy against a synthetic price
path so the round-trip is short. To run a real backtest, point the
`<SLUG>_DATA` env var at a CSV with columns
`timestamp_ms,price,qty,is_buyer_maker`:

```bash
MY_STRATEGY_DATA=/path/to/btcusdt_1m.csv python main.py
```

The strategy class itself is the same shape as in
[strategy classes](strategy-classes.md), so once you've replaced the
SMA crossover with your own logic, the same file runs identically
under [backtest](backtest.md), [grid search](grid-search.md), and live
data feeds.

### `live`

Live-trading scaffold built on `CcxtBroker`. Ships with:

- `main.py` — strategy + asyncio entry point with graceful SIGINT / SIGTERM shutdown.
- `config.py` — Pydantic config model with validation.
- `config.toml.example` — copy to `config.toml` and edit.
- `requirements.txt` — `flox-py`, `ccxt`, `pydantic`, `tomli` (for Python 3.10).

Defaults are conservative: `dry_run = true`, `sandbox = true`. Orders
are logged but not sent until you set `FLOX_LIVE=1` plus
`<PREFIX>_API_KEY` / `<PREFIX>_SECRET` env vars. See the template's
README for the full sequence and the [CCXT how-to](ccxt-adapter.md)
for the exchange wiring.

```bash
flox new my-bot --template=live
cd my-bot
pip install -r requirements.txt
cp config.toml.example config.toml
python main.py                    # dry-run
FLOX_LIVE=1 python main.py        # live (sandbox by default)
```

### `indicator-library`

Standalone Python package shipping one or more streaming indicators
that other FLOX projects can `pip install`. Ships with:

- `<slug>/` — package directory (e.g. `my_indicators/`). Holds one
  module per indicator and re-exports them from `__init__.py`.
- `<slug>/zlema.py` — sample indicator (Zero-Lag EMA). Replace or add
  to it.
- `tests/` — pytest suite with the same bundled BTC/USDT 1m CSV the
  `research` template uses, so the tests run end-to-end out of the box.
- `examples/use_in_strategy.py` — backtest using the indicator inside
  a `flox.Strategy`.
- `pyproject.toml` — `hatchling` build, `flox-py` runtime dep, `[dev]`
  extras (`pytest`, `ruff`). Includes a `[project.entry-points."flox.indicators"]`
  table so the library can be discovered alongside built-ins.
- `.github/workflows/ci.yml` — runs lint + tests on push (Python 3.10/3.11/3.12).

```bash
flox new my-indicators --template=indicator-library
cd my-indicators
pip install -e ".[dev]"
pytest                           # runs the bundled test suite
python examples/use_in_strategy.py
```

Publish flow (after editing `pyproject.toml` author/URLs and bumping
the version):

```bash
pip install build twine
python -m build                  # produces dist/*.whl + dist/*.tar.gz
twine upload --repository testpypi dist/*   # smoke-test on TestPyPI first
twine upload dist/*              # promote to PyPI
```

Once installed, downstream projects can `from my_indicators import ZLEMA`
and use it inside any `flox.Strategy` exactly like a built-in
`flox.SMA` or `flox.EMA`.

The streaming indicator contract — `update(price) -> float | None`,
`ready`, `value`, `reset()` — matches the FLOX built-ins, so swapping
your indicator into an existing strategy is a single import change.

## Why a CLI scaffolder?

The friction in starting a new strategy is rarely the indicator math —
it's setting up the imports, a SymbolRegistry, a Runner, and a
backtest harness so the first line of strategy code can run. The
scaffolder collapses that to one command, which makes it cheap to
spin up throwaway research notebooks without copy-pasting from a
canonical example each time.
