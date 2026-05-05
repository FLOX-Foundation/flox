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

| Placeholder         | Replaced with                           |
|---------------------|-----------------------------------------|
| `__PROJECT_NAME__`  | The name you passed (verbatim).         |
| `__PROJECT_SLUG__`  | A snake_case slug derived from the name. |

For example, `flox new Hedge-Bot` produces files where
`__PROJECT_NAME__` becomes `Hedge-Bot` and `__PROJECT_SLUG__` becomes
`hedge_bot`. The slug is safe to use as a Python identifier, env var
prefix, or directory name.

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

## Why a CLI scaffolder?

The friction in starting a new strategy is rarely the indicator math —
it's setting up the imports, a SymbolRegistry, a Runner, and a
backtest harness so the first line of strategy code can run. The
scaffolder collapses that to one command, which makes it cheap to
spin up throwaway research notebooks without copy-pasting from a
canonical example each time.
