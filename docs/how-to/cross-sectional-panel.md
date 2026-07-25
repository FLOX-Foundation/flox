# Build a cross-sectional (T × S) panel from N floxlog tapes

Cross-symbol research (XS momentum, pair stat-arb, rank-based long-short, dispersion) reduces to the same input: an aligned 2D array indexed by `(time bucket, symbol)`. The hand-rolled version is shared bug surface: intersection vs union join logic, NaN handling, symbol-ordering drift between calls, off-by-one in the alignment.

`flox_py.panel` collapses the pattern into one call. The input is a list of symbol names plus either a `tape_root` whose immediate subdirectories are the per-symbol tapes, or an explicit `tape_paths` mapping.

## Helpers

Everything after `symbols` is keyword-only, and `bucket_ns` is required on all three.

| Function | Output |
|---|---|
| `build_close_panel(symbols, *, bucket_ns, tape_root=None, tape_paths=None, align="intersection", t_from=None, t_to=None)` | `Panel(ts, values=(T,S), symbols, mode)` |
| `build_ohlc_panel(symbols, *, bucket_ns, ...)` | `OHLCPanel(ts, open, high, low, close, symbols, mode)` |
| `build_returns_panel(symbols, *, bucket_ns, lookback_n, ...)` | `ReturnsPanel(ts, values=(T,S), symbols, lookback_n, mode)` |

`build_returns_panel` additionally requires `lookback_n` (>= 1). Every result dataclass carries a `mode` field holding the alignment mode it was built with. `OHLCPanel` has no volume: the underlying `OHLCBinAggregator` does not emit it.

## Alignment modes

| Mode | Index | NaN policy |
|---|---|---|
| `intersection` | timestamps present in every tape | none (intersection guarantees a value per cell) |
| `union_nan`    | union of every tape's timestamps | `NaN` for missing cells |
| `union_ffill`  | union of every tape's timestamps | forward-fill from the last known bar for that symbol |

For rank-based XS strategies the right default is `intersection`. A missing bar in any symbol kills the rank for that bar. For long-only momentum or single-symbol baselines, `union_ffill` is usually what you want.

## Example

```python
--8<-- "examples/python_xs_panel.py"
```

The output:

```text
mode=intersection  shape=(4, 3) nans=0
mode=union_nan     shape=(5, 3) nans=1
mode=union_ffill   shape=(5, 3) nans=0
returns lookback_n=2 shape=(4, 3)
```

The third symbol (`SOLUSDT`) deliberately skips one bucket. Intersection drops that bucket from every column. `union_nan` keeps it and surfaces one `NaN`. `union_ffill` keeps it and copies the previous bar's close into the gap.

## Column ordering is your contract

The column order in `values` and the entries in `symbols` mirror the input list verbatim. The helpers never sort. Reverse the input and the columns reverse. This matters because callers typically index columns by position (`values[:, j]`), not by name lookup.

## Time bounds

`t_from` and `t_to` (nanoseconds, half-open `[t_from, t_to)`) clip every per-symbol series before alignment. The clip happens inside the aggregator output so empty rows never reach the alignment join.

## When this is not the right primitive

If the strategy consumes per-trade data (microstructure, queue position), the panel is the wrong abstraction; keep the per-tape `DataReader` + aggregator chain. Panels are for bar-level cross-sectional work, which is where the join boilerplate had actually been a pain point.
