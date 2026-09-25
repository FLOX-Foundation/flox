# Perimeter: gateways, protocols, control plane

The perimeter is where untrusted bytes become `InboundCommand`s.

## Gateways

| Gateway | Transport |
|---|---|
| `TcpGateway` | length-prefixed frames over TCP |
| `WsGateway` | RFC 6455 WebSocket (handshake + frames) |
| `TlsGateway` | TLS termination via OpenSSL (optional dependency) |
| `UdpMdPublisher` / `UdpMdSubscriber` | outbound market data over IP multicast |

```cpp
TcpGateway gw([](const uint8_t* p, size_t n) { return SbeOrderEntryCodec::decode(p, n); });
gw.start(port, [&](const InboundCommand& cmd, const Responder& respond) {
  venue.submit(cmd);
});
gw.stop();
```

`SocketAcceptor` carries the shared accept machinery. Each connection runs its
loop inside a `try/catch`: a decode or allocation failure drops that one
connection and the process keeps running.

## Sessions

`GatewaySession` sits between the socket and the engine:

- **Authentication.** API-key HMAC logon (`apiKey:timestamp` signed with the
  shared secret), constant-time comparison, timestamp-skew window.
- **Account binding.** A session is bound to a SET of accounts
  (`GatewaySession`'s vector constructor, `bindAccounts`, or `setAccounts` on
  a gateway), identity first. The three rules:

    | the command names | what happens |
    |---|---|
    | no account (`0`) | stamped with the session's identity |
    | one of the session's accounts | kept |
    | anything else | refused, `Unauthenticated` |

    Account `0` as the session's identity is the explicit "unbound / trusted
    transport" sentinel and passes everything through.

    The set exists because the ordinary shape of a client bridge is one
    connection carrying many customers, each with an account of its own.
    Binding a connection to exactly one meant the bridge got reports for that
    one and nothing for the rest; a product on top had to re-derive the
    accounts of every event and fan them out itself.

    The refusal replaces a silent overwrite. Forcing the session's own account
    onto a foreign id was only ever possible because there was exactly one
    account to force, and it was never honest even then: an order aimed at the
    wrong account was quietly placed on a different one and the client was
    told nothing.

    The check reads the account field by the same walk over the command
    variant that stamps it, so a command added later is covered from the day
    it exists. The hand-written list this replaced covered the six order-flow
    commands and missed the ones that move money, close positions and set
    another account's entitlements.
- **Rate limiting**, per session, via `flox::RateLimitPolicy` -- and the
  policy is a SETTING (`SessionRateLimit`, applied with
  `setRateLimit(...)` on `TcpGateway`, `TlsGateway` and `WsGateway`), not a
  fixed profile. `SessionRateLimit::off()` is one of its values.

    Every session used to be handed `RateLimitPolicy::binance_um_futures()`:
    50 order actions per 10 seconds, then a three-minute ban after three
    refusals. That is one exchange's retail tier wired in as the only answer
    available. A client bridge that fans a single price move out into a burst
    of amendments is not abusing anything, and against that profile its
    ordinary traffic is a disconnect followed by three minutes of silence.
    The numbers stay the default, so an existing deployment is unchanged.

    **The refusal says how long to wait.** A rate-limit reject carries
    `RateLimited: retry in <n> ms` in the text (FIX `58`), and once a ban is
    armed, `RateLimitBanned: retry in <n> ms`. The bare reason left the client
    choosing between retrying in a millisecond and retrying in three minutes,
    and the usual choice -- retry at once -- is the one that walks into the
    ban; a silent ban reads exactly like one refused order, so the client
    keeps sending into a session that will refuse everything. The text travels
    beside the event through `SessionRegistry::RejectEncoder` (registered with
    `setRejectEncoder`): `OrderRejected` is the engine's message about an
    order, and why a SESSION refused a frame is not a property of any order.
    Without a reject encoder the refusal still arrives, just without the wait.

    A ban is also announced on the venue's own side, once per ban, through
    `GatewaySession::setBanObserver` (default: a `WARN` naming the session).
    Refusing a named client for minutes is an operational event, not a private
    matter between the limiter and one connection.
