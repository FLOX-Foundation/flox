# `flox/venue` — the venue-side layer

Everything under `flox/venue/` is for building **a market**, not for trading on
one. The rest of flox is strategy-side (you send orders to an exchange);
this layer is the exchange: it matches other participants' orders against each
other, clears the money, and manages the risk of doing so.

Built as an optional module (same shape as `connectors/`): its own
`CMakeLists.txt`, include prefix `<flox-venue/...>`, target `flox::venue`, gated
by `FLOX_BUILD_VENUE`. That keeps the core library free of its weight and lets
the server perimeter carry its own dependencies (OpenSSL for the TLS gateway is
optional and detected here, not forced on core).

Namespace: `flox::venue` (nested so venue vocabulary can reuse names the
strategy side already owns — `Trade` and `SymbolConfig` exist in both). Core
scalar types (`Price`, `Quantity`, `Side`, `OrderId`) come from
`flox/common.h` as usual. Because the two sets overlap, do not pull both in
unqualified: prefer `namespace venue = flox::venue;` and qualify.

What stayed in core, deliberately: the order-level books (`flox/book/*`, they sit
next to the aggregate `NLevelOrderBook` and are useful on their own) and the
general utilities this layer needed (`flox/util/{crypto,wire,transport,websocket,
system_clock}.h`).

Platform: Linux and macOS. Skipped on MSVC/clang-cl, which have no `__int128`
— the type the ledger keeps money in.

## What it unlocks

Strategy backtests replay a recorded tape: the market never reacts to your
orders. With this layer many participants (or agents) can trade against a
single live book, so the market responds — impact, queue position, and
counterparty behaviour are emergent instead of assumed.

## Map

| Area | Headers |
|---|---|
| Order-level books | `flox/book/{resting_order,matching_book,ladder_book}.h` — per-order FIFO + O(1) ladder (distinct from the aggregate `NLevelOrderBook`, which is market-data depth) |
| Matching | `matcher.h` (price-time FIFO, pro-rata, STP, last-look), `matching_engine.h` (order lifecycle, TIF, stops, OCO, peg, iceberg, auctions, LULD, perp clearing), `stop_book.h`, `symbol_router.h` |
| Money | `ledger.h` — double-entry, `__int128`, `available`/`reserved`, conservation-exact |
| Derivatives risk | `cross_margin.h` (portfolio margin), `collateral.h` (haircut basket), `funding_rate.h` + `funding_scheduler.h`, `index_feed.h` (manipulation-resistant mark), `mark_feed_driver.h` (stale-feed circuit breaker) |
| Runtime | `sequenced_shard.h` — single-writer core on the flox `EventBus`, journalled |
| Recovery | `journal.h` (WAL + replay), `event_hash.h` (bit-identical determinism check) |
| Market data out | `market_data.h` + `itch_codec.h` — turns venue events into an L2 feed |
| Protocol / ops | `fix_codec.h`, `resend_buffer.h`, `messages.h`, `reject_reason.h`, `metrics.h`, `prometheus.h` |
| Perimeter | `socket_acceptor.h`, `session.h` (API-key HMAC logon, rate limits), `cancel_on_disconnect.h`, `tcp_gateway.h`, `ws_gateway.h`, `tls_gateway.h` (OpenSSL, optional), `udp_multicast.h`, `control_plane.h` / `control_api.h` / `control_server.h`, `metrics_server.h`, `tape_recorder.h` |
| Wire protocols | `fix_codec.h`, `ouch_codec.h`, `rest_json.h`, `itch_codec.h` |

## Two margin models, on purpose

`flox::venue::CrossMarginManager` and `flox::LiquidationEngine`
(`flox/backtest/`) both liquidate, and that is deliberate — they answer
different questions:

- **`LiquidationEngine` (backtest).** Positions and equity in `double`, driven
  by a mark tape. Right for simulation: fast, tolerant, models cascades,
  slippage, ADL ranking, and mark impact.
- **`CrossMarginManager` (venue).** Portfolio margin backed by the `Ledger`
  (`__int128`), conservation-exact per asset, with multi-asset collateral
  haircuts and segregation. Right for a venue moving real balances, where a
  rounding drift is a money bug rather than a modelling artefact.

Collapsing them into one would either put `double` money into the venue path or
force ledger machinery into backtests. This is the same split flox already makes
between `SimulatedClock` and `SystemClock`: one concept, two drivers. Pick by
what you are building; do not mix them for the same account.

## Verification

The venue core is fuzz-proven in flox's own CI:

- `test_venue_differential_fuzz` — the map-reference book and the O(1) ladder are
  driven in lockstep and must emit an identical event stream (rolling hash) with
  no crossed book. 200k commands in CI; set `FLOX_FUZZ_OPS` for a deep run
  (2M verified).
- `test_venue_conservation_fuzz` — money is neither created nor destroyed across
  a random command stream (spot, perp, perp+ADL), and after the book is drained
  every account's `reserved` must return to zero. A stuck reservation is
  invisible to conservation-of-total but shows up in that second check.

Plus focused suites: matcher (FIFO/pro-rata/STP), ledger, mark price, mark-feed
circuit breaker, market data, cross margin, collateral, segregation, funding
scheduler, sequenced shard, pro-rata, multi-symbol soak, perp venue.


## What is deliberately not here

Broker- or instrument-specific policy: MT4/5 semantics (netting vs hedging,
swap/rollover), CFD contract specifications, LP aggregation and A/B-book
routing. Those belong to the deployment that runs a venue, not to the framework.
