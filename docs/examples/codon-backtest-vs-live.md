# Codon — backtest & live

The same `SMAStrategy` class runs in backtest, synchronous live, and threaded live modes. Identical logic to the [Python example](python-backtest-vs-live.md), compiled to native.

```bash
codon build -exe -o build/codon/codon_backtest_vs_live \
  -L build/src/capi -lflox_capi docs/examples/codon_backtest_vs_live.codon

DYLD_LIBRARY_PATH=build/src/capi:~/.local/lib/codon \
  ./build/codon/codon_backtest_vs_live
```

```python
--8<-- "examples/codon_backtest_vs_live.codon"
```
