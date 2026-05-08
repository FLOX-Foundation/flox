# Delta-encode L2 books in a tape

A full L2 capture on a busy pair runs into hundreds of MB per minute. Most of those bytes are repeated levels: the bid at 100.05 sits at qty 12 for thirty consecutive snapshots before it moves. A delta encoder skips the repeats and writes only the actual changes.

flox ships `DeltaBookEncoder` and `DeltaBookReplayer` in the C++ engine. The on-disk tape format already carries both event types (`BookSnapshot` and `BookDelta`); the encoder is the state-keeping layer that decides which one to emit at each step. The replayer reverses the process back into reconstructed snapshots.

## How delta encoding works

A delta payload carries only the level entries that changed against the previous state:

- A level with `qty_raw == 0` means "remove this price".
- A level with `qty_raw > 0` means "set this price to this quantity".

A snapshot carries the full sorted level set as before. Anchor snapshots fire on a configurable cadence so a reader can seek to a recent anchor and replay forward without having to start from the beginning of the tape.

## Quick start

=== "Python"

    ```python
    from flox_py import DeltaBookEncoder, DeltaBookReplayer

    enc = DeltaBookEncoder(anchor_every=100)
    rep = DeltaBookReplayer()

    bids = [{"price_raw": 10000, "qty_raw": 10}]
    asks = [{"price_raw": 10001, "qty_raw": 8}]

    out = enc.encode(symbol_id=1, bids=bids, asks=asks)
    # out["is_delta"] = False on the first call, True for unchanged
    # bookkeeping or partial mutations afterwards.
    snap = rep.apply(0 if not out["is_delta"] else 1, 1, out["bids"], out["asks"])
    ```

=== "Node.js"

    ```javascript
    const flox = require('@flox-foundation/flox');

    const enc = new flox.DeltaBookEncoder({ anchorEvery: 100 });
    const rep = new flox.DeltaBookReplayer();

    const bids = [{ priceRaw: 10000, qtyRaw: 10 }];
    const asks = [{ priceRaw: 10001, qtyRaw: 8 }];

    const out = enc.encode(1, bids, asks);
    rep.apply(out.isDelta ? 1 : 0, 1, out.bids, out.asks);
    ```

=== "Codon"

    ```python
    from flox.delta_book import DeltaBookEncoder, DeltaBookReplayer

    enc = DeltaBookEncoder(100)
    rep = DeltaBookReplayer()

    summary = enc.encode(1, [10000], [10], [10001], [8])
    # summary.is_delta tells you which event type the encoder emitted;
    # summary.bid_count / summary.ask_count are the level counts.
    ```

=== "QuickJS"

    ```javascript
    var enc = new flox.DeltaBookEncoder({ anchorEvery: 100 });
    var rep = new flox.DeltaBookReplayer();

    var out = enc.encode(1,
      [{ priceRaw: 10000, qtyRaw: 10 }],
      [{ priceRaw: 10001, qtyRaw: 8 }]);
    rep.apply(out.isDelta ? 1 : 0, 1, out.bids, out.asks);
    ```

## Anchor cadence

`anchor_every=100` means every hundredth `encode()` call returns a full anchor snapshot regardless of diff size. Tune this against your seek-latency tolerance:

- Lower values (10-50) keep seek-from-anywhere fast and recover quickly from a corrupt block, at the cost of less compression.
- Higher values (100-1000) maximize compression for long captures, but a reader that wants the full state at an arbitrary point has to replay further.
- `anchor_every=0` disables delta encoding entirely; every event is a snapshot. The output then matches the pre-delta tape shape byte for byte.

For a typical L2 BTCUSDT capture (50 levels per side, ~10 events per second), `anchor_every=100` cuts tape size by 10-30x compared to snapshot-only.

## Round-trip guarantee

The replayer reconstructs the same level sets the encoder saw. The Python test verifies this on a 50-level book that mutates a few entries per step; both bindings replay the original snapshots back exactly.

If you persist the encoder output to disk via the existing `BinaryLogWriter`, set `BookRecordHeader.type = 0` for snapshots and `1` for deltas. The reader picks up either type transparently.

## When to skip it

- Trade-only tapes. Trades have no shared state to delta against.
- BBO-only captures. Two prices and two quantities per event; the saving is negligible.
- Deep order book streams that change every level every event (rare on real exchanges, but plausible during a flash event). The encoder still emits a delta, but the delta is the same size as a snapshot.

## See also

- [Record and replay tapes](tape-record.md). The CLI surface that consumes the encoder output.
- [Tape format spec](../spec/floxlog.md). The `BookRecordHeader.type` field decides whether a payload is a snapshot or a delta.
