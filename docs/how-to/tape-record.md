# Record and replay market data with `flox tape`

`flox tape` captures live market data into a `.floxlog` directory and replays it deterministically. The on-disk format is the same one the engine writes during backtests, so a session you record today drives a backtest tomorrow without conversion.

## Install

```bash
pip install "flox-py[ccxt]"
```

The `[ccxt]` extra pulls in `ccxt.pro` for the live feed. If you have your own data source, wire it up to the `MarketDataRecorderHook` API directly and skip the extra.

## Record a session

```bash
flox tape record bybit BTCUSDT --duration 1h --output ./tapes/bybit-btc-2026-05-07
```

Arguments:

| Argument | Purpose |
|----------|---------|
| `bybit` | Exchange id from ccxt (`bybit`, `bitget`, `binance`, etc). |
| `BTCUSDT` | Symbol. Either flat (`BTCUSDT`) or slash form (`BTC/USDT`); both are accepted. |
| `--duration 1h` | How long to record. Suffixes: `s`, `m`, `h`, `d`. Omit to record until Ctrl+C. |
| `--output PATH` | Destination directory. Created if it does not exist. |

The CLI writes a `.floxlog` segment directory at the output path and rotates segments at `--max-segment-mb` (default 256 MB).

When recording stops, you get a one-line summary:

```
trades_written=14523 book_updates_skipped=0 last_event_ns=1714123456789012345
```

