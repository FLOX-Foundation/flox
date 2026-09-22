# Connecting to another venue over FIX

The venue module answers FIX sessions. This page is the other half: opening
one. `flox::fix::FixInitiator` is the client end of a FIX 4.4 order-entry
session, written against the acceptor described in
[Perimeter](perimeter.md) so that the two ends agree about recovery and not
only about the happy path.

There are two callers. A strategy connects to a counterparty and sends orders.
A venue connects to another venue and becomes its client. They get the same
session.

## Where it lives, and why not next to the codec

The initiator is in the core, at `include/flox/connector/fix/`. The venue's
`FixCodec` stays in the venue module.

The deciding factor was what the arrangement costs a strategy author who will
never run a matching engine. `FLOX_BUILD_VENUE=ON` pulls in the matching
engine, clearing, venue risk and simdjson, forces `FLOX_ENABLE_BACKTEST=ON`,
and is off entirely on MSVC and clang-cl because the module needs native
128-bit integers. Putting the initiator there would charge that whole bill for
a FIX client, and would leave Windows with no FIX client at all.

What the two ends share is smaller than a module: field parsing, the
`BodyLength`/`CheckSum` framing, the header field order, the SendingTime
format, and the decimal printing. Those moved to
`flox/connector/fix/fix_wire.h` and `flox/util/decimal_wire.h`, where both
sides read them. `FixCodec` calls into them now instead of keeping its own
copies, so a change to the checksum or the header order lands on both ends at
once.

| Header | What it holds |
|---|---|
| `flox/connector/fix/fix_wire.h` | framing, field parsing, checksum, header, SendingTime |
| `flox/connector/fix/fix_client_codec.h` | `35=D`/`F`/`G` out, `35=8`/`9`/`3`/`j` in |
| `flox/connector/fix/fix_initiator.h` | the session: Logon, liveness, sequencing, recovery |
| `flox/connector/fix/fix_tcp_client.h` | the framed TCP client and a session driver |
| `flox/connector/fix/fix_executor.h` | `IRoutableExecutor` over a session |

## A session

```cpp
#include "flox/connector/fix/fix_initiator.h"
#include "flox/connector/fix/fix_tcp_client.h"

flox::fix::FixInitiatorConfig cfg;
cfg.senderCompId = "CLIENT";
cfg.targetCompId = "VENUE";
cfg.heartBtIntSec = 30;

flox::fix::FixInitiator initiator{cfg};
initiator.setReportHandler([](const flox::fix::InboundReport& r) {
  if (const auto* e = std::get_if<flox::fix::ExecutionReport>(&r)) {
    // e->execType, e->lastQty, e->lastPx, e->leavesQty
  }
});

flox::fix::FixTcpClient tcp;
flox::fix::FixTcpSession session{initiator, tcp};
session.connect("10.0.0.7", 9876, nowNs());

while (session.poll(nowNs())) {
  // poll() reads what has arrived and runs the session timers
}
```

`poll()` returns false when the session ends, for any of the three reasons a
FIX session ends: a Logout was exchanged, liveness was lost, or the transport
went away. `loggedOn()` goes false at the same moment, however the session
ended -- a caller that judges reachability by the flag rather than by `poll()`'s
return value sees the same answer either way, instead of one that stays true
through a transport loss it never got a Logout for.

Sending is typed:

```cpp
flox::fix::NewOrderRequest o;
o.clOrdId = 1001;
o.symbol = 3;
o.side = flox::Side::BUY;
o.type = flox::OrderType::LIMIT;
o.price = flox::Price::fromRaw(priceRaw);
o.quantity = flox::Quantity::fromRaw(qtyRaw);
initiator.submit(o, nowNs());
```

`Price` and `Quantity` are fixed-point on the way in and on the way out. A
price of `100.25` is printed as `100.25`, not as `100.250000` and not as
`100.24999999`; the printing runs through `flox::decwire`, the same code the
venue uses on its side.

## Logon and the sequence space

The outgoing Logon (`35=A`) carries `HeartBtInt` (108) and, by default,
`ResetSeqNumFlag` (141=Y), which restarts both directions at 1 and clears the
retained resend log. Set `cancelOnDisconnect` and the Logon also carries the
custom tag 20003 the venue reads to negotiate cancel-on-disconnect per session.

The `HeartBtInt` in the reply governs, not the one the Logon proposed. The
acceptor is the end that will act on the interval.

Turn 141 off, and the Logon continues the sequence space the counterparty
already believes in. That is what a reconnect wants, and what the sidecar below
makes possible across a restart.