- **Cancel-on-disconnect** optionally pulls the session's resting orders when
  the connection drops.

## Exec-report delivery (SessionRegistry)

The per-frame `Responder` answers only the connection whose frame is being
handled; an asynchronous event -- a maker fill from a foreign aggressor, a
stop trigger, a GTD expiry, a liquidation cancel, a `FillHeld` -- has no
request context. `SessionRegistry` (`session_registry.h`) is the account ->
session router that closes that hole:

```
engine sink -> registry.route(event) -> AccountStream (per account)
  -> seq stamp + event log + encode -> SessionWriter (per connection)
    -> bounded queue -> writer thread -> socket
```

- Every outbound event carries its owner account (appended fields on the
  event structs, folded into the determinism hash). `Trade`, `FillHeld` and
  `FillRejected` route to both parties; account `0` is unrouteable. The
  mapping is public as `SessionRegistry::accountsOf(event, out[2], n)` -- a
  deployment that fans events out itself would otherwise re-derive it and
  then drift from it as events gain accounts.
- **A session that speaks for several accounts has ONE stream.** The extra
  accounts are aliases onto the first account's `AccountStream`: one sequence
  space, one resend log, one socket. Not one stream per account sharing a
  writer -- N streams would interleave N sequence spaces on one connection,
  and an event naming two of the session's accounts (a trade between two of a
  bridge's own customers) would be encoded, sequenced and delivered twice.
  The aliases are dropped on detach, so a borrowed account is free to open a
  session of its own afterwards.
- The **matching thread never blocks on a client socket**: `route()` encodes
  and enqueues under the account-stream mutex; the write happens on the
  session's writer thread. A full queue is a slow consumer -- the connection
  is shut down, `GatewayCounters::slowConsumerDisconnects` is bumped, and the
  client recovers the gap via `ResendRequest` after reconnecting.
- Gateways enter this mode via `setDelivery(&registry, encoder)`; the
  connection loop attaches the bound account on connect and detaches on
  disconnect. Without a registry a gateway stays in the embedded per-frame
  responder mode.
- **Rejects are not silent, and they name the frame they refused**: a frame
  that fails decode or admission answers with a sequenced `OrderRejected`
  (reason `MalformedMessage` / `RateLimited` / `Unauthenticated`) on the
  session's own stream, carrying the id, symbol and ClOrdID of the command it
  refused. When the frame did not decode there is no command to take them
  from, so the ClOrdID is read out of the raw bytes instead
  (`clientOrderIdFromRaw`: tag 11 at a field boundary, numeric only -- a
  wrong identifier points the client at an order it never sent, which is
  worse than none).

    The zeros this replaced cost the client twice. It could not match the
    refusal to anything it had sent, so it waited out its timeout and
    resent -- and the resend was refused again, as a duplicate ClOrdID. One
    refusal became two, and the second one explained nothing.
- `TlsGateway` supports delivery mode too. OpenSSL forbids CONCURRENT
  `SSL_read`/`SSL_write` on one `SSL*`, not serialized use: each connection
  carries a mutex over its `SSL*`, the read loop takes it only for the
  duration of a single `SSL_read` poll (a short `SO_RCVTIMEO` makes the call
  return periodically), and the writer thread takes it per frame written --
  the two interleave, never overlap. On teardown the writer is detached and
  joined before `SSL_free`.

## Sequencing and session-layer recovery

Every exec report delivered through the registry carries a per-session
monotonic `seq`:

- **SBE** -- schema version 1 appends a trailing `seq` (u64, `sinceVersion=1`)
  to every outbound template's root block. A version-0 reader skips it via the
  header's `blockLength`; no wrapper template, no re-layout
  (`venue/schema/order-entry-sbe.xml`, `SbeOrderEntryCodec::seqOf`).
