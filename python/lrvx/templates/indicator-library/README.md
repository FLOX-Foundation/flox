# __PROJECT_NAME__

A lrvx indicator library. Generated with `lrvx new __PROJECT_NAME__ --template=indicator-library`.

This is a standalone Python package that ships one or more streaming
indicators built on the same contract as the lrvx built-ins
(`SMA`, `EMA`, `RSI`, ...). After publishing to PyPI, anyone can
`pip install __PROJECT_SLUG__` and use the indicators in their own
lrvx strategies.

## Quickstart

```bash
pip install -e ".[dev]"
pytest
python examples/use_in_strategy.py
```

The bundled `ZLEMA` (Zero-Lag EMA) class is a starter — replace or
add to it.

## Layout

- `__PROJECT_SLUG__/` — package source. One module per indicator,
  re-exported from `__init__.py`.
- `__PROJECT_SLUG__/zlema.py` — sample indicator (Zero-Lag EMA).
- `tests/` — pytest suite. Includes a bundled BTC/USDT 1m sample CSV
  for end-to-end checks.
- `examples/use_in_strategy.py` — uses `ZLEMA` inside a `lrvx.Strategy`
  on the bundled CSV.
- `pyproject.toml` — `hatchling` build, `lrvx` runtime dep,
  `[dev]` extras for `pytest`/`ruff`.
- `.github/workflows/ci.yml` — runs lint + tests on push.

## The streaming indicator contract

Every indicator in this package follows the same shape lrvx built-ins
use, so strategies can swap them in without changes:

```python
class MyIndicator:
    def __init__(self, period: int): ...
    def update(self, price: float) -> float | None: ...
    @property
    def ready(self) -> bool: ...
    @property
    def value(self) -> float | None: ...
    def reset(self) -> None: ...
```

`update()` returns `None` while warming up, then a value each tick.
`ready` flips to `True` on the first non-`None` output. `reset()` drops
internal state for re-use across walk-forward folds.

## Adding a new indicator

1. Create `__PROJECT_SLUG__/my_indicator.py` with a class that follows
   the contract above.
2. Re-export from `__PROJECT_SLUG__/__init__.py`:
   ```python
   from .my_indicator import MyIndicator
   __all__ = ["ZLEMA", "MyIndicator"]
   ```
3. Add a `tests/test_my_indicator.py`. The bundled sample CSV is at
   `tests/data/btcusdt_sample.csv` — `tests/test_zlema.py` shows how
   to load it.
4. Optionally register the class under the `lrvx.indicators`
   entry-point group in `pyproject.toml` so `lrvx.list_indicators()`
   can discover it alongside the built-ins:
   ```toml
   [project.entry-points."lrvx.indicators"]
   my_indicator = "__PROJECT_SLUG__.my_indicator:MyIndicator"
   ```

## Using the indicator in a strategy

In any lrvx project that has `__PROJECT_SLUG__` installed:

```python
import lrvx
from __PROJECT_SLUG__ import ZLEMA

class MyStrategy(lrvx.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.fast = ZLEMA(10)
        self.slow = ZLEMA(30)

    def on_trade(self, ctx, trade):
        f = self.fast.update(trade.price)
        s = self.slow.update(trade.price)
        if f is None or s is None:
            return
        if f > s and ctx.is_flat():
            self.market_buy(0.01)
        elif f < s and ctx.is_long():
            self.close_position()
```

## Publishing to PyPI

```bash
pip install build twine
python -m build              # produces dist/*.whl + dist/*.tar.gz
twine upload dist/*          # interactive — you'll be prompted for token
```

Use `twine upload --repository testpypi dist/*` to push to TestPyPI
first; install from there with
`pip install -i https://test.pypi.org/simple/ __PROJECT_SLUG__`
to confirm the package installs cleanly before promoting to real PyPI.

After upload, anyone can:

```bash
pip install __PROJECT_SLUG__
```

and then `from __PROJECT_SLUG__ import ZLEMA` in their own lrvx
project.

## Versioning

`pyproject.toml` carries the version (`0.1.0` to start). Bump it
before each `python -m build` and tag the commit (`git tag v0.1.0`).
For an automated flow, look at `hatch-vcs` or `setuptools-scm` —
both can derive the version from git tags.

See <https://docs.lrvx.dev/> for the full lrvx
reference.
