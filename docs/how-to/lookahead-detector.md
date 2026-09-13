# Detect lookahead bias in a strategy

Lookahead bias is the most common backtest bug in algorithmic trading: a strategy reads tomorrow's price by accident, claims a profit it could never have realized live, and quietly fails the moment it ships. The `flox lint lookahead` tool walks a strategy's AST and flags the obvious patterns. It is a heuristic, not a proof; run it before trusting any backtest, and treat clean output as necessary, not sufficient.

## Quick start

```bash
flox lint lookahead path/to/my_strategy.py
```

Sample output for a clean file:

```
flox lint lookahead: OK (path/to/my_strategy.py); no patterns flagged
```

For a file with a bug:

```
flox lint lookahead: 1 finding(s) in path/to/my_strategy.py
  17:24 [shift_negative] `.shift(-1)` reads from the future. ...
      next_close = df['close'].shift(-1)
```

Add `--json` if you want machine-readable output for CI integration.

## Patterns it catches

### `shift_negative`

`df.shift(-N)` and `Series.shift(-N)` with positive `N` read from the future. Pandas treats negative shift as "look forward". The same applies to numpy's `roll`, which fires under this same rule name, and to any third-party indicator library that adopts the pandas convention.

```python
# bug
df['next_close'] = df['close'].shift(-1)
next_close = np.roll(df['close'], -1)
```

The shift amount is read whether it arrives positionally, as a `periods=` keyword, or as a literal `**{"periods": -1}` unpack. A shift amount that only exists in a variable (`df.shift(-k)`) is not statically resolvable and is not flagged — verify those by hand.

### `forward_index_add`

Index arithmetic that walks forward from the current bar peeks at future rows. The literal can sit on either side of the `+`, and a tuple index (`df.iloc[i + 1, 0]`) is checked the same way. Only reads are flagged — assigning to a forward index (`out[i + 1] = v`) is a write, not a lookahead.

```python
# bug
def on_bar(self, ctx, bar):
    future = df.iloc[i + 1]     # flagged
    ahead  = arr[i + 5]         # flagged
    also   = df.iloc[1 + i]     # flagged (literal on the left)
```

Subtraction (`i - N`) is fine; the lint only fires on a positive integer literal added to the index.

### `forward_slice`

A slice whose upper bound is itself `index + N` walks past the current row the same way `forward_index_add` does, in or out of a per-bar callback.

```python
# bug
future_window = close[i:i+3]
```

### `open_upper_slice_in_callback`

Inside a per-bar callback (`on_trade`, `on_bar`, `on_book`, `signal`, `compute`, `update`, etc), an open-upper slice off a variable lower bound spans every future row.

```python
# bug
def on_bar(self, ctx, bar):
    history_plus_future = bar.history[i:]   # flagged
```

Cap the upper bound at the current index (`bar.history[i - 100:i]`) and the lint stays quiet. A literal lower bound is a different idiom, not a lookahead, and is not flagged: `buf = buf[1:]` (drop the oldest entry) and `last20 = buf[-20:]` (a fixed tail window) both read clean regardless of callback depth.

### `future_attr_name`

Attributes named `next_*`, `future_*`, or `lookahead_*` look like deliberate future references. The lint flags them so you confirm by hand. Some are legitimate (`next_funding_time` is published in advance by the exchange); the warning exists so the read is intentional, not accidental.

## Use it from an AI client

The same logic is exposed as the `validate_strategy_no_lookahead` MCP tool. Paste a strategy into Cursor / Claude Code with `flox-mcp` connected and the agent can call the tool to triage before suggesting changes.

## What it does not catch

The detector is intentionally narrow. It will miss:

- Indirect lookahead through external state (e.g. a global cache populated from a future bar).
- Vectorised computations that read forward without index arithmetic (`np.where(df['close'] > df['close'].rolling(5).mean(), ...)` is fine; specific rolling-window misuse can still leak).
- Boolean masks built from forward-looking data and applied to the current row.
- Function calls that internally peek at future state.
- A shift or index offset that only exists in a variable rather than a literal (`df.shift(-k)`, `arr[i + n]`). The lint resolves literals statically; it does not trace values.

A file the lint cannot read at all — wrong encoding, deleted mid-scan, no read permission — surfaces as a `read_error` finding rather than crashing the process, the same way a syntax error does.

A strategy that needs a hard guarantee should pair this lint with an integration test: run on a tape, then re-run on the same tape with the last K rows truncated, and confirm signals up to bar `T-K` are identical between both runs. The replay-equivalence gate already in CI is the spiritual cousin; both shipping side by side is the cheapest path to high confidence.

## Wiring into CI

```yaml
- name: Lint strategies for lookahead bias
  run: |
    set -e
    for f in strategies/*.py; do
      flox lint lookahead "$f"
    done
```

`flox` is the console script installed with `flox-py`. Without it on
`PATH`, call the module directly: `python3 -m flox_py.cli lint
lookahead "$f"` (`flox_py` itself has no `__main__`).

`--json` lets you aggregate findings across many strategies if you have a fleet. Exit code is `0` when clean, `1` when the lint flags anything, `2` when the file is missing.

## See also

* [Replay-equivalence gate](../explanation/replay-equivalence-gate.md). The harder, slower companion check that proves bytes match across runs.
* [Backtesting](backtest.md). The pipeline this lint protects.
* [Reproducibility bundles](reproducibility-bundles.md). Bundle a strategy plus a tape plus the expected output and the bias becomes visible in the `validate` diff.