- **The resend log stores events, not frames.** The `AccountStream` retains
  `(seq, OutboundEvent, first-send timestamp)`; frames are (re)encoded per
  the session's protocol at send and at resend time. SBE encoding is
  deterministic, so an SBE resend is byte-identical to the original
  transmission; FIX *requires* re-encoding on resend (PossDupFlag `43=Y` and
  OrigSendingTime `122` change BodyLength and CheckSum, so a byte replay was
  never viable there).
- **Recovery verbs (SBE, session layer -- never matched or journaled):**
  `ResendRequest{fromSeq}` replays the retained events with their original
  seqs from the account's resend log (which survives disconnects: an event
  that fires while the account is offline is still sequenced and logged). A
  `fromSeq` older than the retained log answers `SnapshotRequired{lastSeq}`
  -- an explicit signal, never a silent hole. `AccountSnapshotRequest`
  replies with the account's open orders (a series of `Accepted` frames,
  `restingOnBook=0` for pending stops) terminated by
  `SnapshotEnd{position, lastSeq}`, built from
  `MatchingEngine::snapshotAccount`. Snapshot frames are unsequenced (seq 0)
  and deliberately NOT in the resend log -- a resend replays what was
  originally sequenced; a point-in-time snapshot replayed there would be
  stale. `SnapshotEnd.lastSeq` (schema v2, trailing field) carries the
  stream's last assigned outbound seq, so the client resumes gap detection
  from the exact point. Wired via
  `setSessionVerbs(makeSbeSessionVerbs(engine))`.
- **Session config on the wire (SBE `SetSessionConfig{codEnabled}`)**:
  per-session cancel-on-disconnect negotiation, handled at the gateway via
  `setSessionConfigVerb(makeSbeSessionConfigVerb())`. Fire-and-forget by
  design: the update takes effect immediately and has no reply frame -- its
  effect is observable (a later disconnect sweeps or keeps the session's
  orders). The FIX equivalent is Logon tag 20003 (below).
