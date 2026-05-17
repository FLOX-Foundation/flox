# Import Binance public archives into a floxlog tape

`data.binance.vision` publishes daily aggregate-trade zip archives for spot, USDT-margined perpetuals, and coin-margined perpetuals, going back more than two years. The archive layout is stable, but the per-row work is fiddly: column ordering, header autoskip, millisecond-to-nanosecond rescale, side mapping, and dedup on re-runs.

`flox_py.archives.binance` wraps the converter in two function calls, plus a `flox archive binance` CLI subcommand. The output is a regular `.floxlog` tape, which means every aggregator, `MergedTapeReader`, the live recorder hook, and the engine itself read it without special-casing.

## Install

```bash
pip install flox-py
```

No extras are required. `--mirror` downloads use `urllib` from the standard library.

## Convert a single day

The simplest form takes one already-downloaded zip and writes a tape directory. The example below is the same script CI runs on every push: it builds a synthetic Binance-style zip in memory, calls `aggtrades_to_floxlog`, then reads the produced tape back through `DataReader`.

```python
--8<-- "examples/python_binance_archive.py"
```

`csv_path` accepts either the zip published by Binance or the extracted CSV; the reader autoskips a header row when one is present.

Side encoding follows the floxlog convention. `is_buyer_maker = true` becomes `Side::SELL` (the buyer was the resting maker, so the active flow is a seller hitting the bid). `is_buyer_maker = false` becomes `Side::BUY`.

## Convert a date range

For multi-day backfills, `range_to_floxlog` accepts a date range and optionally a local mirror cache. Missing zips are downloaded from `data.binance.vision` in parallel and reused on follow-up calls. The function signature:

```text
binance.range_to_floxlog(
    symbol: str,
    market: str,                # "spot" | "um-futures" | "cm-futures"
    date_from: str | date,      # YYYY-MM-DD, inclusive
    date_to:   str | date,      # YYYY-MM-DD, inclusive
    out_tape:  str | Path,
    *,
    mirror:    str | Path | None = None,
    parallel:  int = 4,
    symbol_id: int = 1,
    skip_missing: bool = False,
    ...
) -> ConvertStats
```

Without `mirror`, the converter writes to a tempdir and discards it after the range is done. Set `skip_missing=True` to keep going if Binance has not published a particular day yet (common at the head of the archive).

## CLI

`flox archive binance` is the same surface from the shell. For a date range:

```bash
flox archive binance \
  --symbol BTCUSDT \
  --market um-futures \
  --from 2024-01-01 --to 2024-01-31 \
  --out ./tapes/binance-um-BTCUSDT \
  --mirror ./.cache/binance \
  --parallel 4
```

For one-off conversions of a file already on disk, pass `--csv` instead of the date range:

```bash
flox archive binance \
  --csv ./BTCUSDT-aggTrades-2024-01-15.zip \
  --out ./tapes/binance-um-BTCUSDT \
  --symbol BTCUSDT --market um-futures --symbol-id 1
```

## Append-safe and idempotent

The converter dedups on `agg_trade_id` against any trades already in the target tape. Re-running the same day, or running an overlapping range, is a no-op: every previously-imported row is skipped, and the writer adds zero new records. The reported `rows_skipped` counter shows how many rows were elided.

## metadata.json

The tape's `metadata.json` is created (or merged into) on every successful conversion. `MergedTapeReader` keys symbols by `(metadata.exchange, name)`, so the Binance archive tapes line up against tapes captured live by the recorder hook. The symbol IDs picked here do not need to match what the live recorder picked; the reader rekeys both into a global ID space at read time.

## What is and is not in the archive

aggTrades only carries print events. There is no book information. The archive that does carry it (`depth20` and `T1` / `bookTicker`) is tracked separately, so the produced tapes can later be combined into a single floxlog directory with both trades and book updates.

Other public archives (Bybit, OKX, Bitget) follow the same pattern but ship as separate subcommands under `flox archive`; see the multi-exchange reader task in the roadmap.
