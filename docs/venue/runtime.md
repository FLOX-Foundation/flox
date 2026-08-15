# Runtime and recovery

## Single-writer core

`SequencedShard` runs a symbol's engine as a single-writer state machine behind
the FLOX `EventBus` (Disruptor). Commands are published to the ingress ring;
one consumer journals each command write-ahead and then applies it. All engine
state is owned by that one consumer thread.

```cpp
SequencedShard<LadderBook> shard(cfg, journalPath, book);
shard.submit(InboundCommand{order});   // published to the ring
shard.journaled();                     // records written
```

Symbols are independent (a CLOB has no cross-symbol matching), so each one can
run on its own shard. `SymbolRouter` is the dispatch layer: it owns an engine
per symbol, routes a command to the right one, and exposes `shardOf()` for
partitioning. It dispatches inline on the calling thread; to run shards
concurrently, drive several `SequencedShard`s, each with its own consumer.

```cpp
SymbolRouter<MatchingBook> router(/*shards*/ 4);
auto& engine = router.addSymbol(cfg, sink);
router.submit(cmd);                       // routed by symbol
router.snapshotAccount(acct);             // cross-shard view for a reconnecting client
```

## Determinism

Same commands in, same events out, byte for byte. That is what makes replay
and hot standby work, and the tests enforce it:

- **Every state-mutating input is a command.** Orders, cancels, modifies, mass
  cancels, quotes, last-look decisions, and also `SetMark`, `ApplyFunding`,
  `SetFundingSchedule` (the funding calendar) and `AdminCmd` (auction
  transitions, halts, emergency cancel-all, session open/close). A crash after
  an opening uncross must recover the same book, so the uncross has to be in
  the stream. The same holds for money and configuration: `Deposit` /
  `Withdraw` (balance genesis; a withdraw exceeding `available` is rejected and
  changes nothing) and `ListInstrument` / `SetBands` / `SetTriggerRef`
  (instrument configuration) are commands too. Even time is a command: the shard's idle
  sweeper injects `TimeTick` records so a last-look hold that expires on a
  quiet symbol expires identically on replay.
- **No order-sensitive decision reads an unordered container.** Anywhere output
  depends on processing order (ADL victim choice, liquidation order, peg
  repricing, expiry, mass cancel) the ids are collected and sorted first, so a
  replica on a different STL or build makes the same choices.
- **`event_hash.h`** folds the outbound stream into a rolling hash, so two runs
  can be compared in one comparison.

There is no separate configuration store: the journaled command stream is the
source of truth for instrument configuration as well. Listing, band changes,
trigger-reference switches and halts arrive as `ListInstrument` / `SetBands` /
`SetTriggerRef` / `AdminCmd` records; `InstrumentRegistry::apply` rebuilds the
registry from the same stream the engines replay. Structural knobs the control
plane cannot express (assets, scales, margin parameters, fee schedule) are
startup configuration supplied when a shard is constructed.

## Trading sessions and the funding calendar

The engine owns two pieces of *schedule-shaped* state, and in both cases it
holds the **state and the transitions** while the **calendar stays outside**.
Nothing in the matching path fires on a clock.

| State | Set by | Engine behaviour |
|---|---|---|
| Session open / closed | `AdminCmd{CloseSession \| OpenSession}` (control-plane `session` verb) | while closed, new orders are rejected with `MarketClosed`; the resting book stands and cancels are still accepted |
| Next funding boundary | `SetFundingSchedule{intervalNs, nextFundingNs}` (control-plane verb of the same name) | published on the derivatives feed; advanced by one whole interval on each `ApplyFunding` |

**There is deliberately no session calendar in the engine.** A venue's trading
hours are a product-level configuration — holidays, half days, per-instrument
variations, timezone rules — and none of it belongs in a matching engine that
must stay deterministic and clock-free. The operator (or the control plane
driving it) runs the schedule and sends the command; what the engine guarantees
is that the transition is *sequenced*: journaled, replayed at its exact point in
the stream, hashed into the determinism digest, published to the feed as a
`TradingStatusChanged`, and carried by the checkpoint. The same contract holds
for funding: the engine publishes and advances the calendar, but settles only
when told to with `ApplyFunding`.

`Closed` is not a halt (see [market-data.md](market-data.md)): a close leaves a
halt or auction phase underneath it untouched, so reopening returns to exactly
the state the close interrupted.

## Journal and replay

```cpp
Journal j(path);
j.append(cmd, tsNs);        // write-ahead: record, then apply
j.flush();

for (const auto& [ts, cmd] : Journal::loadTimed(path)) engine.submit(cmd, ts);
```

### Durability

