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
- **Account binding.** A session bound to an account stamps that account onto
  every command, overwriting whatever the payload carried. Otherwise a client
  could act as any account by writing a different id into the message. Account
  `0` is the explicit "unbound / trusted transport" sentinel.
- **Rate limiting**, per session, via `flox::RateLimitPolicy`.
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
  `FillRejected` route to both parties; account `0` is unrouteable.
- The **matching thread never blocks on a client socket**: `route()` encodes
  and enqueues under the account-stream mutex; the write happens on the
  session's writer thread. A full queue is a slow consumer -- the connection
  is shut down, `GatewayCounters::slowConsumerDisconnects` is bumped, and the
  client recovers the gap via `ResendRequest` after reconnecting.
- Gateways enter this mode via `setDelivery(&registry, encoder)`; the
  connection loop attaches the bound account on connect and detaches on
  disconnect. Without a registry a gateway stays in the embedded per-frame
  responder mode.
- **Rejects are not silent**: a frame that fails decode / admission answers
  with a sequenced `OrderRejected` (id 0, reason `MalformedMessage` /
  `RateLimited` / `Unauthenticated`) on the session's own stream.
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
| `SbeOrderEntryCodec` | SBE binary order entry + exec reports, full fidelity: every field round-trips, including `reduceOnly`, `peg`, `expiryNs`, `ocoGroup`, `lastLook` (schema `venue/schema/order-entry-sbe.xml`) |
| `FixCodec` | FIX 4.4: `D`/`F`/`G` in, `ExecutionReport` out, with `BodyLength` and validated `CheckSum` |
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
- **A parse error closes one connection**, and only that connection.

`test_venue_parser_fuzz` drives all decoders with random and adversarial
input; the sanitizer gate runs it under ASAN/UBSAN.

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

Deploy the control plane on an internal interface: it has no authentication of
its own.

## Observability

`Metrics` counts orders, trades, rejects by reason, and fills. `Gauges`
samples venue state (open interest, position count, best bid/ask, mark age,
feed-breaker state). `prometheus.h` renders the exposition format and
`MetricsServer` serves it over HTTP for scraping.
