[![CI](https://github.com/flox-foundation/flox/actions/workflows/ci.yml/badge.svg)](https://github.com/flox-foundation/flox/actions)
[![Docs](https://img.shields.io/badge/docs-site-blue)](https://flox-foundation.github.io/flox)

## FLOX

FLOX is a modular framework for building trading systems, written in modern C++.  
It provides low-level infrastructure for constructing execution pipelines, processing market data, managing strategy logic, and integrating with exchanges or storage backends. The framework is designed to be composable, testable, and suitable for both research and production environments.

📖 Documentation is available at [https://flox-foundation.github.io/flox](https://flox-foundation.github.io/flox)

## Connectors

The **open-source community implementations of connectors built on top of Flox** are maintained in the following repository: [https://github.com/FLOX-Foundation/flox-connectors](https://github.com/FLOX-Foundation/flox-connectors)

## Contribution

Contributions are welcome via pull requests.

Please follow the existing structure and naming conventions.  
Tests, benchmarks, and documentation should be included where appropriate.  
Code style is enforced via `clang-format`.

Pre-commit hook setup and formatting instructions are described in
[Contributing Guide](https://flox-foundation.github.io/flox/how-to/contributing/).


## License

FLOX is licensed under the MIT License.  
See the [`LICENSE`](./LICENSE) file for details.


## Disclaimer

FLOX is provided "as is" without warranty of any kind, express or implied.

The authors and contributors are **not liable** for any damages, losses, or financial harm arising from the use of this software, including but not limited to trading losses, system failures, or data corruption.

This software is intended for **educational and research purposes only**. Any use in production trading systems is entirely at your own risk. The authors make no guarantees regarding the correctness, reliability, or performance of the code.

By using this software, you acknowledge that you understand and accept these terms.

See [`DISCLAIMER.md`](./DISCLAIMER.md) for full legal notice.