`book_updates_skipped` will be non-zero if the source emits L2 updates that the build does not write to disk yet. See [Limitations](#limitations).

## Inspect a tape

```bash
flox tape inspect ./tapes/bybit-btc-2026-05-07
```

Prints trade count, first and last exchange timestamp, and the symbols seen. Useful as a smoke check after a recording session.

## Diff two tapes

```bash
flox tape diff ./tapes/run-a ./tapes/run-b
```

Compares two `.floxlog` directories trade-by-trade on `(exchange_ts_ns, symbol_id, price_raw, qty_raw, side)`. Exits 0 when equal, 1 on divergence, with the first divergent index plus a sample of mismatched rows printed to stderr.

`--ts-tolerance-ns N` lets timestamps drift by up to `N` nanoseconds before flagging. Useful when comparing two live captures that share content but came through different recv paths. `--max-mismatches K` caps how many divergent rows are recorded; the rest are summarized by count. `--json` emits the full diff structure for CI scripting.

The most common reason to run this: the replay-equivalence gate failed and you want to know exactly which trades shifted between the captured reference and what the engine produces today.

The walk runs in the C++ engine; every binding speaks the same surface and returns the same shape:

=== "Python"

    ```python
    from flox_py.tape import diff_tapes

    diff = diff_tapes("./tapes/run-a", "./tapes/run-b",
                     max_mismatches=16, field_tolerance_ns=0)
    if not diff.equal:
        print(f"first divergence at index {diff.first_divergence_index}")
        for m in diff.mismatches[:3]:
            print(m)
    ```

=== "Node.js"

    ```javascript
    const flox = require('@flox-foundation/flox');

    const diff = flox.tapeDiff('./tapes/run-a', './tapes/run-b',
                                { maxMismatches: 16, fieldToleranceNs: 0 });
    if (!diff.equal) {
      console.log(`first divergence at index ${diff.firstDivergenceIndex}`);
      for (const m of diff.mismatches.slice(0, 3)) console.log(m);
    }
    ```

=== "Codon"

    ```python
    from flox.tape_diff import diff_tapes

    summary = diff_tapes("./tapes/run-a", "./tapes/run-b")
    if not summary.equal:
        print("first divergence at index", summary.first_divergence_index)
        print("total mismatches recorded:", summary.mismatch_count)
    ```

    Codon exposes the headline summary. For per-record mismatch
    payloads, use the Python or Node binding.

=== "QuickJS"

    ```javascript
    const diff = flox.tapeDiff("./tapes/run-a", "./tapes/run-b");
    if (!diff.equal) {
      console.log("first divergence at index " + diff.firstDivergenceIndex);
    }
    ```

## Replay a tape

```bash
flox tape replay ./tapes/bybit-btc-2026-05-07 --max-events 100
```

Walks the trades in exchange-timestamp order and prints one line per event. `--max-events` caps output; drop it to print everything. The replay reads the same fixed-point types the engine writes, so prices and quantities round-trip exactly.

For programmatic replay inside a strategy or notebook:

```python
from flox_py.tape import replay_tape

def on_trade(ts_ns, sym_id, price, qty, side):
    print(ts_ns, price, qty, side)

n = replay_tape("./tapes/bybit-btc-2026-05-07", on_trade=on_trade)
print(f"replayed {n} trades")
```

## Use it from a strategy

The recorder is a `MarketDataRecorderHook`. Attach it to any `Runner`, not just the CLI:

```python
import flox_py as flox
from flox_py.tape import make_recorder_hook

registry = flox.SymbolRegistry()
sym = registry.add_symbol("bybit", "BTCUSDT", tick_size=0.01)

recorder = make_recorder_hook("./tapes/run-1", max_segment_mb=64)
runner = flox.Runner(registry, on_signal=lambda sig: None)
runner.set_market_data_recorder(recorder)

# ... feed events through runner.on_trade(...) ...

recorder.close()
print(recorder.stats)
```

`recorder.stats` exposes `trades_written`, `book_updates_skipped`, `started_at_ns`, `last_event_ns`, and an `error` field that is set when the underlying writer rejects a row.

## Historical backfill

`flox tape record` is the live capture path. For historical data — anything you need before the recorder was running — there is no built-in backfill CLI. Two reasons: each exchange exposes a different historical surface (some return trades back N days, some only klines, some neither), and the redistribution rules vary. The framework leaves the choice to the strategy author.

The canonical pattern when you want a tape covering past data:

1. Pull the raw history with `ccxt.fetch_ohlcv` (or `fetch_trades` if the exchange supports it). This is plain ccxt — no flox layer.
2. Feed the rows into a `Runner` whose `MarketDataRecorderHook` writes them to a `.floxlog`. The recorder does not care whether the trades are live or replayed from history; the wire format is the same.

```python
import time, ccxt
import flox_py as flox
from flox_py.tape import make_recorder_hook

ex = ccxt.bybit({"enableRateLimit": True})
since_ms = ex.parse8601("2026-04-01T00:00:00Z")
end_ms = ex.parse8601("2026-04-08T00:00:00Z")

registry = flox.SymbolRegistry()
sym = registry.add_symbol("bybit", "BTCUSDT", tick_size=0.01)
recorder = make_recorder_hook("./tapes/bybit-btc-apr-week-1", max_segment_mb=64)
runner = flox.Runner(registry, on_signal=lambda s: None)
runner.set_market_data_recorder(recorder)

while since_ms < end_ms:
    bars = ex.fetch_ohlcv("BTC/USDT", "1m", since=since_ms, limit=1000)
    if not bars:
        break
    for ts_ms, o, h, l, c, v in bars:
        # one synthetic trade per bar at close price; replace with
        # fetch_trades + per-trade emission for full fidelity if the
        # exchange exposes it.
        runner.on_trade(sym, price=c, qty=v, side=0,
                        exchange_ts_ns=ts_ms * 1_000_000)
    since_ms = bars[-1][0] + 60_000
    time.sleep(ex.rateLimit / 1000)

recorder.close()
print(recorder.stats)
```

Use `fetch_trades` when fidelity matters (every print, with native side and id) and `fetch_ohlcv` when you need a longer window than the exchange exposes for trades. Either way the resulting `.floxlog` plugs into backtest the same way as a live recording.

## Limitations

* The Python `DataWriter` C-API writes trades only. Book snapshots and deltas reach the recorder hook and increment `book_updates_skipped`, but they do not land on disk yet. The C++ `BinaryLogWriter::writeBook` path exists; exposing it through Python is a separate task.
* Trade IDs are written as `0`. Replay deduplicates by `(symbol_id, exchange_ts_ns, price_raw, qty_raw)` instead.
* The format version is `v1`. A reader from a newer flox release reads v1 tapes; a v1 reader does not read newer tapes. Migration paths come with the public format spec.

## Runnable example

A complete record-then-replay round-trip with synthetic trades, no ccxt needed:

```python
--8<-- "examples/python_tape_roundtrip.py"
```

Runs in CI and serves as the regression test for the recorder hook plus
replay reader.

## See also

* [Backtest with realistic fills](backtest-realistic-fills.md). Drive a backtest off a recorded tape.
* [CCXT adapter](ccxt-adapter.md). The live-feed source that `flox tape record` wraps.
