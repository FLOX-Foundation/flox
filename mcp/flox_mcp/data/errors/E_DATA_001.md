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

`MmapBarStorage` and `MmapBarWriter` are C++-only — neither is bound in
Python, Node.js, Codon, or QuickJS — so this code reaches you either
from C++ or from the `preagg_bars` tool.

## How to fix

Pre-aggregate the raw tape into bar files first. `preagg_bars` does it
for a whole dataset and writes one file per timeframe:

```bash
cmake -B build -DFLOX_BUILD_TOOLS=ON -DFLOX_ENABLE_BACKTEST=ON
cmake --build build

./build/tools/preagg_bars /data/bybit/BTCUSDT /data/bybit/BTCUSDT/bars 60 300 900 3600
# bars_60s.bin, bars_300s.bin, bars_900s.bin, bars_3600s.bin
```

Then point the storage at the directory the tool wrote, not at the tape
directory it read:

```cpp
#include "flox/backtest/mmap_bar_storage.h"

flox::MmapBarStorage storage("/data/bybit/BTCUSDT/bars");
```

To produce the files from your own C++ code instead, subscribe an
`MmapBarWriter` to the bar bus and flush it before the process exits:

```cpp
#include "flox/backtest/mmap_bar_writer.h"

flox::MmapBarWriter writer("/data/bybit/BTCUSDT/bars");
writer.setMetadata({{"exchange", "bybit"}, {"kind", "perpetual"}});
bus.subscribe(&writer);

// ... run the aggregator ...

writer.flush();          // writes bars_60s.bin (or similar)
writer.writeMetadata();  // writes .symbol_metadata
```

There is no mmap bar path from the bindings at all. From Python or
Node.js, aggregate the trade arrays in-process — `flox.aggregate_time_bars`
and friends — and feed the result straight to `run_bars` / `runBars`;
no bar file is involved. See [Bar aggregation](../how-to/bar-aggregation.md).

## Common causes

- Pointing at the tape directory (`"/data/bybit/BTCUSDT"`) instead of
  the bar directory `preagg_bars` wrote (`"/data/bybit/BTCUSDT/bars"`).
- The writer ran but `flush()` was never called — buffered bars never
  hit disk.
- The symbol directory was created (`MmapBarWriter` does this on init)
  but no bars were ever written.
- Only non-time bars were requested, so nothing time-based landed in the
  directory — see [E_BACKTEST_001](E_BACKTEST_001.md).
