[![CI](https://github.com/flox-foundation/flox/actions/workflows/ci.yml/badge.svg)](https://github.com/flox-foundation/flox/actions)
[![Docs](https://img.shields.io/badge/docs-site-blue)](https://flox-foundation.github.io/flox)
[![PyPI](https://img.shields.io/pypi/v/flox-py)](https://pypi.org/project/flox-py/)

## FLOX

FLOX is a modular framework for building trading systems, written in modern C++.
It provides low-level infrastructure for execution pipelines, market data processing, strategy logic, backtesting, and exchange integration.

Documentation is available at [flox-foundation.github.io/flox](https://flox-foundation.github.io/flox)

## Python

```bash
pip install flox-py
```

```python
import flox_py as flox
import numpy as np

ema = flox.ema(close, 50)
atr = flox.atr(high, low, close, 14)
rsi = flox.rsi(close, 14)

returns = flox.bar_returns(sig_long, sig_short, log_ret)
pf = flox.profit_factor(returns)
```

Full API reference in [documentation](https://flox-foundation.github.io/flox/reference/python/).

## Connectors

The open-source community connector implementations are maintained in [flox-connectors](https://github.com/FLOX-Foundation/flox-connectors).

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