## Liveness

A Heartbeat goes out after `HeartBtInt` of outbound silence. No inbound traffic
for 1.2 intervals sends a TestRequest (`35=1`); silence for another 1.2
intervals ends the session. An inbound TestRequest comes back as a Heartbeat
echoing `112`, which is what identifies the probe being answered.

These are the acceptor's numbers, read from the other side. A client that
waited longer than the acceptor does would be disconnected while it was still
deciding whether to worry.

## Recovery

### Their ResendRequest (`35=2`)

Application messages replay from the retained log, re-encoded with
`PossDupFlag (43=Y)` and `OrigSendingTime (122)` and their original `34`.
Sequence numbers the log does not hold -- admin traffic, which is sequenced and
not logged, and history trimmed past `resendLogCapacity` -- collapse into
`SequenceReset-GapFill (35=4, 123=Y)`.

Re-encoding rather than replaying bytes is forced: 43 and 122 change
`BodyLength` and `CheckSum`, so a byte replay was never available here.

### Our own gap

There is no reorder buffer. A message whose `34` sits above what is expected
triggers a `ResendRequest` and is dropped, along with everything else above the
hole, until the counterparty's PossDup replay closes it. The request repeats at
most once per `HeartBtInt` while the hole stays open, and a TestRequest is
still answered so liveness survives it.

The acceptor does the same thing for the same reason. An execution report
applied out of order tells a strategy about a fill that has not happened yet,
and the counterparty has to resend regardless, so a buffer would buy nothing
and cost a wrong answer. A PossDup whose `34` was already consumed is dropped
silently.

### Sequence violations

A `34` below what is expected, with no PossDupFlag, is a corrupted session:
Logout with the reason in `58`, then disconnect. A message with no `34` at all
is not a FIX 4.4 message and gets the same treatment. A message whose checksum
is present and wrong is dropped without an answer, since its `34` cannot be
trusted either.

## Message types a client session does not speak

An in-sequence message outside the set a client handles (admin `0/1/2/3/4/5/A/j`,
application `8/9`) is answered, never swallowed. Which answer depends on what
is wrong:

- `35=j` (BusinessMessageReject) for a real FIX 4.4 application type that this
  client does not implement. The session is healthy; the message is declined.
- `35=3` (session Reject) for anything else, carrying `45=RefSeqNum`,
  `372=RefMsgType`, `373=11` and a `58` text.

The line is drawn by the same list the acceptor uses, in `fix_wire.h`. Sending
`35=3` for a message the counterparty is entitled to send reads as a fault on
this side rather than as a declined capability.

## Restart

`FixInitiatorSidecar` persists the two counters that have to survive: the next
outbound `34` and the next inbound `34` expected. It writes through a temporary
file, an `fsync` and an atomic rename, with a trailing CRC32 -- the journal's
discipline, and the same one the venue's `FixSessionSidecar` uses. A torn or
corrupt file loads as absent, which costs a `141=Y` on the next Logon and never
a wrong sequence space.

```cpp
flox::fix::FixSeqState saved;
if (flox::fix::FixInitiatorSidecar::load(path, saved)) {
  initiator.restore(saved);   // also turns 141=Y off: continue, do not restart
}
// ... later, at a checkpoint boundary
flox::fix::FixInitiatorSidecar::write(path, initiator.seqState());
```

The resend log is deliberately not persisted. A ResendRequest reaching into the
pre-restart range is answered with GapFill, the same honest signal the venue
gives for history it no longer holds.

## Reading a flox venue's reports

The decoder reads the fields a flox venue actually writes, including the ones
FIX 4.4 has no spelling for. A last-look venue reports a fill held pending the
maker's decision as `ExecType=U` with custom tags 20001 (`heldId`) and 20002
(`makerId`), and reports a held fill that will not stand as `ExecType=H`. Those
arrive as `ExecType::FillHeld` and `ExecType::TradeCancel` with the ids intact.

A client that dropped them would read a hold as a plain working order and would
never learn how it resolved. See [Matching](matching.md) for what the venue
means by the two.

`OrderCancelReject (35=9)` decodes as its own type rather than as an execution
report, because that is what it is: an execution report describes the state of
an order, and a refused cancel changed no order state.

## Routing between counterparties

`FixRoutableExecutor` implements `IRoutableExecutor`, so `OrderRouter`
registers several FIX counterparties and switches between them on its own
routing strategy:

