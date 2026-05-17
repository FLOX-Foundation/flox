# Import multi-exchange public archives

`flox_py.archives` collects the per-exchange importers under one namespace. Each submodule exposes the same two entry points and the matching CLI subcommand:

| Exchange | Module | Trade entry point | CLI |
|---|---|---|---|
| Binance | `flox_py.archives.binance` | `aggtrades_to_floxlog` | `flox archive binance ...` |
| Bybit   | `flox_py.archives.bybit`   | `trades_to_floxlog`    | `flox archive bybit ...`   |

OKX, Bitget, and Deribit follow the same shape and ship as separate tasks when a concrete research use case shows up. Adding a new venue is a self-contained module under `flox_py/archives/` that implements `trades_to_floxlog` + `range_to_floxlog` matching the `ArchiveReader` Protocol.

## Bybit

Bybit publishes daily trade ticks at `https://public.bybit.com/` going back two-plus years. The on-disk CSV layout (post-2022):

```text
timestamp, symbol, side, size, price, tickDirection, trdMatchID,
grossValue, homeNotional, foreignNotional
```

`timestamp` is Unix seconds (microsecond decimal precision). `side` is the active flow as `Buy` / `Sell` strings; the importer maps to floxlog's `Side::BUY` / `Side::SELL`. `trdMatchID` is the exchange-assigned trade id, used as the floxlog `trade_id` for append-safe dedup.

### URL layout

- spot: `public.bybit.com/spot/<SYMBOL>/<SYMBOL><YYYY-MM-DD>.csv.gz`
- linear: `public.bybit.com/trading/<SYMBOL>/<SYMBOL><YYYY-MM-DD>.csv.gz`
- inverse: `public.bybit.com/trading/<SYMBOL>/<SYMBOL><YYYY-MM-DD>.csv.gz`

The converter accepts the symbol verbatim; cross-exchange symbol normalisation is a `W5 connectors` concern, not an importer one. Each tape keys its own `(metadata.exchange, name)` so `MergedTapeReader` treats `binance:BTCUSDT` and `bybit:BTCUSDT` as distinct global symbols.

### Example

The script below builds a synthetic Bybit-style gzipped CSV in memory, runs `trades_to_floxlog`, then reads the produced tape back through `DataReader.read_trades`:

```python
--8<-- "examples/python_bybit_archive.py"
```

### Range form

For multi-day backfills, `range_to_floxlog` downloads missing files from the public archive and reuses anything already in the local mirror cache:

```text
flox_py.archives.bybit.range_to_floxlog(
    symbol="BTCUSDT",
    market="linear",
    date_from="2024-01-01",
    date_to="2024-12-31",
    out_tape="/path/floxlog/BTCUSDT_bybit",
    mirror=None,        # default = ~/.flox/archive-cache/bybit
    parallel=4,
    skip_missing=False,
)
```

CLI form:

```bash
flox archive bybit \
  --symbol BTCUSDT --market linear \
  --from 2024-01-01 --to 2024-12-31 \
  --out ./tapes/bybit-linear-BTCUSDT \
  --parallel 4
```

## Shared download cache

All exchange-specific importers share one on-disk cache, rooted by default at `~/.flox/archive-cache/`. Override with `FLOX_ARCHIVE_CACHE=/some/path` (env var) or by passing `mirror=...` to the range form. Each exchange writes under its own subdirectory (`bybit/`, `binance/`, etc.) so the cache layout stays predictable.

The cache has no auto-eviction; it grows monotonically. Wipe it whenever you want — the download path is idempotent and will refetch what is missing.

## Cross-exchange research

Tapes from different exchanges sit side by side in `MergedTapeReader`. The reader assigns one global symbol id per `(exchange, name)` pair, so a strategy or analysis that consumes both `binance:BTCUSDT` and `bybit:BTCUSDT` sees two distinct streams with the right exchange tag on every event.

```text
import flox_py
reader = flox_py.MergedTapeReader([
    "./tapes/binance-um-BTCUSDT",
    "./tapes/bybit-linear-BTCUSDT",
])
trades = reader.read_trades()
sym_table = reader.symbol_table()   # [{global_id, exchange, name, ...}, ...]
```

The `(exchange, name)` keying is the contract: every cross-exchange analysis that wants the right interpretation of an event must hold the exchange tag alongside the symbol id.

## Adding a new exchange

A new exchange importer is one module under `flox_py/archives/<exchange>.py` exposing:

- `trades_to_floxlog(csv_path, out_tape, *, symbol_id, symbol_name, market, ...)` — parse one day's CSV, write trades via `DataWriter`, append-safe by the venue's trade id.
- `range_to_floxlog(symbol, market, date_from, date_to, out_tape, *, mirror, parallel, ...)` — download a date range, hand each day to the single-day function, merge `metadata.json` with the union counters.

Register the new module in `archives/__init__.py`, add a `flox archive <exchange>` subparser in `flox_py/cli.py`, and write a synthetic-fixture test under `python/tests/`. The CLI and tests work generically against the `ArchiveReader` Protocol; no changes elsewhere in the framework are required.

## See also

- [Import Binance public archives](import-binance-archive.md) — trade stream.
- [Import Binance book archives](import-binance-book-archive.md) — bookTicker / bookDepth.
- [Merge multiple tapes on read](multi-tape.md) — `MergedTapeReader` semantics across multiple exchanges.
