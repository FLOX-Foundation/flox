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

Every command carries an explicit wire tag (`kWireTag` in `messages.h`). The
tag used to be the alternative's position in the `InboundCommand` variant,
which made the variant's declaration order a format promise kept only by a
comment saying "append only, never reorder". Breaking that rule did not
produce an error: an old journal was re-read as **different commands**, which
is the worst thing a format can do.

The tag travels with the alternative now, so the variant can be rearranged
freely. What is still append-only is the tag space: a tag that has been on
disk may never mean a different command.

Two things follow, and both are checked at compile time:

- the layout fingerprint is folded in **tag order**, and its sizes are derived
  from the variant rather than listed by hand, so rearranging the type does not
  move it and a hand-maintained list cannot fall out of step;
- a record tag this build has no command for is refused with
  `JournalFormatError` rather than ending the read, because stopping there
  hands back a prefix of the history as though it were all of it.


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

### Sizing a shard

A shard carries its ingress and outbound rings **by value**, so the capacity
it is instantiated with is the object's size:

| ingress / outbound | `sizeof(SequencedShard<MatchingBook, ...>)` |
|---|---|
| 65536 / 65536 (default) | 28.1 MiB |
| 4096 / 4096 | 1.8 MiB |
| 1024 / 1024 | 0.45 MiB |

Two consequences follow, and neither is a defect as long as it is chosen.

**A shard is a heap object.** At the default it does not fit in a thread's
stack -- two of them as locals in `main` overflow the stack in the prologue,
before a line of the body runs, which is not a failure anyone reads correctly
the first time. Hold one through `std::unique_ptr`, as the tests do.

**A venue of N instruments is N shards.** At the default that is 2.8 GiB of
ring for a hundred symbols, plus a consumer thread each, whatever the flow
through most of them is.

Capacity does not buy throughput. Saturated ingest on one shard, journal
`Sync::Off` so the ring is the variable rather than the disk:

| capacity | throughput |
|---|---|
| 65536 | 533k cmd/s |
| 4096 | 552k cmd/s |
| 1024 | 546k cmd/s |

The differences are noise; if anything the smaller rings are marginally
quicker, having more of themselves in cache. What capacity buys is **burst
absorption**. The bus back-pressures rather than dropping: a required consumer
that falls behind stalls the publisher at wrap gating, so a ring sized below
the burst makes `submit` wait sooner. Nothing is lost either way -- the
question is only whether a producer is allowed to run ahead.

So: size the ring to the deepest burst an instrument actually sees, not to the
throughput you want. A quiet instrument at 1024 costs a fifth of a percent of
the default's memory and measures the same.

`tests/test_venue_shard_capacity.cpp` prints these sizes on every run and
pins the two things that must not drift: that the default is still far past a
stack frame, and that **the events a shard produces do not depend on its ring
size** -- a venue whose history changed with a buffer could not be replayed.

Records are `[ts:8][stamp:1][tag:1][len:4][body][crc:4]`; every command type is
trivially copyable (enforced by `static_assert`), so a body is a raw blob. The
sequencer timestamp is stored, so `loadTimed` reproduces time-dependent
behaviour (GTD expiry, last-look windows, MMP windows, LULD pauses) at the
same points it happened live.

31 of the 35 command structs would otherwise carry compiler-inserted alignment
padding between or after their named fields (`SnapshotBegin` alone has 4 bytes
between `formatVersion` and `lastAppliedTs`). **Padding bytes are explicit,
zero-initialised.** Every such gap is a named `uint8_t padN_[k]{}` member at
the exact byte offset the implicit padding used to sit at -- sizeof and layout
are unchanged (`journal.h` static_asserts each type's size against its pre-fix
value and, for every type but one, `std::has_unique_object_representations_v`,
which is true exactly when nothing but named fields is left in the object
representation). An explicit field has a default member initializer like any
other field, so ordinary construction zeroes it the same way it zeroes any
other field a caller did not set; nothing runs at journal-write time to
compensate for padding. Two snapshots of the same engine state -- or two
journaled records of the same command -- are byte-for-byte identical, not
merely field-for-field equal, and no stack content leaks into the file. The
loader does not read padding, so this changes nothing about what a file
already on disk means; only newly written padding content moves, never the
format.

### Format version and what is readable

The `stamp` byte carries the format version. A build reads exactly one version,
the one it writes. There is no conversion layer and no plan to add one.

A file in any other version is refused by name, as a `JournalFormatError`
naming the version found and the version expected. Nothing in it is decoded,
and it is not treated as a torn tail: a torn tail is the expected shape of a
crash and the prefix ahead of it is sound, whereas a foreign version means
every byte after the header was laid out by rules this build does not have.

What that means per file:

- a journal segment in a foreign version stops the shard from starting. There
  is no older copy of a segment, so continuing would mean serving traffic on a
  history short by whatever that file held;
