[![CI](https://github.com/flox-foundation/flox/actions/workflows/ci.yml/badge.svg)](https://github.com/flox-foundation/flox/actions)
[![Docs](https://img.shields.io/badge/docs-site-blue)](https://flox-foundation.github.io/flox)
[![PyPI](https://img.shields.io/pypi/v/flox-py?v=1)](https://pypi.org/project/flox-py/)
[![npm](https://img.shields.io/npm/v/@flox-foundation/flox)](https://www.npmjs.com/package/@flox-foundation/flox)

## FLOX

FLOX is a modular framework for building trading systems, written in modern C++.
It provides low-level infrastructure for execution pipelines, market data processing, strategy logic, backtesting, and exchange integration.

Documentation is available at [flox-foundation.github.io/flox](https://flox-foundation.github.io/flox)

## Language bindings

| Language | Install | Docs |
|----------|---------|------|
| Python | `pip install flox-py` | [reference](https://flox-foundation.github.io/flox/reference/python/) |
| Node.js | `npm install @flox-foundation/flox` | [reference](https://flox-foundation.github.io/flox/reference/node/) |
| Codon | build from source | [reference](https://flox-foundation.github.io/flox/reference/codon/) |
| JavaScript (embedded) | bundled with C++ build | [reference](https://flox-foundation.github.io/flox/reference/quickjs/) |
| C API | `libflox_capi.so` | [reference](https://flox-foundation.github.io/flox/reference/api/capi/flox_capi/) |

All bindings expose the same strategy API, indicators, order books, backtesting, and data I/O. The C API is the integration point for adding support for any other language.

## AI companion

`flox-mcp` is a Model Context Protocol server that gives AI coding agents (Cursor, Claude Code, Cline) grounded access to the FLOX surface — symbol lookups across bindings, scaffolders, indicator and backtest tools, full-text doc search.

```bash
pip install flox-mcp
```

See [the package README](./mcp/README.md) for setup and the full tool list.

## Connectors

Native exchange connectors (Bybit, Bitget, Hyperliquid, Polymarket) live under [`connectors/`](./connectors/) and build with `-DFLOX_BUILD_CONNECTORS=ON`. The flag defaults to OFF so a backtest-only or research build doesn't pay the dependency cost. See the [connectors README](./connectors/README.md) for adapter notes and the optional Polymarket Rust toolchain step.

## Build options

The CMake options that gate every optional artefact (bindings, demo, tools, tests, benchmarks, connectors) are catalogued in [`docs/build/feature-flags.md`](https://flox-foundation.github.io/flox/build/feature-flags/). Defaults are OFF so a bare `cmake -B build` produces only the core C++ static library.

## Commercial Services

For commercial support, enterprise connectors, and custom development, visit [floxlabs.dev](https://floxlabs.dev).

## Contributing

Contributions are welcome via pull requests.
Please follow the existing structure and naming conventions.
Tests, benchmarks, and documentation should be included where appropriate.
Code style is enforced via `clang-format`. See [contributing guide](https://flox-foundation.github.io/flox/how-to/contributing/).

## License

FLOX is licensed under the MIT License. See [LICENSE](./LICENSE) for details.

## Disclaimer

FLOX is provided "as is" without warranty of any kind, express or implied.
The authors are not liable for any damages or financial harm arising from its use, including trading losses or system failures.
This software is intended for educational and research purposes. Production use is at your own risk.
See [DISCLAIMER.md](./DISCLAIMER.md) for full legal notice.
