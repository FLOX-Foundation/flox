# Connectors

Native exchange connectors for the FLOX engine.

Adapters in this directory:

- **Bybit V5** — WebSocket + REST executor.
- **Bitget V2** — WebSocket + classic account REST executor.
- **Hyperliquid** — WebSocket + REST executor (uses `utils/hl_signerd.py` as the signing daemon).
- **Polymarket** — WebSocket + Rust FFI executor.

## Build

The connectors are part of the FLOX repo and are built when the parent project is configured with `-DFLOX_BUILD_CONNECTORS=ON`. The static library exposes the target `flox::connectors` (alias of `flox-connectors`).

```bash
cmake -B build -DFLOX_BUILD_CONNECTORS=ON
cmake --build build --target flox-connectors
```

`ixwebsocket` and `simdjson` come in via FetchContent at configure time, so a fresh clone needs no submodules. System dependencies: OpenSSL, zlib, libcurl. The Polymarket executor additionally needs the Rust toolchain (cargo); pass `-DFLOX_ENABLE_POLYMARKET_ORDER_EXECUTOR=OFF` to skip it.

## Tests

The integration tests under `tests/` connect to live exchange endpoints, so they are not part of the default ctest run. To build them:

```bash
cmake -B build -DFLOX_BUILD_CONNECTORS=ON \
                -DFLOX_ENABLE_TESTS=ON \
                -DFLOX_BUILD_CONNECTOR_INTEGRATION_TESTS=ON
cmake --build build
```

To register them with `ctest` (still requires network access at runtime):

```bash
cmake -B build ... -DFLOX_RUN_CONNECTOR_INTEGRATION_TESTS=ON
```

## Hyperliquid signing daemon

Hyperliquid orders are signed via a small Python daemon that wraps the official [`hyperliquid-python-sdk`](https://github.com/hyperliquid-dex/hyperliquid-python-sdk):

```bash
pip install git+https://github.com/hyperliquid-dex/hyperliquid-python-sdk.git
python3 connectors/utils/hl_signerd.py
```

The C++ executor talks to it over a local socket; see `hyperliquid_order_executor.h` for the protocol.

## Polymarket configuration

Polymarket trading requires a proxy-wallet setup:

1. Connect a wallet at <https://polymarket.com> and enable trading. This creates a proxy wallet on Polygon.
2. Deposit USDC to fund the proxy wallet.
3. Note the proxy wallet address (account settings or API).

Pass the trading wallet's private key and the proxy wallet address into `PolymarketOrderExecutor`:

```cpp
#include <flox-connectors/polymarket/polymarket_order_executor.h>

flox::PolymarketOrderExecutor executor(
    "0xYOUR_PRIVATE_KEY_HEX",      // trading wallet private key
    "0xYOUR_PROXY_WALLET_ADDRESS", // proxy / funder wallet
    logger
);

if (executor.init()) {
    executor.warmup();
    executor.prefetch(tokenId);
    auto result = executor.buy(tokenId, Volume::fromDouble(10.0));
}
```

Recommended: keep credentials in env vars (`PM_PRIVATE_KEY`, `PM_FUNDER_WALLET`) rather than in source.

## License

MIT — same as FLOX.

## Disclaimer

The authors are not affiliated with any exchange listed above and accept no responsibility for trading losses or regulatory issues arising from use of this code. Comply with each venue's terms of service.
