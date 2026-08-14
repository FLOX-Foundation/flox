# Market data

The engine emits `OutboundEvent`s about individual orders. A market data feed
is an ordered, sequenced stream that subscribers can apply to rebuild the
book. `MarketDataPublisher` is the bridge between the two.

```cpp
MarketDataPublisher<> md([](const MdMessage& m) { multicast(m); },
                         /*tickSize*/ Price::fromDouble(0.01), /*symbol*/ 1);

MatchingEngine<MatchingBook> venue(cfg, [&](const OutboundEvent& e)
                                   {
                                     md.onEvent(e);   // fan the venue stream into MD
                                     ledgerOrMetrics(e);
                                   });

md.book();   // flox::NLevelOrderBook kept in step with the venue book
```

## Messages

| `MdType` | Meaning | `qty` field |
|---|---|---|
| `AddOrder` | an order joined the book | shown quantity |
| `Trade` | a print (`id` is the taker) | executed quantity |
| `Executed` | a resting order was hit | displayed remaining |
| `Cancel` | an order left the book | — |
| `Replace` | an order was repriced/resized | displayed remaining |
| `Triggered` | a stop fired | — |

Every message carries `(symbol, epoch, seq)`: `seq` is contiguous from 1
within one publisher lifetime, `epoch` is a random value minted when the
publisher is constructed. One publisher serves one symbol; several publishers
may share a multicast group, and the symbol in every root block is what a
consumer demultiplexes on before any gap accounting.

## Sequencing, restarts and epochs

Sequence numbers are deliberately not persisted across restarts. The book is
rebuildable from matching state, so a restarted publisher mints a new epoch
and starts over at `seq=1` instead of carrying seq durability it does not
need. A consumer that sees the epoch change knows every prior seq is void and
re-snapshots; without the epoch, a restarted feed would look like an endless
stream of duplicates to a surviving consumer (a live but empty feed).

## Gap detection (client side)

`GapDetector` (`resend_buffer.h`) is the client-side sequencer. Fed the raw
(possibly reordered, duplicated, gapped) datagram stream, it delivers messages
strictly in seq order per `(symbol, epoch)`:

- A message ahead of the next expected seq is held, not delivered; when the
  missing seq arrives — a late, reordered datagram, not a duplicate — the
  contiguous run drains in order. If held messages exceed `maxHeld`, the gap
  is abandoned and the held run is delivered in seq order rather than lost.
- `onGap(symbol, epoch, fromSeq)` fires when a gap opens and re-fires every
  `reraiseAfter` further messages while it stays unfilled, so a lost resend
  request does not strand the consumer.
- An epoch change drops held state, resets the stream to seq 1 and fires
  `onEpoch` so the consumer can re-snapshot.
- `reset()` fast-forwards a stream after a snapshot was applied.

`UdpMdSubscriber::setGapDetector` wires this into `recv()`: with a detector
attached, `recv()` only surfaces in-order messages; without one it is the raw
decode path, unchanged.

## Snapshot and recovery (TCP)

`MarketDataPublisher::snapshotAtomic()` returns a consistent
`(orders, lastSeq, epoch)` triple — taken under the same lock as the live
emit path, so no increment with `seq <= lastSeq` is missing from the snapshot
and none with `seq > lastSeq` is included. The publisher also keeps a bounded
ring of recent increments (`resendCapacity`, default 1024) for tail replay.

`MdRecoveryServer` (`md_recovery.h`) serves both over TCP with
length-prefixed frames, one request per connection:

- client sends `ResendRequest{symbol, fromSeq}` (`fromSeq=0` = late joiner);
- if `fromSeq` is inside the ring, the server replays the increments with
  `seq >= fromSeq` and closes (EOF terminates the replay);
- otherwise it answers `SnapshotRequired` followed by `SnapshotBegin` +
  `AddOrder` body messages (`seq=0`, epoch set) + `SnapshotEnd{lastSeq}`; the
  consumer applies the body and resumes the incremental feed at `lastSeq+1`.