- **FIX** -- a full session layer (`fix_session.h`: `FixSessionHost` +
  `FixConnection`, wired via `setFixSession` on `TcpGateway`, `TlsGateway`
  and `WsGateway`; requires delivery mode). The `FixConnection` is
  transport-independent; the wire shape per transport:
  - **TCP / TLS**: one FIX message per length-prefixed frame (TLS inside the
    encrypted stream, using the existing per-connection `sslMu` writer path
    and poll-before-lock read loop; the FIX timers run on the read loop's
    poll tick).
  - **WebSocket**: one FIX message per WebSocket data frame. The venue sends
    Text frames (FIX tag=value is ASCII; Text keeps the messages readable in
    WS tooling) and accepts inbound FIX in Text or Binary frames alike.
    FIX liveness (Heartbeat/TestRequest death) replaces the WS Ping probe on
    these connections.
  - **Logon (35=A)**: HeartBtInt (108) adoption; ResetSeqNumFlag (141=Y)
    resets both sequence directions to 1 and clears the outbound stream.
    Without 141, the client's `34` is checked against the expected inbound
    seq: above -> the venue's own `ResendRequest (35=2)` after the Logon
    reply; below -> Logout (35=5) with the reason in `58`, disconnect.
  - **Custom tag 20003 (CancelOnDisconnect=Y/N) on the Logon**: wire
    negotiation of the session's cancel-on-disconnect, overriding the
    gateway default in either direction (sits next to the custom last-look
    tags 20001 heldId / 20002 makerId). Applied on any accepted Logon,
    before order flow exists.
  - **Restart**: the sequence counters (per-account inbound `expectedIn` +
    outbound `lastSeq`) can be persisted across a venue restart via the FIX
    session sidecar (`FixSessionSidecar`, file `<journal base>.fixsessions`):
    the gateway harness hooks `SequencedShard::onCheckpoint` and writes the
    sidecar at every checkpoint boundary with the journal's durability
    discipline (tmp -> fsync -> atomic rename, trailing CRC32; a torn or
    corrupt sidecar loads as absent). With a restored sidecar, a Logon
    without 141=Y that continues the pre-restart sequence space WORKS. The
    EVENT LOG is deliberately not persisted: a ResendRequest that reaches
    into the pre-restart range is answered with `SequenceReset-GapFill` --
    the honest signal for history the venue no longer holds; full state
    reconciliation is the snapshot path. Without a sidecar the old rule
    stands: the first Logon after a restart MUST carry 141=Y or it is
    answered with `Logout "session state lost (venue restart): Logon must
    set ResetSeqNumFlag (141=Y)"`.
  - **Liveness**: outbound Heartbeat every HeartBtInt on the gateway's
    `SO_RCVTIMEO` tick; no inbound traffic for 1.2 intervals -> TestRequest
    (35=1); no answer for another 1.2 intervals -> disconnect, and COD sweeps
    normally. An inbound TestRequest is answered with a Heartbeat echoing
    `112`.
  - **Their ResendRequest (35=2, `7`..`16`, 16=0 = infinity)**: application
    messages replay from the event log re-encoded with `43=Y` +
    `122=OrigSendingTime` (the logged first-send time) and their ORIGINAL
    `34`; admin seq ranges (Heartbeats, Logon replies -- sequenced but not
    logged) collapse into `SequenceReset-GapFill (35=4, 123=Y)`. A range
    older than the retained log is gap-filled up to the first available seq
    -- the client sees the trimmed part as an explicit gap-filled hole; full
    state reconciliation is the SBE `AccountSnapshotRequest` path. A served
    FIX resend bumps `GatewayCounters::resendServed`, same as the SBE path.
  - **Our inbound gap**: no reorder buffer -- the venue sends `35=2` and
    DROPS every message above the hole (repeating the request at most once
    per HeartBtInt) until the counterparty's PossDup replay closes it. This
    is deliberate: buffering out-of-order application traffic would run
    orders outside admission order, and the peer must resend anyway. A
    PossDup whose `34` was already seen is silently dropped; an inbound
    `SequenceReset-GapFill` advances the expectation; a Reset-mode
    `SequenceReset` (no 123=Y) is accepted with a WARN log.
  - **Unknown MsgType**: an in-sequence message whose `35` is outside the
    known set (admin 0/1/2/4/5/A, application D/F/G) is answered with a
    session `Reject (35=3)` carrying `45=RefSeqNum`, `372=RefMsgType` and a
    `58` text -- never silently consumed, never a session kill. Application
    messages keep their own path: a decode failure there answers with an
    exec-report reject, not 35=3.
  - **BalanceUpdate has no FIX mapping** (documented as unsupported): an
    ExecutionReport is semantically an order-event report and carrying a
    pure balance change in one would be dishonest; FIX sessions reconcile
    balances out-of-band (the SBE/REST feeds carry `BalanceUpdate`). The
    event is simply not encoded on FIX sessions (no seq is consumed).
  - The sequencing/framing building block (`FixSession` in `fix_codec.h`)
    stays for embedded/test use.
  - **The other end of this session is `flox::fix::FixInitiator`**
    (`flox/connector/fix/`, in the core rather than the venue module -- see
    [Connecting to another venue over FIX](fix-initiator.md) for why). It
    opens the Logon, keeps the same liveness timers, serves a `ResendRequest`
    with the same PossDup/GapFill split, and closes its own gaps under the
    same no-reorder-buffer rule. Both ends frame through one shared
    implementation in `flox/connector/fix/fix_wire.h` (field parsing, the
    checksum, the header field order, SendingTime) and print decimals through
    one shared `flox/util/decimal_wire.h`, so the bytes cannot drift apart.
    `venue/tests/test_venue_fix_initiator.cpp` runs the pair against each
    other in one process.

The old `ResendBuffer` (an event-level log reachable from no wire path) was
removed; the client-side `GapDetector` stays in `resend_buffer.h` for the
market-data path.

## Liveness and cancel-on-disconnect