| `Journal::Sync` | Barrier | Survives |
|---|---|---|
| `Off` | none | process crash (the record is in the OS cache) |
| `Full` | `fsync` per record | power loss |
| `Group` | one `fsync` per drained ingress batch | power loss |

`Group` exists because `Full` pays for something it does not need. The barrier
only has to be taken before anyone is TOLD the batch took effect, and the
ingress already arrives in batches -- so `SequencedShard` stages the outbound
events a batch produced and releases them only after the barrier. An outbound
event is a promise, and a promise made before the record behind it is durable
is the promise `Full` exists to keep, broken more cheaply.

The batch edge comes from the bus: a consumer that has drained what was
available emits end-of-batch, which is the moment where waiting longer buys no
more amortisation.

Measured on one shard, an M-series laptop, saturated ingest:

| | throughput | p50 | p99 | p99.9 |
|---|---|---|---|---|
| `Off` | 450k cmd/s | 24us | 42us | 56us |
| `Group` | 451k cmd/s | 58us | 90us | 136us |
| `Full` | 44k cmd/s | 124us | 335us | 563us |

Durability against power loss therefore costs about 35us of median latency and
nothing in throughput, rather than a factor of ten.

One caveat worth stating: on macOS `fsync` does not flush the drive's write
cache (`F_FULLFSYNC` does), so these figures are an upper bound for durability
on that platform and the Linux gap may be larger.

Records are `[ts:8][tag:1][struct]`; every command type is trivially copyable
(enforced by `static_assert`), so a record is a tag plus a raw blob. The
sequencer timestamp is stored, so `loadTimed` reproduces time-dependent
behaviour (GTD expiry, last-look windows, MMP windows, LULD pauses) at the
same points it happened live.

A `SequencedShard` opens its journal in append mode and, on `start()`, replays
whatever the file already holds into the engine before serving traffic
(`recoveredCommands()` reports how much; gate ingress on `ready()`). Each
accepted command is stamped by the shard's injectable clock (system time by
default, strictly monotonic), and the SAME timestamp is journaled and fed to
the engine -- so GTD expiry, last-look and MMP windows, and LULD pauses replay
exactly. Replayed events are not re-published outbound; reconnecting clients
reconcile via snapshots.

Replaying from empty must reconstruct an identical ledger, book, positions,
and reservations -- including balances, which enter the stream as `Deposit`
commands rather than out-of-band seeding. `test_venue_engine` asserts the
event-stream hash and the ledger match after a round trip; `test_venue_venue`
covers the auction case, replaying a journal whose stream contains an
`AdminCmd` uncross; `test_venue_recovery` covers hard process death (fork +
`_exit`), the append-mode restart, timed replay, and genesis replay from an
empty ledger.

## Checkpoint and journal rotation

An unbounded WAL means unbounded restart time. A checkpoint bounds both: it
serializes the engine into a **journal-format snapshot** -- the same
`[ts][tag][len][body][crc]` framing, applied on load through the same engine
paths live traffic uses. There is no second binary format and no second
deserializer to drift; the torn-tail/CRC machinery guards snapshots for free.

Snapshot contents, in canonical order (price levels best-first, FIFO within a
level; everything else sorted by key -- the file is byte-for-byte
deterministic):

- `SnapshotBegin{formatVersion, lastAppliedTs, stateHash, configHash}`.
  `configHash` digests the engine's CONSTRUCTOR configuration (fixed-point
  scales, assets, tick/lot/minQty, last-look window, perp mode, match
  policy); the loader compares it against the recovering engine and rejects
  the snapshot with a clear log on mismatch -- restoring raw fixed-point
  state into an engine built with different scales would silently
  reinterpret every price and quantity;
- instrument config as the **existing** records (`ListInstrument`, `SetBands`,
  `SetTriggerRef`, `SetStpGroup` firm-group STP memberships, `AdminCmd`
  halt/auction/session state -- the session record is written last of the
  three so it restores as the outermost state);
- derivatives funding state as a snapshot-only `RestoreFunding` record: the
  last applied rate and the live funding calendar (`nextFundingNs`,
  `intervalNs`). It is a record rather than three more fields on `SnapshotEnd`
  because `SnapshotEnd` is a strictly-sized journal body -- widening it would
  change its `expectedBodySize` and make every existing snapshot unreadable.
  An engine with no funding state at all writes no such record, and a file
  without one restores rate 0 and no schedule, exactly as before the record
  existed (read compatibility, pinned by a test);
- balances as snapshot-only `RestoreBalance` records, one per account x asset
  carrying the EXACT signed `(available, reserved)` split -- every live
  moment is representable, including a negative wallet mid-liquidation.
  `RestoreReservation` / `RestorePosition` then only rebuild the engine-side
  reservation and position tables (the records still carry the exact live
  amounts: partial fills, held slices and STP interactions make a formula
  re-derivation unfaithful). Snapshots written by format v1 carried `Deposit`
  totals instead; those records still apply (read compatibility) and
  reconstitute `reserved` by re-reservation;
