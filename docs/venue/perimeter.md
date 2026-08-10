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
TcpGateway gw([](const uint8_t* p, size_t n) { return OuchCodec::decode(p, n); });
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

## Wire protocols

| Codec | Notes |
|---|---|
| `OuchCodec` | binary, full fidelity: every field round-trips, including `reduceOnly`, `peg`, `expiryNs`, `ocoGroup`, `lastLook` |
| `FixCodec` | FIX 4.4: `D`/`F`/`G` in, `ExecutionReport` out, with `BodyLength` and validated `CheckSum` |
| `RestJson` | REST/JSON adoption path |
| `SbeMdCodec` | outbound market data (SBE) |

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
cancel-all, halts) go through the sequenced `AdminCmd` path instead, so they
are journalled and survive replay. Configuration knobs recover from the
configuration store. See [Runtime and recovery](runtime.md).

Deploy the control plane on an internal interface: it has no authentication of
its own.

## Observability

`Metrics` counts orders, trades, rejects by reason, and fills. `Gauges`
samples venue state (open interest, position count, best bid/ask, mark age,
feed-breaker state). `prometheus.h` renders the exposition format and
`MetricsServer` serves it over HTTP for scraping.