- **Idle timeout** (`setIdleTimeout`, default 30s): `SO_RCVTIMEO` on every
  connection fd; a peer with no inbound bytes for the whole window is
  disconnected (`GatewayCounters::idleDisconnects`), and COD then sweeps its
  orders normally -- a half-open peer can no longer hold orders and a thread
  forever. The WebSocket gateway pings at half the window first; any inbound
  (Pong or data) before the deadline keeps the session alive.
- **Shutdown**: `SocketAcceptor` tracks connection fds and owns their
  lifecycle; `stop()` shuts every one of them down before joining, so a
  silent client cannot hang `stop()`. Gateway handlers must not close the fd.
- **`DisconnectCanceller` is bounded**: the per-session delivery observer
  prunes an order on its terminal report (complete fill / cancel / reject),
  so the disconnect flush cancels only what is actually still live.
- **COD is per-session**: `GatewaySession::setCancelOnDisconnect` carries the
  flag; the gateway-wide atomic is only the default seeded into new sessions.
  Wire-level negotiation overrides the default in either direction: FIX Logon
  tag 20003 (CancelOnDisconnect=Y/N) or the SBE `SetSessionConfig{codEnabled}`
  verb (fire-and-forget, no reply frame).

## Wire protocols

| Codec | Notes |
|---|---|
| `SbeOrderEntryCodec` | SBE binary order entry + exec reports, full fidelity: every field round-trips, including `reduceOnly`, `peg`, `expiryNs`, `ocoGroup`, `lastLook`; schema version 8 adds inbound `QuoteLadder` (template 7), a maker's whole set of levels in one frame (schema `venue/schema/order-entry-sbe.xml`) |
| `FixCodec` | FIX 4.4: `D`/`F`/`G` in, `ExecutionReport` out, with `BodyLength` and validated `CheckSum`; `MassQuote` (`i`) / `QuoteCancel` (`Z`) in, `QuoteStatusReport` (`AI`) out -- a maker's whole ladder on one symbol, see [fix-quoting.md](fix-quoting.md) |
| `flox::fix::ClientCodec` | the mirror, for talking TO a venue: `D`/`F`/`G` out, `35=8`/`9`/`3`/`j` in ([FIX initiator](fix-initiator.md)) |
| `RestJson` | REST/JSON adoption path (simdjson) |
| `SbeMdCodec` | outbound market data (SBE, schema `venue/schema/md-sbe.xml`) |

Round-trip tests pin every field, because a dropped field here changes
behaviour: a `reduceOnly` flag lost on the wire turns a risk-reducing order
into one that can open a position.

## Hostile input

The perimeter is fuzzed, and the rules are explicit:

- **Length prefixes are capped before allocation**: `flox::net::kMaxFrame`
  (`flox/util/transport.h`) and `flox::ws::kMaxFramePayload`
  (`flox/util/websocket.h`), 16 MiB, both in core alongside the framing they
  guard. A 4-byte header cannot reserve gigabytes.
- **WebSocket extended length is overflow-safe.** The naive completeness check
  `n < off + len` wraps for a 64-bit length; the comparison is done as
  `len > n - off`, and an absurd length is a protocol error that closes the
  connection before any `resize`.
- **A parse error closes one connection**, and only that connection. That holds
  for an exception too: a connection handler runs as a thread body, so the
  acceptor contains anything escaping it rather than letting one malformed
  request reach `std::terminate` and take every engine in the process with it.
- **A frame split across packets is still one frame.** Every gateway sets a
  receive timeout so session timers and shutdown can run, and that timeout
  fires mid-message whenever a peer's write lands in two packets. Read state
  survives it: the bytes already taken are kept and the frame resumes on the
  next call (`flox::net::FrameReader` for the plain framed transport, the
  equivalent state in the TLS and WebSocket gateways). Dropping them would
  offset the stream by exactly that many bytes, and every length prefix after
  it would be read from the middle of a message.
