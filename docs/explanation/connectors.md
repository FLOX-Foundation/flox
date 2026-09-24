# Connectors

FLOX ships native exchange connectors as part of the same repo. They live under [`connectors/`](https://github.com/FLOX-Foundation/flox/tree/main/connectors) and build into a single `flox::connectors` static library that links against `flox::flox`.

The module is gated by `FLOX_BUILD_CONNECTORS` in CMake. It is **off** by default — backtest-only and research builds skip the dependency cost (OpenSSL, libcurl, zlib, plus ixwebsocket and simdjson via FetchContent at configure time).

## Adapters in tree

| Venue | Trades | BBO | Book | Orders |
|---|---|---|---|---|
| Bybit (V5) | ✓ | ✓ | ✓ | ✓ |
| Bitget (V2) | ✓ | ✓ | ✓ | ✓ |
| Hyperliquid | ✓ | ✓ | ✓ | ✓ |
| Polymarket | ✓ | ✓ | — | ✓ |

(No position query/stream is implemented in any connector yet — the earlier "Positions" column was aspirational.)

Each adapter sits under `connectors/src/<venue>/` with public headers under `connectors/include/flox-connectors/<venue>/`. The header layout uses the `flox-connectors/` prefix so consumers' include sites are stable across the repo move.

### Order-type and flag coverage

The "Orders" column above means "submit/cancel/replace exists," not "every `OrderType` and flag is supported." Coverage differs per venue and is enforced at submit time — an order the connector cannot serialize correctly is rejected rather than sent as an approximation:

- `TimeInForce` and reduce-only are serialized on all four venues. Post-only maps to the venue's maker-only token (Bybit `PostOnly`, Bitget `post_only`, Hyperliquid `Alo`); `GTD` has no native equivalent on any of the four and is rejected rather than silently downgraded to `GTC`.
- Stop-market, stop-limit, take-profit-market and take-profit-limit route through Bitget's plan-order endpoint and Bybit's conditional-order fields on the regular order endpoint. Hyperliquid has no trigger-order implementation in this connector and rejects them.
- `TRAILING_STOP` and `ICEBERG` are not implemented on any of the three CEX connectors (Polymarket has no order-side concept of either) and are rejected at submit time. Bybit's trailing stop in particular lives on a different endpoint (`/v5/position/trading-stop`) than the rest of order submission, which this connector does not call.

A rejected order publishes `OrderEventStatus::REJECTED` on the venue's `OrderExecutionBus` with a reason string identifying the unsupported type or flag — it never reaches the exchange as a same-looking order with the unsupported part silently dropped.

## The live fill contract

Every connector that reports fills must honour all of the following. The engine has no way to detect a violation — a fill that breaks one of these rules looks exactly like a correct fill, and the damage shows up as a wrong position or a wrong PnL much later.

- **`fillQty` is the size that just traded**, not the order's size and not the cumulative filled quantity. A venue that reports cumulatively (Bybit's `cumExecQty`, Bitget's `accBaseVolume`) must be differenced against what the connector has already published. The cumulative number belongs in `order.filledQuantity`.
- **`fillPrice` is the price it traded at.** `PositionTracker::onOrderPartiallyFilled(order, fillQty, fillPrice)` builds cost basis and realized PnL from it, so an unset `fillPrice` books the position at zero.
- **One event per execution.** Where a venue announces the same execution on more than one channel (Bybit's `order` and `execution` topics both report it), the connector publishes it once. Identity is the venue's own execution id or, where there is none, a per-order cumulative watermark: the first channel to report an execution advances the watermark and publishes the increment, the second computes a zero increment and publishes nothing.
- **`order.id` is the id the engine issued**, never the venue's. Executors send the engine's `OrderId` as the venue's client order id (Bybit `orderLinkId`, Bitget `clientOid`, Hyperliquid `cloid`) and the connector reads it back off every private frame. The venue's own order id is the fallback for orders this engine did not place, and it belongs in the tracker's `exchangeOrderId`, not in `OrderEvent::order.id`.
- **A venue rejection publishes `REJECTED` with the venue's own reason text** and leaves no live order behind, in the tracker or in the strategy's belief.
- **`recvNs` is stamped on receipt and `sourceExchange` names the venue** on every event. Both are covered in [Building a custom connector](../how-to/custom-connector.md): without `sourceExchange`, `CompositeBookMatrix` drops the update and the cross-venue book is empty in live; without `recvNs`, its staleness sweep skips the venue and a frozen feed keeps being quoted.

`connectors/tests/unit_test_*_fill_contract.cpp` pins this per venue, offline, by feeding recorded frames into the connector's message handlers and asserting on what a real `IOrderExecutionListener` receives from a real `OrderExecutionBus`.

## Build

```bash
cmake -B build -DFLOX_BUILD_CONNECTORS=ON
cmake --build build --target flox-connectors
```

The result is `build/connectors/libflox-connectors.a`. To link against another target:

```cmake
target_link_libraries(my_target PRIVATE flox::connectors)
```

The Polymarket executor is a Rust FFI library. If `cargo` isn't on the path, the executor source is excluded from the build and a CMake warning surfaces instead of a configure error. Pass `-DFLOX_ENABLE_POLYMARKET_ORDER_EXECUTOR=OFF` to silence the warning when Rust isn't desired.

## Why monorepo

The connector code used to live in a separate `flox-connectors` repo, pulled in as a git submodule. Three reasons it moved:

- Cross-repo changes (a new event type in core needing a new field on the connector side) cost two PRs with submodule pinning. For a one-contributor project, the overhead is pure friction.
- AI agents reading FLOX without recursing into submodules saw zero connector code, which made the polyglot positioning weaker than it actually was.
- Single dependency policy. `connectors/CMakeLists.txt` now uses the same `FetchContent` mechanism the parent uses for `lz4` and `tracy`, instead of bundling its own external/ submodule tree.

The trade-off: every push that touches connectors runs the full flox CI. The `connectors-build` job stays minimal (no tests, no platform matrix beyond ubuntu) so the cost is bounded.

## Tests

`connectors/tests/` holds two kinds of test:

- `unit_test_*.cpp` are offline — no sockets, no exchange credentials. Order-serialization tests fake the transport (`ITransport`) or the client-provided injection point and assert on the exact request body a connector builds; protocol tests feed raw WebSocket frames straight into a connector's message handler. They build and register with `ctest` whenever `FLOX_BUILD_CONNECTORS=ON` and `FLOX_BUILD_TESTS=ON` — no extra flag — and run in the default CI matrix.
- `integration_test_*.cpp` connect to real exchange WebSocket endpoints. Building them is gated by both `FLOX_BUILD_TESTS=ON` and `FLOX_BUILD_CONNECTOR_INTEGRATION_TESTS=ON`; running them via `ctest` requires the further `FLOX_RUN_CONNECTOR_INTEGRATION_TESTS=ON`. The default flox CI build skips them entirely.

To build the integration tests locally:

```bash
cmake -B build -DFLOX_BUILD_CONNECTORS=ON \
                -DFLOX_BUILD_TESTS=ON \
                -DFLOX_BUILD_CONNECTOR_INTEGRATION_TESTS=ON
cmake --build build
```

## Hyperliquid signing daemon

Hyperliquid uses an off-process Python daemon for order signing — the C++ executor talks to it over a local socket. The daemon wraps the official [`hyperliquid-python-sdk`](https://github.com/hyperliquid-dex/hyperliquid-python-sdk):

```bash
pip install git+https://github.com/hyperliquid-dex/hyperliquid-python-sdk.git
python3 connectors/utils/hl_signerd.py
```

Out-of-process signing keeps the secret out of the trading binary's address space and avoids shipping a Rust crypto stack into every flox build.

## Adding a new venue

The expected pattern for a new connector:

1. Create `connectors/src/<venue>/` and `connectors/include/flox-connectors/<venue>/`.
2. Implement the venue-specific subclasses of the connector / executor abstractions in `flox::IExchangeConnector` (`flox/connector/abstract_exchange_connector.h`) and `flox::IOrderExecutor` (`flox/execution/abstract_executor.h`) — both live directly in namespace `flox`.
3. The CMake glob in `connectors/CMakeLists.txt` picks up new `.cpp` files automatically — no CMakeLists edit needed.
4. Add an integration test under `connectors/tests/integration_test_<venue>.cpp` (build-only by default).

The existing four adapters are concrete worked examples; a Hyperliquid-style off-process signer or a Polymarket-style Rust FFI is supported via the same gating pattern.
