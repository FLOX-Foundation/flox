[![CI](https://github.com/flox-foundation/flox-connectors/actions/workflows/ci.yml/badge.svg)](https://github.com/flox-foundation/flox-connectors/actions)

# flox-connectors

Open-source exchange connectors for the **Flox** engine.

Available adapters:

* **Bybit V5 WebSocket + REST executor**
* **Bitget V2 WebSocket + classic account REST executor**
* **Hyperliquid WebSocket + REST executor (must use utils/hl_signerd.py as a signing daemon)**
* **Polymarket WebSocket + Rust FFI executor**

## Dependencies

* C++23 compiler, CMake ≥ 3.20  
* **Submodules** (fetched with `--recurse-submodules`):  
  * [Flox](https://github.com/eeiaao/flox) – core engine interfaces  
  * [simdjson](https://github.com/simdjson/simdjson) – JSON parsing  
  * [IXWebSocket](https://github.com/machinezone/IXWebSocket) – TLS WebSocket client  
* System libs: OpenSSL, Zlib, pthread, curl
* For Hyperliquid connector:
  * python3
  * [hyperliquid-python-sdk](https://github.com/hyperliquid-dex/hyperliquid-python-sdk) (install via `pip install git+https://github.com/hyperliquid-dex/hyperliquid-python-sdk.git`)
* For Polymarket connector:
  * Rust toolchain (cargo)

Clone with:

```bash
git clone --recurse-submodules https://github.com/eeiaao/flox-connectors.git
```

## Build

```bash
mkdir build && cd build
cmake -DBUILD_TESTS=OFF ..    # set to ON to compile GoogleTest units
cmake --build . -j
```

The static library exports the target **`flox::flox-connectors`**, ready for `add_subdirectory` or `find_package`.

## Polymarket Configuration

The Polymarket connector requires a **proxy wallet** setup for trading. You need:

1. **Private Key** (`privateKey`) - Hex-encoded private key of your trading wallet (with or without `0x` prefix)
2. **Funder Wallet** (`funderWallet`) - Address of the proxy/funder wallet that holds USDC allowance (with `0x` prefix)

### Setting up a Proxy Wallet

1. Go to [polymarket.com](https://polymarket.com) and connect your wallet
2. Enable trading - this creates a proxy wallet on Polygon
3. Deposit USDC to fund your proxy wallet
4. Find your proxy wallet address in account settings or via the API

### Example Usage

```cpp
#include <flox-connectors/polymarket/polymarket_order_executor.h>

flox::PolymarketOrderExecutor executor(
    "0xYOUR_PRIVATE_KEY_HEX",           // Trading wallet private key
    "0xYOUR_PROXY_WALLET_ADDRESS",       // Funder/proxy wallet address
    logger
);

if (executor.init()) {
    executor.warmup();                   // Pre-establish TLS connections
    executor.prefetch(tokenId);          // Cache token metadata

    auto result = executor.buy(tokenId, Volume::fromDouble(10.0));  // Buy $10 worth
}
```

### Environment Variables (recommended)

Store credentials securely:

```bash
export PM_PRIVATE_KEY="0x..."
export PM_FUNDER_WALLET="0x..."
```

## Contributing

Contributions are welcome! Please follow the project's `.clang-format` style and keep pull requests focused.

## Commercial Services

For commercial support, enterprise connectors, and custom development services, visit [floxlabs.dev](https://floxlabs.dev)

## Disclaimer

This software is provided “as is”, without warranty of any kind.
The authors are not affiliated with Bybit or any other exchange and are not responsible for financial losses or regulatory issues arising from the use of this code. Use at your own risk and ensure compliance with each venue’s terms of service.
