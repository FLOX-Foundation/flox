# Python — backtest & live

The same `SMAStrategy` class runs unchanged in three modes:

1. **BacktestRunner** — replays a CSV through the strategy; `SimulatedExecutor` handles fills
2. **Runner** (sync) — push ticks from your connector; callbacks fire before the call returns
3. **Runner(threaded=True)** — events go into a lock-free ring buffer; strategy runs in a background C++ thread

```
cd /path/to/flox
PYTHONPATH=build/python python3 docs/examples/python_backtest_vs_live.py
```

```python
--8<-- "examples/python_backtest_vs_live.py"
```
