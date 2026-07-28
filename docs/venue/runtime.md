# Runtime and recovery

## Single-writer core

`SequencedShard` runs a symbol's engine as a single-writer state machine behind
the flox `EventBus` (Disruptor). Commands are published to the ingress ring;
one consumer journals each command write-ahead and then applies it. There is no
lock on the matching path and no shared mutable state to race over — the
property TSAN is pointed at in the gate.

```cpp
SequencedShard<LadderBook> shard(cfg, journalPath, book);
shard.submit(InboundCommand{order});   // published to the ring
shard.journaled();                     // records written
```

Symbols are independent: there is no cross-symbol matching in a CLOB, so each
one can run on its own shard. `SymbolRouter` is the dispatch layer for that — it
owns an engine per symbol, routes a command to the right one, and exposes
`shardOf()` for partitioning. Note it dispatches **inline on the calling
thread**: it is symbol-keyed routing, not a thread pool. Running shards
concurrently means driving several `SequencedShard`s, each with its own
consumer.

```cpp
SymbolRouter<MatchingBook> router(/*shards*/ 4);
auto& engine = router.addSymbol(cfg, sink);
router.submit(cmd);                       // routed by symbol
router.snapshotAccount(acct);             // cross-shard view for a reconnecting client
```

## Determinism

Same commands in, same events out, byte for byte. That is what makes replay and
hot standby meaningful, and it is enforced rather than assumed:

- **Every state-mutating input is a command.** Orders, cancels, modifies, mass
  cancels, quotes, last-look decisions — and also the things that are tempting
  to leave as method calls: `SetMark`, `ApplyFunding`, and `AdminCmd` (auction
  transitions, halts, emergency cancel-all). A crash after an opening uncross
  must recover the same book, so the uncross has to be *in the stream*.
- **No order-sensitive decision reads an unordered container.** Anywhere output
  depends on processing order — ADL victim choice, liquidation order, peg
  repricing, expiry, mass cancel — the ids are collected and sorted first, so a
  replica on a different STL or build makes the same choices.
- **`event_hash.h`** folds the outbound stream into a rolling hash, so two runs
  can be compared in one comparison instead of field by field.

Persistent instrument configuration (tick, lot, bands, risk limits) is recovered
from the configuration store, not the WAL, and is therefore deliberately **not**
an `AdminCmd`.

## Journal and replay

```cpp
Journal j(path);
j.append(cmd, tsNs);        // write-ahead: record, then apply
j.flush();

for (const auto& [ts, cmd] : Journal::loadTimed(path)) engine.submit(cmd, ts);
```

Records are `[ts:8][tag:1][struct]`; every command type is trivially copyable
(enforced by `static_assert`), so a record is a tag plus a raw blob. The
sequencer timestamp is stored, so `loadTimed` reproduces time-dependent
behaviour — GTD expiry, last-look windows, MMP windows, LULD pauses — at the
same points it happened live.

Replaying from empty must reconstruct an identical ledger, book, positions, and
reservations. `test_venue_engine` asserts the event-stream hash and the ledger
match after a round trip; `test_venue_venue` covers the auction case, replaying a
journal whose stream contains an `AdminCmd` uncross.

## Clock

Time enters through `IClock`, so the same code runs on simulated and real time:

| Implementation | Use |
|---|---|
| `SimulatedClock` | backtests, deterministic replay |
| `SystemClock` (`flox/util/system_clock.h`) | live |

## Client recovery

- **`ResendBuffer`** keeps per-session outbound events for gap-fill, and
  `GapDetector` tells a client when it missed a sequence.
- **`snapshotAccount`** returns an account's open orders (with the true
  remaining quantity, hidden reserve included — the owner sees its whole order,
  unlike the public feed), pending stops, and position, so a reconnecting client
  can reconcile in one shot rather than replaying everything.
- **Cancel-on-disconnect** optionally pulls a session's resting orders when the
  connection drops.