- **An enum-typed field carries only values the schema defines.** `Side`,
  `OrderType`, `TimeInForce`, `STPMode` and `PegRef` each cross the wire as
  one byte, and a byte holds 256 values where these name at most eight.
  `SbeOrderEntryCodec::decode` range-checks every one of them (`inRange`,
  `flox-venue/messages.h`) before it builds a command, on `EnterOrder` and on
  `QuoteLadder` alike, and a value outside the set is a decode failure like
  any other -- `RejectReason::MalformedMessage` to the client, and
  `decode(p, n, err)` naming the field for the operator's log. Carrying such
  a byte inward makes the engine answer for a value it never defined: an
  order type wider than the 32-bit admission bitmap that indexes it, or a
  self-trade mode the auction uncross has no action for.

`test_venue_parser_fuzz` drives all decoders with random and adversarial
input; the sanitizer gate runs it under ASAN/UBSAN. The order-entry fuzz does
not stop at "it did not crash": every frame it decodes has its enum fields
checked against the same `inRange` the decoder uses, and is then submitted to
a live engine under the golden replay driver's per-command event budget
(`venue/tests/test_venue_golden_replay.cpp`).

## Control plane

`InstrumentRegistry` is the venue admin surface (list instruments, halt and
resume, adjust tick, lot, bands, trigger reference). `ControlApi` exposes it
as JSON-RPC and `ControlServer` serves it over TCP.

```cpp
api.handle(R"({"method":"halt","symbol":1,"halted":true})");
api.handle(R"({"method":"setBand","symbol":1,"minPrice":90,"maxPrice":110})");
```

Actions that change matchable state (auction transitions, emergency
cancel-all, halts) go through the sequenced `AdminCmd` path, so they are
journalled and survive replay. Configuration mutations are sequenced the same
way: `ControlApi` applies a successful mutation to the registry and forwards
the equivalent command (`ListInstrument`, `SetBands`, `SetTriggerRef`,
`AdminCmd` halt/resume)
to its command sink, which the deployment wires into the journaled stream --
on restart, `InstrumentRegistry::apply` replays those records. There is no
separate configuration store. See [Runtime and recovery](runtime.md).

Requests are read literally. A field nobody named is not a zero, so `setBand`
needs both of its bounds and a paired risk limit needs both of its halves; a
flag has to be spelled out rather than inferred. Half a band used to answer
`ok` and write a zero over the other half, which removes the collar an operator
believed was in place, and two handlers next to each other guessed opposite
defaults for a missing flag. A value that does not parse, is not finite, or
does not fit the fixed-point range answers `bad_field`; an inverted band
answers `bad_band`. Neither reaches the registry. The REST codec states the
same rule for order entry.

One connection is bounded too. A request line caps at 1 MiB
(`TcpControlServer::kMaxRequestLine`), and a client past that gets
`request_too_long` and a closed socket. The framed transport bounds a length
prefix for the same reason; on a line-delimited surface the unbounded thing is
the newline that never arrives.

`TcpControlServer` binds to loopback and has no port option to change that.
Deploy the control plane on an internal interface: it has no authentication of
its own, so anything able to reach it can move every risk limit on the venue.

### Verbs the deployment adds

The built-in verbs are not the whole vocabulary. `ControlApi::registerMethod`
puts a handler of your own on the same surface, and nothing in `ControlServer`
or `TcpControlServer` changes: the registered verb is reached through the
existing accept loop and line framing.

```cpp
api.registerMethod("parkInstrument",
                   [](const ControlRequest& req)
                   {
                     SymbolId sym{};
                     if (!req.symbolField("symbol", sym))
                     {
                       return ControlApi::err("bad_field");
                     }
                     if (!req.registry().get(sym))
                     {
                       return ControlApi::err("unknown_symbol");
                     }
                     req.forward(InboundCommand{AdminCmd{sym, AdminAction::Halt}});
                     return ControlApi::ok();
                   });
```

The handler reads its arguments through `ControlRequest`, which carries the
same accessors the built-in verbs use -- so a field nobody named stays absent
there too -- plus the two things a verb needs from the venue: `registry()` to
validate against, and `forward()` for the journaling rule. A handler that
changes engine state forwards the record that reproduces the change on replay;
a handler that only reads forwards nothing, exactly as `snapshotNow` and `get`
forward nothing.