```cpp
flox::fix::FixRoutableExecutor primary{initiatorA};
flox::fix::FixRoutableExecutor backup{initiatorB};

flox::OrderRouter<4> router;
router.registerExecutor(0, &primary);
router.registerExecutor(1, &backup);
router.setFailoverPolicy(flox::FailoverPolicy::FailoverToBest);
```

The router's `cancel()` carries only an `OrderId`, while FIX wants the symbol
and side of the order being cancelled, so the executor remembers the few bytes
of each working order and drops the entry on the terminal report. Feed inbound
reports to `onReport()` for that to stay accurate.

## Transport and TLS

`FixTcpClient` speaks the framed TCP wire the venue's `TcpGateway` serves: one
FIX message per length-prefixed frame. Reads go through
`flox::net::FrameReader`, so a receive timeout in the middle of a frame is
resumable and the session timers run without the reader losing its place.

`FixTlsChannel` is the same thing encrypted: the same `connect`/`send`/`read`/
`close`, the same framing, the same resumable read. The initiator hands
finished messages to a `SendFn` and is fed complete messages through
`onFrame()`, so the channel drops in where `FixTcpClient` sits and the session
layer does not know which one it is on.

### Why it is behind a flag

`-DFLOX_FIX_TLS=ON` finds OpenSSL and builds the channel; the flag is off by
default and the core links no OpenSSL without it. Nobody who does not ask for
TLS acquires the dependency, which is why the initiator is in the core in the
first place: a FIX client should not pay for a matching engine, and it should
not pay for a TLS stack it never opens either.

The alternative placement considered for THIS channel was `connectors/`, which
already links OpenSSL. It also requires ZLIB and CURL and fetches ixwebsocket
and simdjson -- three system libraries and two fetched projects to get an
encrypted FIX session. One optional dependency behind one flag is the cheaper
of the two, and that is the whole reason for the placement.

(The venue module does not come into this. `flox-venue` links neither
`connectors` nor OpenSSL: its TLS gateway finds OpenSSL on its own and links
it to that gateway's test alone.)

### Verification is on

The venue's own `tls::clientCtx()` sets `SSL_VERIFY_NONE`, which is correct
where it is used: a test talking to a self-signed certificate it just
generated. It would not be correct here. A counterparty's certificate is the
only thing between a FIX session and whoever happens to answer that port, so
the channel:

- verifies the chain against the system trust store, or a `caFile`/`caPath`
  the caller names;
- checks the hostname (`SSL_set1_host`) -- without it any certificate the
  trust store accepts would pass, not just this counterparty's;
- requires TLS 1.2 or better;
- **refuses to connect if the trust store could not be loaded.** A verify that
  cannot load its roots verifies nothing, and failing is the honest outcome.

`Options::verifyPeer = false` turns it off. It is a named, explicit act, which
is what a test harness against a self-signed venue needs and what production
should never contain.

A failed handshake leaves nothing readable: the `SSL*` and the socket are
dropped together, so no later read can pick bytes off a half-established
channel. A frame length the channel will not honour closes the session rather
than resynchronising -- past that point the stream is no longer one this side
can follow.

## What the numbers look like

Measured over loopback against a live venue acceptor, from the `submit()` call
to the bytes reaching the socket:

| | p50 | p99 |
|---|---|---|
| `submit()` to bytes handed to the socket | 7.6 us | 19.0 us |
| `submit()` to encoded and sequenced, no write | 1.25 us | 1.5 us |

The write dominates. The 1.25 us the initiator owns is string building: the
codec composes `tag=value` through `std::string`, the way `FixCodec` does on
the venue side. That is where to look first if the number matters to you.

## Testing

`venue/tests/test_venue_fix_initiator.cpp` runs the initiator against the
venue's own `FixSessionHost` in one process: logon, an order, the report
decoded back into typed fields, a dropped outbound message recovered through
the counterparty's ResendRequest, a dropped inbound message recovered through
ours, and a reconnect that continues the sequence space across a sidecar
restore. Both ends are real, which is the whole point of that file: a client
and a server that each pass their own tests can still disagree about recovery,
and the disagreement shows up as orders applied in the wrong order rather than
as a failure.

`tests/test_fix_initiator.cpp` pins the session rules against a scripted
counterparty in memory, and `tests/test_fix_client_parser_fuzz.cpp` drives
hostile input through the decoder and the session layer under AddressSanitizer
and UndefinedBehaviorSanitizer. An initiator dials out, so the bytes it parses
come from a machine it does not run.
