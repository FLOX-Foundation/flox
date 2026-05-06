# Connectors

FLOX ships native exchange connectors as part of the same repo. They live under [`connectors/`](https://github.com/FLOX-Foundation/flox/tree/main/connectors) and build into a single `flox::connectors` static library that links against `flox::flox`.

The module is gated by `FLOX_BUILD_CONNECTORS` in CMake. It is **off** by default — backtest-only and research builds skip the dependency cost (OpenSSL, libcurl, zlib, plus ixwebsocket and simdjson via FetchContent at configure time).

## Adapters in tree

| Venue | Trades | BBO | Book | Orders | Positions |
|---|---|---|---|---|---|
| Bybit (V5) | ✓ | ✓ | ✓ | ✓ | ✓ |
| Bitget (V2) | ✓ | ✓ | ✓ | ✓ | ✓ |
| Hyperliquid | ✓ | ✓ | ✓ | ✓ | ✓ |
| Polymarket | ✓ | ✓ | — | ✓ | — |

Each adapter sits under `connectors/src/<venue>/` with public headers under `connectors/include/flox-connectors/<venue>/`. The header layout uses the `flox-connectors/` prefix so consumers' include sites are stable across the repo move.

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

Connector tests are *integration* tests — they connect to real exchange WebSocket endpoints. Building them is gated by both `FLOX_BUILD_TESTS=ON` and `FLOX_BUILD_CONNECTOR_INTEGRATION_TESTS=ON`; running them via `ctest` requires the further `FLOX_RUN_CONNECTOR_INTEGRATION_TESTS=ON`. The default flox CI build skips them entirely.

To build them locally:

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
2. Implement the venue-specific subclasses of the connector / executor abstractions in `flox::book::IExchangeConnector` and `flox::execution::IOrderExecutor`.
3. The CMake glob in `connectors/CMakeLists.txt` picks up new `.cpp` files automatically — no CMakeLists edit needed.
4. Add an integration test under `connectors/tests/integration_test_<venue>.cpp` (build-only by default).

The existing four adapters are concrete worked examples; a Hyperliquid-style off-process signer or a Polymarket-style Rust FFI is supported via the same gating pattern.