- snapshot-only `Restore*` records: book orders (applied straight to the tail
  of their level, no matching pass -- a crossing restore marks the file
  corrupt), pending stops with their current triggers, peg specs, open
  last-look holds, perp positions, MMP config, MMP sliding-window fills
  (`RestoreMmpFills`, exact -- a maker one fill from its limit is still one
  fill from it after recovery), and the clientOrderId dedup sets in
  fixed-size batches;
- `SnapshotEnd{stateHash, tradeSeq, heldSeq, ...}` -- sequence counters,
  last/mark price, pending timed halt.

`stateHash` is an event-hash-style FNV fold over the same canonical traversal
(book, stops, holds, positions, balances available+reserved, MMP windows,
STP groups, config, session and funding state, sequence counters). The session
and funding terms fold in **only when set**, the same "zero == absent" rule the
balance traversal follows -- an engine that was never closed and never saw a
funding rate or schedule hashes exactly as it did before those fields existed,
which is what lets a snapshot written without them still verify on load. The
loader re-verifies it at
`SnapshotEnd`; a mismatch rejects the generation. Startup wiring
(`setLedger`, `setFeeSchedule`, ladder config) is construction state, not
snapshot state -- re-apply it before `start()`, exactly as for plain journal
replay. STP groups are NOT startup wiring anymore: runtime mutations arrive
as the sequenced `SetStpGroup` command, journal, snapshot and replay like any
other matching-relevant state.

Checkpoint protocol (`SequencedShard`, asynchronous, crash-safe at every
step):

1. At a command boundary on the consumer thread (natural quiescence), CLONE
   the engine state (`cloneForSnapshot`: deep copy of every container, ledger
   by value) and rotate the journal onto `<base>.journal.<ts>` (fsync the
   directory). The pause is O(clone), not O(serialize+fsync); the
   `onCheckpoint` hook (sidecars) also runs here, at the boundary. A
   `fork()`-based copy-on-write snapshot was rejected deliberately: the
   process is multi-threaded, and `fork()` clones only the calling thread
   while a malloc-arena lock held elsewhere deadlocks the child on its first
   allocation. A segment's numeric suffix names its base snapshot: the naming
   convention plus per-record CRC IS the manifest -- no manifest file to tear.
2. On a background thread, serialize the clone to `<base>.snapshot.<ts>.tmp`,
   fsync, rename (atomic publish), then delete generations beyond the
   retention window (`CheckpointConfig::retainGenerations`, default 2). The
   pre-checkpoint single file `<base>` is the oldest generation and is read
   as one during recovery until enough snapshot generations exist. At most
   one publish is in flight: the next checkpoint waits for the previous one
   (no snapshot queue). `checkpointNow()` still returns only once the new
   generation is on disk.

Crash window of the asynchronous publish: between the rotation and the
background rename the disk holds "segment `ts` exists, snapshot `ts` absent
(a torn `.tmp` at most)". Recovery tolerates exactly that: the `.tmp` never
parses as a generation, the previous valid snapshot is chosen and BOTH tail
segments replay after it, reproducing the state; the snapshot only ever
appears atomically via rename (pinned by `test_venue_checkpoint`).

Recovery scans the directory: the newest snapshot that validates end-to-end
(structure + configHash + stateHash, applied into a scratch engine first so a
corrupt file never pollutes the real one) plus every segment at or after it.
An invalid snapshot falls back a generation with a WARN; with no valid
snapshot at all, recovery replays the full retained history from scratch.

Triggers: `SequencedShard::checkpointNow()` (surfaced as the control-plane
`snapshotNow` verb -- deliberately NOT journaled, a snapshot must never be
replay-visible), and an automatic record/byte threshold on the current
segment checked by the idle sweeper (`CheckpointConfig`; automatic
checkpoints therefore require the sweeper armed).

The consumer pause is the clone alone (`lastCheckpointPauseNs()` gauges it);
`test_venue_checkpoint` measures both the clone pause and the old synchronous
serialize time on a 100k-order book rather than guessing (the clone is a
small fraction of the serialize+fsync cost). Snapshot-only `Restore*` records
arriving through live `submit` are dropped and counted
(`droppedSnapshotRecords()`): a client must never be able to "restore" itself
an order or a balance.

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
  remaining quantity, hidden reserve included: the owner sees its whole order,
  unlike the public feed), pending stops, and position, so a reconnecting
  client can reconcile in one shot.
- **Cancel-on-disconnect** optionally pulls a session's resting orders when the
  connection drops.
