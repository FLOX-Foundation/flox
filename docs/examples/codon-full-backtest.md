# Codon — full API

Exercises the Codon API end-to-end on synthetic data: streaming indicators, `SimulatedExecutor`, position tracking, order books, volume profile, footprint bars, statistics. No CSV required.

```bash
codon build -exe -o build/codon/codon_full_backtest \
  -L build/src/capi -lflox_capi docs/examples/codon_full_backtest.codon

DYLD_LIBRARY_PATH=build/src/capi:~/.local/lib/codon \
  ./build/codon/codon_full_backtest
```

```python
--8<-- "examples/codon_full_backtest.codon"
```