Registration happens at wiring time, before a server accepts on that api:
`handle()` reads the table without a lock. It refuses an empty name, an empty
handler, a name a built-in verb already answers
(`ControlApi::kBuiltinMethods`), and a second registration of a name already
taken -- a verb that silently replaced another, or that a built-in silently
shadowed, sends operator traffic to a handler other than the one whose name
was typed.

A name nobody answers comes back named:
`{"ok":false,"error":"unknown_method","method":"parkInstrument"}`. A bare
`unknown_method` reads the same for a typo, a registration that never ran, and
a request that reached the wrong process. The echoed name is caller input, so
it is cut at 64 characters and escaped before it travels in the response.

## Observability

`Metrics` counts orders, trades, rejects by reason, and fills. `Gauges`
samples venue state (open interest, position count, best bid/ask, mark age,
feed-breaker state). `prometheus.h` renders the exposition format and
`MetricsServer` serves it over HTTP for scraping.

### Last look

Six series, from `LastLookSample`. Four are labeled by maker:

| Series | What it counts |
|---|---|
| `fme_last_look_holds_total{maker}` | fills held for that maker's decision |
| `fme_last_look_rejects_total{maker}` | held fills it refused |
| `fme_last_look_rejects_adverse_total{maker}` | ... of which the price had moved AGAINST it |
| `fme_last_look_rejects_favourable_total{maker}` | ... of which the price had moved its WAY |

and two are venue-wide, because no maker can be blamed for them:
`fme_last_look_tolerance_rejects_total` (the venue refused the hold on its own
tolerance, whatever the maker answered) and
`fme_last_look_prorata_skips_total` (pro-rata participants skipped for holding
rather than standing firm).

The split by direction is the reason these exist. A maker refusing only the
fills that moved its way is taking a free option: it keeps the good ones and
hands back the bad, and every taker pays for it. A maker refusing at a similar
rate in both directions is answering a latency problem instead. **The totals
are identical in both cases** -- an alert on `fme_last_look_rejects_total`
alone cannot tell them apart, which is what `..._favourable_total` over
`..._rejects_total` is for.

A maker that refused nothing still gets its series, at zero. A series that
vanishes when it is healthy cannot be alerted on, and its absence would read
as "no data" rather than "held ten, refused none".

Like `Gauges`, these are sampled rather than pushed: the library runs no
monitoring thread, so a deployment reads `lastLookStats()`,
`toleranceRejectedHolds()` and `skippedLastLookProRata()` off the engine and
hands them to `prom::render`. Left unsampled they render zeros, and zeros look
exactly like a venue where nobody has ever held a fill.

### Who may read the engine while it is matching

Almost everything in the engine belongs to the matching consumer thread and is
read by nobody else. Three accessors are the exception, because they exist to
be scraped: **`lastLookStats()`, `admissionProfiles()` and
`restingOrderCount()` may be called from any thread while the consumer is
matching** — `MetricsServer` runs its sampler on the connection thread that
answered the scrape, which is exactly what the section above instructs. No
other accessor carries that permission; `hasHold()` / `forEachHold()` state
their own consumer-thread rule and mean it.

The first two hand back a **snapshot by value**, taken under
`engine::SnapshotLock` (`venue/include/flox-venue/engine/snapshot_lock.h`) --
a spinlock the consumer takes when it writes one of those containers and the
sampler takes for the length of one copy. A reference into live storage would
be neither safe nor useful: unsafe because an insert can rehash the map under
the reader's iterator, and useless because a page built from a container that
keeps moving carries two series from two different moments. The lock is a
spinlock and not a mutex because of who pays: uncontended it is one atomic
exchange on the consumer, and a scrape happens once every few seconds.

`restingOrderCount()` is published through a relaxed atomic instead, because
it is the one of the three written on the per-ORDER path -- every order that
rests or leaves. A lock there, for a gauge, is a cost the matching path does
not take; a count is a single number with nothing else it has to agree with,
so nothing is lost by publishing it that way.
