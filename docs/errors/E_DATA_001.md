---
code: E_DATA_001
title: Bar data not found
severity: error
since: 0.5.7
---

# E_DATA_001 — Bar data not found

`MmapBarStorage` couldn't find any `bars_*.bin` files in the symbol
directory. Either the directory doesn't exist, or it exists but
contains no recorded data.

## How to fix

Run a recorder over the dataset first:

=== "Python"
    ```python
    import flox

    rec = flox.MmapBarWriter("out/BTCUSDT")
    rec.set_metadata({"exchange": "binance", "kind": "perpetual"})

    for bar in your_data_source():
        rec.on_bar(bar)
    rec.flush()           # writes bars_60s.bin (or similar)
    rec.write_metadata()
    ```

Then read it back:

```python
storage = flox.MmapBarStorage("out/BTCUSDT")
```

## Common causes

- Pointing at the data root (`"out"`) instead of the per-symbol
  subdirectory (`"out/BTCUSDT"`).
- The recorder ran but `flush()` was never called — buffered bars never
  hit disk.
- The symbol directory was created (`MmapBarWriter` does this on init)
  but no bars were ever written.
