# `flox/venue` — the venue-side layer

The rest of FLOX is strategy-side: you send orders to an exchange. This layer
is the exchange. It matches participants' orders against each other, clears
the money, and manages the risk of doing so.

It is an optional module with its own `CMakeLists.txt`, include prefix
`<flox-venue/...>`, and target `flox::venue`, gated by `FLOX_BUILD_VENUE`.
Core stays free of its weight, and the server perimeter carries its own
dependencies (simdjson via FetchContent for the REST codec; OpenSSL for the
TLS gateway is optional and detected here).

Namespace: `flox::venue`, nested so venue vocabulary can reuse names the
strategy side already owns; `Trade` and `SymbolConfig` exist in both. Core
scalar types (`Price`, `Quantity`, `Side`, `OrderId`) come from
`flox/common.h` as usual. Because the two sets overlap, avoid pulling both in
unqualified; write `namespace venue = flox::venue;` and qualify.

The order-level books stayed in core (`flox/book/*`, next to the aggregate
`NLevelOrderBook`) along with the general utilities this layer needed
(`flox/util/{crypto,wire,transport,websocket,system_clock}.h`), because they
are useful on their own.

Platform: Linux and macOS. Skipped on MSVC/clang-cl, which have no `__int128`,
the type the ledger keeps money in.

## What it unlocks

Strategy backtests replay a recorded tape, and the market never reacts to your
orders. With this layer many participants (or agents) trade against a single
live book, so the market responds: impact, queue position, and counterparty
behaviour emerge from the interaction.

## Map

| Area | Headers |
|---|---|
| Order-level books | `flox/book/{resting_order,matching_book,ladder_book}.h` — per-order FIFO + O(1) ladder (distinct from the aggregate `NLevelOrderBook`, which is market-data depth) |
| Matching | `matcher.h` (price-time FIFO, pro-rata, STP, last-look), `matching_engine.h` (order lifecycle, TIF, stops, OCO, peg, iceberg, auctions, LULD, perp clearing), `stop_book.h`, `symbol_router.h` |
| Money | `ledger.h` — double-entry, `__int128`, `available`/`reserved`, conservation-exact, per-symbol scale helpers |
| Derivatives risk | `cross_margin.h` (portfolio margin), `collateral.h` (haircut basket), `funding_rate.h` + `funding_scheduler.h`, `index_feed.h` (manipulation-resistant mark), `mark_feed_driver.h` (stale-feed circuit breaker) |
| Runtime | `sequenced_shard.h` — single-writer core on the FLOX `EventBus`, journalled |
| Recovery | `journal.h` (WAL + replay), `event_hash.h` (bit-identical determinism check) |
| Market data out | `market_data.h` + `itch_codec.h` — turns venue events into an L2 feed |
| Protocol / ops | `fix_codec.h`, `resend_buffer.h`, `messages.h`, `reject_reason.h`, `metrics.h`, `prometheus.h` |
| Perimeter | `socket_acceptor.h`, `session.h` (API-key HMAC logon, rate limits), `cancel_on_disconnect.h`, `tcp_gateway.h`, `ws_gateway.h`, `tls_gateway.h` (OpenSSL, optional), `udp_multicast.h`, `control_plane.h` / `control_api.h` / `control_server.h`, `metrics_server.h`, `tape_recorder.h` |
| Wire protocols | `fix_codec.h`, `ouch_codec.h`, `rest_json.h`, `itch_codec.h` |

## Two margin models

`flox::venue::CrossMarginManager` and `flox::LiquidationEngine`
(`flox/backtest/`) both liquidate. They answer different questions:

- **`LiquidationEngine` (backtest).** Positions and equity in `double`, driven
  by a mark tape. Built for simulation: fast, tolerant, models cascades,
  slippage, ADL ranking, and mark impact.
- **`CrossMarginManager` (venue).** Portfolio margin backed by the `Ledger`
  (`__int128`), conservation-exact per asset, with multi-asset collateral
  haircuts and segregation. Built for a venue moving real balances, where a
  rounding drift is a money bug.

Collapsing them would either put `double` money on the venue path or force
ledger machinery into backtests. It is the same split FLOX makes between
`SimulatedClock` and `SystemClock`: one concept, two drivers. Pick by what you
are building, and keep one account on one model.

## Verification

The venue core is fuzzed in FLOX CI:

- `test_venue_differential_fuzz`: the map-reference book and the O(1) ladder
  are driven in lockstep and must emit an identical event stream (rolling
  hash) with no crossed book. 200k commands in CI; set `FLOX_FUZZ_OPS` for a
  deep run (2M verified).
- `test_venue_conservation_fuzz`: each asset's total is invariant across a
  random command stream (spot, perp, perp+ADL, cross margin, multi-asset
  collateral), and after the book is drained every account's `reserved` must
  return to zero. A stuck reservation passes conservation-of-total and fails
  that second check.

Focused suites cover the matcher (FIFO/pro-rata/STP), ledger, mark price,
mark-feed circuit breaker, market data, cross margin, collateral, segregation,
funding scheduler, sequenced shard, per-symbol scale, multi-symbol soak, and
the perp venue. See `docs/venue/` for the full documentation.
