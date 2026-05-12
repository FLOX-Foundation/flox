# Merge multiple tapes on read

Each tape is per-exchange. `exchange_id` lives in the manifest,
`recv_ts` comes from one writer process, and failure isolation is
per-venue. To let a strategy see N venues at once, FLOX merges them
*on read*, not on write. `.floxlog` format does not change.

Three consumption paths share the same `MergedTapeReader` primitive:

- [`flox_py.tape.replay_tapes`](#python-replay) is a callback API for
  ad-hoc cross-venue scripts.
- [`flox_py.MergedTapeReader`](#python-direct) has the DataReader
  shape and is the right fit for bulk analytics.
- [`flox tape view path1 path2 …`](#cli-view) renders N tapes
  side-by-side in the replay viewer.

## Symbol identity

The merge keys symbols by `(metadata.exchange, name)`. Two tapes that
recorded the same `(bybit, BTCUSDT)` collapse into one global symbol
id (their trades go into a single time-sorted stream). Two tapes
that recorded `(bybit, BTCUSDT)` and `(binance, BTCUSDT)` get
distinct global ids; the merge keeps them as separate venues.

Same `(exchange, name)` with different `price_precision` /
`qty_precision` is a data-quality error
([E_INPUT_003](../errors/E_INPUT_003.md)). Tapes from older capture
stacks that drifted on precision either get excluded or re-recorded
under a distinct exchange string (`"bybit-legacy"` vs `"bybit"`).

When you record with `BinaryLogRecorderHook`, pass `exchange_name`
explicitly so the merge has something to key on:

=== "Python"
    ```python
    hook = flox.BinaryLogRecorderHook(
        "tapes/bybit-btc",
        exchange_name="bybit",
        instrument_type="perpetual",
    )
    ```

=== "Node.js"
    ```javascript
    const hook = new flox.BinaryLogRecorderHook(
        "tapes/bybit-btc", 256, 0, "none", "bybit", "perpetual");
    ```

If `exchange_name` is empty the merge still runs; it just won't
discriminate venues from each other.

## Tie-breaking

For two events with identical `exchange_ts_ns` the merge breaks ties
by tape order in the `paths` list, then by per-tape internal
sequence. Reproducible, but only if you pass the paths in the same
order across runs. Reorder the list and the tie-broken sub-sequence
flips.

## Trade vs book overlap

The merge handles trades and books differently when two tapes claim
the same `(exchange, name)` and their time ranges intersect:

- Trades are emitted in time order, no dedup. Each writer's view is
  preserved faithfully. If the same trade appeared in both tapes it
  shows up twice; upstream non-overlap or downstream dedup is on the
  caller.
- Books raise [`OverlappingBookStreamError`](../errors/E_INPUT_003.md)
  at `MergedTapeReader` construction. Overlapping book state has no
  defined semantics (whose reset wins?). Either time-slice the
  inputs or pick one.

## <a name="python-replay"></a>Python: `replay_tapes` callback API

```python
from flox_py import tape

def on_trade(ts_ns, global_id, price, qty, side):
    print(f"  trade {ts_ns} sym={global_id} {price:.2f}@{qty}")

def on_book(ts_ns, global_id, is_snapshot, bids, asks):
    typ = "snap" if is_snapshot else "delta"
    print(f"  book  {ts_ns} sym={global_id} {typ} {len(bids)}b/{len(asks)}a")

stats = tape.replay_tapes(
    ["tapes/bybit-btc", "tapes/binance-btc"],
    on_trade=on_trade,
    on_book=on_book,
)

print(f"merged {stats.trades} trades, {stats.books} books across "
      f"{len(stats.tapes)} tapes")
for entry in stats.tapes:
    print(f"  {entry['path']}: {entry['trades']} trades / {entry['books']} books")
```

`stats.tapes` gives a per-tape breakdown. Useful when one of the
inputs is empty (typo in path, recording crashed before any events).

## <a name="python-direct"></a>Python: `MergedTapeReader` direct API

```python
import flox_py

reader = flox_py.MergedTapeReader(
    ["tapes/bybit-btc", "tapes/binance-btc"],
    from_ns=1_700_000_000_000_000_000,   # optional time filter
    to_ns=None,
    symbols=None,                         # optional global-id filter
)

# Symbol table: rekeyed (global_id, exchange, name, precisions).
for s in reader.symbol_table():
    print(s)

# Merged numpy structured arrays. Same dtype as DataReader.
trades = reader.read_trades()
headers, levels = reader.read_books()

# Trades sorted by exchange_ts_ns; symbol_id is the global rekey.
bybit_trades = trades[trades["symbol_id"] == 1]
binance_trades = trades[trades["symbol_id"] == 2]
```

`read_trades` and `read_books` materialise the merged arrays. For
memory-bounded streaming use `replay_tapes` instead.

## Cross-exchange basis backtest example

A two-line strategy that watches the spread between bybit and binance
BTC and prints when it exceeds 5 bps:

```python
import flox_py
from flox_py import tape

last = {1: None, 2: None}      # global_id → last price
def on_trade(ts_ns, sym, price, qty, side):
    last[sym] = price
    a, b = last.get(1), last.get(2)
    if a is not None and b is not None:
        bps = (a - b) / b * 10_000
        if abs(bps) > 5:
            print(f"{ts_ns}: bybit-binance = {bps:+.1f} bps")

tape.replay_tapes(
    ["tapes/bybit-btc", "tapes/binance-btc"],
    on_trade=on_trade,
)
```

For a real backtest, hand the merged stream to a `BacktestRunner`
via `bt.run_tapes([t1, t2])`. Mirrors `bt.run_tape(t)`: single-tape
input round-trips identically.

## <a name="cli-view"></a>CLI: `flox tape view`

```bash
flox tape view tapes/bybit-btc tapes/binance-btc
```

Opens the replay viewer pre-loaded with both tapes merged into one
synthetic stream. Each global symbol gets its own colour on the
price chart.

Under the hood the CLI materialises the merge into a fresh
synthetic `.floxlog` inside the per-session temp dir, rekeyed to
global symbol ids, and points the existing single-tape viewer at it.
This is the v1 path. The follow-up plan is a streaming `/merge`
HTTP endpoint that the SPA reads directly, with a multi-venue legend
and per-venue book chart; the materialisation route is what works
today without SPA-side changes.

Limitations:

- Books for the same `(exchange, name)` must not overlap in time.
- Legend labels show the rekeyed global ids; reading the
  metadata.json the CLI writes alongside the synthetic tape gives
  you the `exchange/name` mapping until the SPA learns to render it.

## See also

- [Spec: floxlog format](../spec/floxlog.md) for the per-tape on-disk
  shape.
- [Record and replay tapes](tape-record.md) for capturing the input
  tapes.
- [`BinaryLogRecorderHook`](../reference/api/replay/market_data_recorder.md):
  the producer side. `exchange_name=` matters for merging.
- Errors: [E_INPUT_002](../errors/E_INPUT_002.md) (missing metadata),
  [E_INPUT_003](../errors/E_INPUT_003.md) (precision mismatch).
