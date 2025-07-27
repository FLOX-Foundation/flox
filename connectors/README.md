[![CI](https://github.com/flox-foundation/flox-connectors/actions/workflows/ci.yml/badge.svg)](https://github.com/flox-foundation/flox-connectors/actions)

# flox-connectors

Open-source exchange connectors for the **Flox** engine.

Available adapters:

* **Bybit V5 WebSocket**

## Dependencies

* C++23 compiler, CMake ≥ 3.20  
* **Submodules** (fetched with `--recurse-submodules`):  
  * [Flox](https://github.com/eeiaao/flox) – core engine interfaces  
  * [simdjson](https://github.com/simdjson/simdjson) – JSON parsing  
  * [IXWebSocket](https://github.com/machinezone/IXWebSocket) – TLS WebSocket client  
* System libs: OpenSSL, Zlib, pthread, curl

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

## Contributing

Contributions are welcome! Please follow the project’s `.clang-format` style and keep pull requests focused.

## Disclaimer

This software is provided “as is”, without warranty of any kind.
The authors are not affiliated with Bybit or any other exchange and are not responsible for financial losses or regulatory issues arising from the use of this code. Use at your own risk and ensure compliance with each venue’s terms of service.
