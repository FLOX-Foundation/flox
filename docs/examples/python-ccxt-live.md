# Python — live via ccxt

Connects to Binance public WebSocket via `ccxt.pro`, streams BTC/USDT trades in real time, and runs an SMA crossover strategy. No API key required.

```
pip install ccxt
cd /path/to/flox
PYTHONPATH=build/python python3 docs/examples/python_ccxt_live.py
```

Stop with `Ctrl+C`.

```python
--8<-- "examples/python_ccxt_live.py"
```