- a snapshot in a foreign version is one more generation that does not
  validate: recovery logs the reason and falls back a generation, exactly as it
  does for a torn one.

To move a venue across a format version, drain it and take a snapshot with the
build that wrote the journal, or replay the old files with that build. Bit 7 of
the stamp is always set, which is what lets a file written before versioning
existed be named as version 0 rather than misread.

A scale-checked build (`FLOX_SCALE_CHECKS`, the default without `NDEBUG`) is a
different format under this rule, not a debugging variant of the same one. It
widens `Decimal`, so `sizeof(Price)` goes 8 to 16 and every body holding a price
or a quantity moves its fields. Those layouts carry version 10 and version 9
respectively, so a journal from a debug venue is refused by name in a release
one rather than read at the wrong offsets.

The pair moves by two on every format change, never by one: bumping both by
one would hand the unchecked build the number the checked build just left, and
a checked journal would then pass the version test in an unchecked reader --
the failure the separate numbering exists to prevent.

Earlier pairs are refused, not converted. Versions 1 and 2 were the pair
before order records carried the identifier the submitter gave the order: a
file written by a build without it holds orders whose reports would name
nobody, so it is refused rather than read as though the field had always been
absent.

Versions 9 and 10 were the pair before `Quote` carried `clientOrderId` (7 and 8 were the pair before `FillHeld` named the taker's side). A quote
is one submission that becomes two resting orders (bid and ask), so it is the
one command that already splits into children by design; both legs are
stamped with the same value, so every report on either leg names the quote
the submitter sent rather than a per-leg id it never chose. A file written by
a build without the field holds quote-spawned orders whose reports name
nobody on either leg, so it is refused rather than read as though the field
had always been absent.

Versions 11 and 12 were the pair before `FillHeld` and `FillRejected` carried
the taker's `clientOrderId`. A hold's own id (`heldId`) names the hold, not
the order that caused it, so a submitter whose order was split into
venue-level children -- or simply resting under a name of its own choosing --
had to keep a 37-to-11 map to recognise a hold or its reject on one of them;
the engine has had the taker's name since the hold was created. Nothing
journaled changed size at this bump (`FillHeld`/`FillRejected` are outbound
events, not journaled `InboundCommand` bodies), which is why the fingerprint
did not move either -- but replaying an existing journal through this build
still produces a different exec-report stream than the previous one did (the
hold and its reject now name the taker), so the generation number moves
anyway, the same reasoning as 7/8 and 9/10.

Bumping the version is a deliberate edit, and the build stops you from
forgetting it. The sizes of all 35 journaled command structs are folded into a
compile-time fingerprint next to the version constant; adding a field to any of
them fails that assertion with the reason, instead of surfacing months later as
a length that does not add up during someone's recovery.

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
`[ts][stamp][tag][len][body][crc]` framing, applied on load through the same
engine paths live traffic uses. There is no second binary format and no second
deserializer to drift; the torn-tail, CRC and format-version machinery guards
snapshots for free.

A snapshot therefore carries two version numbers with different jobs. The
stamp on every record says how to read the bytes, and a build reads one value
of it. `SnapshotBegin.formatVersion` says what the records mean -- which
records a snapshot of this generation is expected to contain -- and a mismatch
there names both numbers and discards the generation. Contents version 3 is
the current one: order and held-fill records carry the identifier the
submitter gave the order, so an order restored from a snapshot still reports
under the name its submitter chose.

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
  change the on-disk layout and cost a format-version bump (the compile-time
  fingerprint next to `kRecordVersion` stops the build until it gets one).
  An engine with no funding state at all writes no such record, and a file
  without one restores rate 0 and no schedule, exactly as before the record
  existed (read compatibility, pinned by a test);
- balances as snapshot-only `RestoreBalance` records, one per account x asset
  carrying the EXACT signed `(available, reserved)` split -- every live
  moment is representable, including a negative wallet mid-liquidation.
  `RestoreReservation` / `RestorePosition` then only rebuild the engine-side
  reservation and position tables (the records still carry the exact live
  amounts: partial fills, held slices and STP interactions make a formula
  re-derivation unfaithful). Contents format 1 carried `Deposit` totals here
  instead; a file declaring that version is refused at `SnapshotBegin`, so the
  `Deposit` path survives only for a hand-built file that declares the current
  version and uses the older records;
- snapshot-only `Restore*` records: book orders (applied straight to the tail
  of their level, no matching pass -- a crossing restore marks the file
  corrupt), pending stops with their current triggers, peg specs, open
  last-look holds (both legs exactly as they were named: the hold record
  carries the maker's and the taker's `clientOrderId` and the loader restores
  them, so a hold that resolves after a restart reports under the names its
  submitters chose -- and, since `stateHash` folds a non-zero one in, a loader
  that dropped them would reject its own file), perp positions, MMP config,
  MMP sliding-window fills
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
   one publish is in flight; what the next checkpoint does about that depends
   on who asked (see the lane below). `checkpointNow()` still returns only
   once the new generation is on disk.

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
segment checked by `sweepOnce()` -- the idle sweeper thread when one is armed,
otherwise whoever drives the shard (`CheckpointConfig`).

### A shard with no threads of its own

```cpp
shard.setOwnThreads(false);   // before start(): nothing is spawned
shard.setCheckpointLane(&lane);
while (running)
{
  bool any = false;
  for (auto& s : myShards)
  {
    any |= s->pollOnce();     // matching consumer, then outbound subscribers
    any |= s->sweepOnce();    // hold expiry and the checkpoint threshold
  }
  if (!any) backoff.pause();
}
```

A shard owns three threads by default: the matching consumer, the outbound
subscribers and the idle sweeper. That is the right shape for a shard on a
machine it owns, and an impossible one for a process holding hundreds --
three thousand threads on fourteen cores is a scheduler problem before it is
a trading system. Turned off, the shard spawns nothing and does nothing
unless somebody steps it; everything those threads used to do is a call.

Two rules, and nothing detects a breach of either: one thread steps one shard
at a time (it is the ring's single reader), and a shard with its own threads
must not also be stepped. `flush()`, and therefore `checkpointNow()` and
`stop()`, step for themselves when the shard has no threads -- otherwise they
would wait for a consumer that does not exist.

### Where such a driver sleeps

The loop above ends in `backoff.pause()`, which is a core burning while every
shard is quiet -- the cost of having nowhere to block. Both of a shard's buses
are private, so the driver cannot reach their wait points, and neither would
suit it anyway: a bus's condition variable wakes a consumer of *that* bus, and
this thread steps many shards.

`setWakeSet(&set)` hands one [`WakeSet`](../explanation/disruptor.md#one-wait-point-over-many-buses)
to both of a shard's buses, so a command published into ingress from a gateway
thread and an engine event published outbound both wake the driver.
`hasPending()` is the look `pollOnce()` starts with -- the matching consumer,
then every outbound subscriber -- without the delivery, which is what makes it
usable as the park predicate: that predicate runs under the set's mutex, where
delivering anything would hold up every publisher trying to wake the set.

```cpp
flox::WakeSet set;
for (auto& s : myShards)
{
  s->setOwnThreads(false);
  s->setWakeSet(&set);      // both before start()
  s->start();
}

int64_t nextSweep = venueMonoNs();
while (running)
{
  const int64_t now = venueMonoNs();
  if (now >= nextSweep)
  {
    nextSweep = now + sweepIntervalNs;
    for (auto& s : myShards) s->sweepOnce();
  }
  bool any = false;
  for (auto& s : myShards) any = s->pollOnce() || any;
  if (any) continue;

  // Until somebody submits, or until the sweep is due -- whichever is first.
  set.parkUntil(nextSweep, [&] {
    for (const auto& s : myShards) if (s->hasPending()) return true;
    return false;
  });
}
```

The deadline is the half that makes the sweep affordable: `parkUnless` is
bounded only by the set's 50 ms net, which is an order of magnitude coarser
than the cadences a driver actually runs. `setWakeSet` is before `start()`,
with everything else that fixes the publish path, and a shard nobody points at
a set is the shard as it was.

Measured on 14 cores, one thread over N shards with no cadences
(`bench_venue_shard_wake_set`; the bus-level numbers one layer down are in
[the disruptor notes](../explanation/disruptor.md#one-wait-point-over-many-buses)):

| shards on the thread | idle CPU, backoff | idle CPU, wake set |
|---|---|---|
| 3 | 0.22 core | 0 |
| 16 | 0.20 core | 0 |

The backoff figure is flat in the number of shards because it is one thread
either way -- that is the point of stepping -- and it is the whole thread,
idle, forever. Wake-up, submit to outbound handler entry after 2 ms of quiet,
same run:

| wait | wake-up | worst |
|---|---|---|
| backoff, 3 shards | 15 μs | 73 μs |
| backoff, 16 shards | 16 μs | 53 μs |
| wake set, 3 shards | 96 μs | 482 μs |
| wake set, 16 shards | 113 μs | 402 μs |

Unlike the bus-level comparison, the set is a latency trade here, and the
reason is what runs immediately after the wake-up: this path is a matching
pass plus a journal append, executed on a core that has just been idle, where
the bus-level one is a memcpy into a handler on a core that never stopped.
A driver that needs the tail more than the core keeps the backoff; one holding
hundreds of shards does not have that many cores to give.

### The pause when shards share a thread

A shard that owns a thread pays its pause alone, and that is what the pause
was priced for. A thread driving many shards pays every one of them, and the
automatic trigger is the segment's size -- so equally loaded shards reach it
together and the pauses arrive together. Three things keep that from
multiplying:

- **A lane.** `setCheckpointLane(&lane)` gives a group of shards one
  `CheckpointLane`, held from the start of the pause until the snapshot has
  been written. Two shards on a lane never pause at the same time, and at most
  one snapshot per lane is being written, which also caps the threads the
  publishes spawn. Shards on different lanes are independent; a shard with no
  lane behaves as it always did.
- **Automatic checkpoints skip rather than wait.** If the lane is taken, or
  this shard's own previous snapshot is still being written, the automatic
  checkpoint is skipped and counted (`checkpointsSkippedBusy()`,
  `CheckpointLane::skipped()`); the threshold has not gone away, so the next
  sweep asks again. Waiting would put a disk wait inside the pause, and under
  a shared driver every shard behind this one would wait for that disk too. A
  skipped checkpoint is a snapshot not taken, never a record not written:
  recovery simply replays further. `checkpointNow()` waits instead of
  skipping -- somebody is waiting for the answer.
- **Jittered triggers.** `CheckpointConfig::triggerJitterPct` (0 = off, the
  default) gives each shard its own cut below the configured threshold,
  derived from its symbol and re-rolled after every checkpoint. Deterministic,
  so a venue's checkpoints land where they landed last run.

What to watch: `CheckpointLane::pauseTotalNs()` and `pauseMaxNs()` -- how long
the driver was stopped, by anybody on it, which is the number per-shard gauges
cannot give. A `skipped()` count that keeps climbing on one shard while others
checkpoint fine is a shard being crowded out.

Measured with `bench_venue_checkpoint_lane` (14 cores, one thread feeding all
shards, 5000-order books, one checkpoint round each):

| shards | driver stopped | worst single pause | snapshots | skipped |
|---|---|---|---|---|
| 1, no lane | 1.5 ms | 0.8 ms | 2 | 0 |
| 1, lane | 1.4 ms | 0.8 ms | 2 | 0 |
| 70, no lane | 153.9 ms | 66.8 ms | 70 | 0 |
| 70, lane | 39.8 ms | 19.0 ms | 55 | 274 |

The lane does not make a checkpoint cheaper; it spreads checkpoints in time,
so fewer of them land inside any given window and none of them wait for a disk
inside the pause.

The consumer pause is the clone alone (`lastCheckpointPauseNs()` gauges it,
`checkpointPauseTotalNs()` adds them up);
`test_venue_checkpoint` measures both the clone pause and the old synchronous
serialize time on a 100k-order book rather than guessing (the clone is a
small fraction of the serialize+fsync cost). Snapshot-only `Restore*` records
arriving through live `submit` are dropped and counted
(`droppedSnapshotRecords()`): a client must never be able to "restore" itself
an order or a balance.

## Replaying a window of history

Recovery replays a journal to rebuild an engine. A dispute asks something
narrower and with a deadline: *what happened to account N between t1 and t2*.
`flox-venue/journal_window.h` answers it, and `tools/venue_replay_window.cpp`
is the command line around it.

It applies a snapshot (optional) and then the segments, in order, into a fresh
engine, and returns the events whose sequencer timestamps fall inside the
window -- both ends included -- optionally narrowed to one account.

Two properties are what make the extract usable as evidence rather than as a
plausible story.

**The digest covers the whole replayed stream, not the window.** It is the
same `hashEvent` fold the determinism tests use, so it can be compared against
a live run's. Equal digests say this replay took the path the venue took,
which is the only thing that makes the windowed extract worth anything. A
digest over the window alone would agree with a run that diverged before the
window opened and converged again inside it.

**A foreign format version throws, by name.** Not softened into a partial
answer: an operator settling a dispute must never be handed a short history
that looks complete. This is the same refusal recovery makes, for the same
reason.

An event that names no single account -- a public print names two sides and
belongs to neither -- is attributed to nobody, so an account filter never
returns another party's business.

The instrument parameters must match the engine that wrote the journal. Raw
fixed-point state read back under other scales is silently reinterpreted,
which is why the tool asks for them rather than guessing.

Cost is linear in journal size, and a window at the end of the file pays for
the whole file (there is no index; replay is the only thing that reproduces
state exactly). Measured on an apple-silicon laptop, release build:

| records | segment | replay to the end |
|---|---|---|
| 10 000 | 1.8 MiB | 6.4 ms |
| 100 000 | 17.7 MiB | 55 ms |
| 1 000 000 | 177 MiB | 580 ms |

About 3.3 ms per MiB, ~1.7M records/s. A window early in a segment returns
sooner only in the sense that the events are collected sooner; the replay
still has to finish to produce the digest.

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