`MdRecoveryClient` (same header) is the consumer-side counterpart: one
`recover(symbol, fromSeq, out)` call opens the connection, sends the request
and returns the classified reply — `snapshot` flag, the replayed increments or
the snapshot body, and the `lastSeq`/`epoch` resume point (feed
`resetSequencer(symbol, epoch, lastSeq + 1)` to the `GapDetector` and continue
the incremental feed). The socket carries a receive timeout; an unreachable
server is a `ConnectError` status, a stalled reply a `Timeout`, and a
truncated snapshot a `ProtocolError` — never an exception. The low-level
protocol cases in `test_venue_md_recovery` drive it directly (they are the
protocol's documentation); the late-joiner and publisher-restart cases run
through `RecoveringMdSubscriber` below.

`MdCounters` tracks the paths: `resendServed`, `snapshotsServed`, and
`sendDrops` (below).

### Exposing the recovery channel

`MdRecoveryServer::start(port, bindIp)` binds loopback by default
(`bindIp = nullptr`, the historical behaviour). Passing an explicit address
(`"0.0.0.0"`, a NIC address) exposes the channel to other hosts, and an
unparseable address fails the start rather than silently falling back to
loopback.

The hardening contract is the same as for the order-entry gateways: the
recovery protocol carries **no authentication and no encryption** of its own,
so anything reachable off-host must sit behind TLS termination and/or a
firewall / private network, exactly like the control plane. Binding
externally is a deployment decision, not a default.

## Automatic client recovery (`RecoveringMdSubscriber`)

`GapDetector` reports; `MdRecoveryClient` fetches; `RecoveringMdSubscriber`
(`md_recovery.h`) is the two composed into the whole consumer protocol, so a
subscriber implements neither. It wraps a `UdpMdSubscriber` (which must NOT
have its own detector attached — this class owns sequencing) and keeps a
`recv()`-style API: only in-order messages surface, recovery is invisible
except through the `onSnapshot` callback and the counters.

```cpp
UdpMdSubscriber sub;
sub.join(group, port, /*multicast*/ true);

RecoveringMdSubscriber::Config cfg{host, recoveryPort, /*maxGapBeforeSnapshot*/ 256};
RecoveringMdSubscriber rsub(sub, cfg);
rsub.onSnapshot([&](SymbolId sym, const MdSnapshotBegin& b, const std::vector<MdMessage>& orders)
                { book.clear(); for (const auto& m : orders) book.apply(m); });

MdMessage m;
while (rsub.recv(m)) book.apply(m);   // strictly in seq order, holes already recovered
```

- **Gap** → `ResendRequest{fromSeq}`; the replayed increments are woven back
  through the detector, so they fill the hole in seq order and drain whatever
  was held behind it.
- **Epoch change** (publisher restart) or a gap deeper than
  `maxGapBeforeSnapshot` → snapshot recovery (`fromSeq = 0`): the body goes to
  `onSnapshot`, the sequencer fast-forwards to `lastSeq + 1` and the
  incremental feed continues. A deep gap takes this path because the server
  would route it there anyway once the ring has trimmed.
- **Failure** → bounded retries with doubling backoff (`maxAttempts`,
  `initialBackoff`), never an eternal loop. On exhaustion the incident is
  counted in `recoveriesFailed()` and the stream falls back to plain gap
  semantics — the detector re-raises on further traffic and recovery re-arms
  (a test drives exactly that: server down → bounded failure → clean `recv()`
  timeout → server back → the re-raised gap recovers through the ring).
- Counters: `gapsDetected`, `epochChanges`, `resendsRecovered`,
  `snapshotsRecovered`, `recoveriesFailed`.

Single-threaded like the subscriber itself: all recovery work happens inside
`recv()` on the calling thread (the backoff sleeps block the caller, as any
synchronous recovery would).

## Icebergs and the public feed

An iceberg shows a peak and hides the rest. The public feed must only ever see
the peak, or the book can be probed for hidden size by watching what the feed
reports.

That is why `OrderExecuted` carries two quantities:

- `leavesQty`: the whole remaining (peak + hidden), for the owner's execution
  report. You should see your own order in full.
- `displayLeaves`: the displayed peak, for the public feed.

The publisher drives depth from `displayLeaves`. Using the whole remaining
would leak the reserve and, on a partial peak fill, corrupt the level
aggregate.

## Depth is the aggregate, kept in step

The publisher maintains a `flox::NLevelOrderBook`, the same aggregate book
type strategies consume elsewhere in FLOX, so market-data depth and the
venue's internal order-level book stay in agreement. A test drives 300k
commands and periodically compares the publisher's full depth against the
engine's book.

## SBE codec and multicast

`sbe_md_codec.h` encodes `MdMessage` in SBE (Simple Binary Encoding): a
canonical SBE message header (`blockLength`, `templateId`, `schemaId`,
`version`) followed by a per-type little-endian root block. One template per
`MdType`, each carrying only the fields that type needs, plus the recovery
templates (`ResendRequest`, `SnapshotBegin`, `SnapshotEnd`,
`SnapshotRequired`). Schema version 1 appended a trailing `epoch` (u64) to
every incremental template — the SBE-sanctioned extension path: a version-0
reader skips it via the header's `blockLength`, and the decoder accepts v0
frames (epoch reads as 0). The schema in `venue/schema/md-sbe.xml` is the
source of truth; the codec is hand-written against it (no Java codegen in the
build). This is the venue's own SBE schema, not Nasdaq ITCH.

`UdpMdPublisher` (`udp_multicast.h`) publishes frames over IP multicast, with
`UdpMdSubscriber` on the receiving end. Both take an optional interface selector
(`ifaceIp`): the default (`nullptr`) lets the kernel routing table pick the
egress NIC -- the production default so the feed reaches co-located clients on
other hosts; pass `"127.0.0.1"` to pin loopback for same-host use.

The publisher socket is non-blocking and `publish()` checks the `sendto`
result: a datagram that cannot be handed to the kernel (full socket buffer,
closed fd) is a counted drop (`MdCounters::sendDrops`), never a stall of the
matching thread and never a silent loss the venue cannot see. Consumers
recover the hole through gap detection and the recovery channel.

```cpp
std::vector<uint8_t> buf;
SbeMdCodec::encode(msg, buf);                        // header + template block

MdMessage out;
const bool ok = SbeMdCodec::decode(buf.data(), buf.size(), out);  // false if short/foreign
```

The decoder validates the SBE header (rejects a foreign `schemaId`), uses
`blockLength` to skip trailing fields a newer schema version appended (forward
compatible), and bounds-checks before reading. The parser fuzz drives it with
hostile input alongside the FIX / SBE order-entry / REST parsers.

## Tape

`tape_recorder.h` records trade prints only: it filters `Trade` events out of
the outbound stream and persists them through `BinaryLogWriter`, the same
binary format the rest of FLOX reads. The trade tape a venue produces can be
replayed by ordinary FLOX tooling; the non-trade events (adds, cancels,
replaces) are not on the tape.
