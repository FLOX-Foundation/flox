# Import multi-exchange public archives

`flox_py.archives` collects the per-exchange importers under one namespace. Each submodule exposes the same two entry points and the matching CLI subcommand:

| Exchange | Module | Trade entry point | CLI |
|---|---|---|---|
| Binance | `flox_py.archives.binance` | `aggtrades_to_floxlog` | `flox archive binance ...` |
| Bybit   | `flox_py.archives.bybit`   | `trades_to_floxlog`    | `flox archive bybit ...`   |
| OKX     | `flox_py.archives.okx`     | `trades_to_floxlog`    | `flox archive okx ...`     |
| Bitget  | `flox_py.archives.bitget`  | `trades_to_floxlog`    | `flox archive bitget ...`  |
| Deribit | `flox_py.archives.deribit` | `trades_to_floxlog`    | `flox archive deribit ...` |

Adding a new venue is a self-contained module under `flox_py/archives/` that implements `trades_to_floxlog` + `range_to_floxlog` matching the `ArchiveReader` Protocol.

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

## OKX

OKX publishes daily trade ticks on `www.okx.com/cdn/okex/traderecords/` for spot, swap (perpetual), futures, and options. The on-disk CSV columns:

```text
trade_id, side, size, price, timestamp_ms
```

`trade_id` is an integer exchange-assigned id, used directly as the floxlog `trade_id` for dedup. `side` is the active flow as `buy` / `sell` lowercase, mapped to floxlog's `Side::BUY` / `Side::SELL`.

### URL layout

- spot:    `cdn/okex/traderecords/spot/daily/<YYYYMMDD>/<SYMBOL>-trades-<YYYY-MM-DD>.zip`
- swap:    `cdn/okex/traderecords/swap/daily/<YYYYMMDD>/<SYMBOL>-trades-<YYYY-MM-DD>.zip`
- futures: `cdn/okex/traderecords/futures/daily/<YYYYMMDD>/<SYMBOL>-trades-<YYYY-MM-DD>.zip`
- option:  `cdn/okex/traderecords/option/daily/<YYYYMMDD>/<SYMBOL>-trades-<YYYY-MM-DD>.zip`

Symbol naming follows OKX convention (`BTC-USDT` for spot, `BTC-USDT-SWAP` for perp, `BTC-29MAR24-50000-C` for option-chain instruments). The converter accepts the symbol verbatim; cross-exchange normalisation is out of scope.

### Example

The script below builds a synthetic OKX-style zipped CSV in memory and round-trips it through the converter:

```python
--8<-- "examples/python_okx_archive.py"
```

### CLI

```bash
# Single day from a local CSV / zip
flox archive okx \
  --csv ./BTC-USDT-SWAP-trades-2024-01-15.zip \
  --out ./tapes/okx-swap-BTC-USDT-SWAP \
  --symbol BTC-USDT-SWAP --market swap

# Multi-day range with download
flox archive okx \
  --symbol BTC-USDT-SWAP --market swap \
  --from 2024-01-01 --to 2024-12-31 \
  --out ./tapes/okx-swap-BTC-USDT-SWAP \
  --parallel 4
```

## Bitget

Bitget publishes daily trade ticks on its public archive S3 / CDN mirror. The on-disk CSV columns:

```text
trade_id, price, size, side, timestamp_ms
```

`trade_id` is an integer exchange-assigned id, used directly for append-safe dedup. `side` is the active flow as `buy` / `sell` lowercase. `timestamp_ms` is Unix milliseconds.

Market codes follow Bitget's own API naming: `spot`, `umcbl` (USDT-margined perpetual), `cmcbl` (coin-margined perpetual). The converter accepts them verbatim.

### Example

```python
--8<-- "examples/python_bitget_archive.py"
```

### CLI

```bash
# Single day from a local file
flox archive bitget \
  --csv ./BTCUSDT-trades-2024-01-15.zip \
  --out ./tapes/bitget-umcbl-BTCUSDT \
  --symbol BTCUSDT --market umcbl

# Multi-day range with download
flox archive bitget \
  --symbol BTCUSDT --market umcbl \
  --from 2024-01-01 --to 2024-12-31 \
  --out ./tapes/bitget-umcbl-BTCUSDT \
  --parallel 4
```

Bitget specifically matters for production reproduction: `md_collector` deployments on Singapore default to Bitget feeds for multi-symbol fixtures, so the archive lets researchers re-run the same `(exchange, name)` keying that the live capture used.

## Deribit

Deribit dominates crypto options volume and is the only venue of this set whose public archive carries options trades as a first-class event type. The on-disk CSV columns (post-2022):

```text
trade_id, timestamp_ms, instrument, side, price, amount,
mark_price, iv, index_price
```

`trade_id` is integer, used directly for append-safe dedup. `side` is `buy` / `sell` lowercase. `mark_price`, `iv`, and `index_price` are dropped at read time — the floxlog `TradeRecord` schema does not represent them; keep the source CSV alongside the tape and re-join in numpy when needed.

### Instrument naming

- perpetual: `BTC-PERPETUAL`, `ETH-PERPETUAL`, ...
- future:    `BTC-29MAR24`, `ETH-28JUN24`, ... (date-encoded expiry)
- option:    `BTC-29MAR24-50000-C`, `BTC-29MAR24-50000-P` (date-encoded expiry + strike + C/P)

The converter takes one instrument per tape. Multi-instrument option-chain aggregation (one tape covering every strike at a given expiry) is left as a follow-up — backtests that pin to a specific strike or roll through a known series sequentially are well served by the single-instrument path.

### Example

```python
--8<-- "examples/python_deribit_archive.py"
```

### CLI

```bash
# Single day from a local file
flox archive deribit \
  --csv ./BTC-PERPETUAL-2024-01-15.csv.gz \
  --out ./tapes/deribit-perp-BTC \
  --symbol BTC-PERPETUAL --market perpetual

# Multi-day option-chain instrument
flox archive deribit \
  --symbol BTC-29MAR24-50000-C --market option \
  --from 2024-01-01 --to 2024-03-29 \
  --out ./tapes/deribit-opt-BTC-29MAR24-50000-C \
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
