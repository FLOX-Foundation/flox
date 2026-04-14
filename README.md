[![CI](https://github.com/flox-foundation/flox/actions/workflows/ci.yml/badge.svg)](https://github.com/flox-foundation/flox/actions)
[![Docs](https://img.shields.io/badge/docs-site-blue)](https://flox-foundation.github.io/flox)
[![PyPI](https://img.shields.io/pypi/v/flox-py?v=1)](https://pypi.org/project/flox-py/)

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

## Codon

[Codon](https://github.com/exaloop/codon) compiles Python-like code to native binaries.
Flox Codon bindings let you write strategies in Python syntax compiled to native code.

```python
from flox.strategy import Strategy
from flox.context import SymbolContext
from flox.types import TradeData
from flox.indicators import StreamingSMA

class SmaCrossover(Strategy):
    fast_sma: StreamingSMA
    slow_sma: StreamingSMA

    def __init__(self, symbols: List[int]):
        super().__init__(symbols)
        self.fast_sma = StreamingSMA(10)
        self.slow_sma = StreamingSMA(30)

    def on_trade(self, ctx: SymbolContext, trade: TradeData):
        fast = self.fast_sma.update(trade.price.to_double())
        slow = self.slow_sma.update(trade.price.to_double())
        if self.slow_sma.ready and fast > slow and ctx.is_flat():
            self.emit_market_buy(self.primary_symbol, 1.0)
```

```bash
codon build -exe -o strategy -lflox_capi my_strategy.codon
```

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
