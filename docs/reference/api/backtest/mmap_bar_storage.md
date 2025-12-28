# MmapBarStorage

Memory-mapped storage for efficient bar data access.

## Overview

`MmapBarStorage` provides zero-copy access to pre-computed bar data stored in binary files. Uses memory-mapping for optimal performance with large datasets.

## Header

```cpp
#include "flox/backtest/mmap_bar_storage.h"
```

## Class Definition

```cpp
class MmapBarStorage {
public:
  explicit MmapBarStorage(const std::filesystem::path& symbolDir);
  ~MmapBarStorage();

  // Move-only
  MmapBarStorage(MmapBarStorage&&) noexcept;
  MmapBarStorage& operator=(MmapBarStorage&&) noexcept;

  // Access methods
  const Bar* getBar(TimeframeId tf, size_t index) const;
  const Bar* findBar(TimeframeId tf, TimePoint time, char mode = 'e') const;
  std::span<const Bar> getBars(TimeframeId tf) const;

  // Query methods
  size_t barCount(TimeframeId tf) const;
  size_t totalBars() const;
  std::pair<TimePoint, TimePoint> timeRange() const;
  std::vector<TimeframeId> timeframes() const;
};
```

## Constructor

```cpp
explicit MmapBarStorage(const std::filesystem::path& symbolDir);
```

Opens all `bars_*.bin` files in the directory. File naming convention:
- `bars_60s.bin` - 60-second bars
- `bars_300s.bin` - 5-minute bars
- `bars_3600s.bin` - 1-hour bars

**Throws:** `std::runtime_error` if directory doesn't exist or no bar files found.

## File Format

Each `.bin` file:
```
[uint64_t bar_count][Bar][Bar][Bar]...
```

The `Bar` struct is written directly in binary format.

## Methods

### getBar

```cpp
const Bar* getBar(TimeframeId tf, size_t index) const;
```

Get bar by index. Returns `nullptr` if timeframe not found or index out of bounds.

**Performance:** O(1), no copy.

### findBar

```cpp
const Bar* findBar(TimeframeId tf, TimePoint time, char mode = 'e') const;
```

Find bar by time with search mode:
- `'e'` - Exact match only
- `'b'` - Bar before (or at) time
- `'a'` - Bar at or after time

**Performance:** O(log n) binary search.

### getBars

```cpp
std::span<const Bar> getBars(TimeframeId tf) const;
```

Get all bars for timeframe as span. Zero-copy access to memory-mapped data.

### barCount / totalBars

```cpp
size_t barCount(TimeframeId tf) const;
size_t totalBars() const;
```

Get bar count for specific timeframe or total across all timeframes.

### timeRange

```cpp
std::pair<TimePoint, TimePoint> timeRange() const;
```

Get earliest and latest bar times across all timeframes.

### timeframes

```cpp
std::vector<TimeframeId> timeframes() const;
```

List all available timeframes.

## Example

```cpp
// Open bar storage
MmapBarStorage storage("/data/BTCUSDT");

// Get available timeframes
for (auto tf : storage.timeframes()) {
  std::cout << "TF: " << tf.seconds() << "s, "
            << storage.barCount(tf) << " bars\n";
}

// Iterate 1-minute bars
auto tf1m = TimeframeId::time(std::chrono::seconds(60));
for (const auto& bar : storage.getBars(tf1m)) {
  double vwap = (bar.high.toDouble() + bar.low.toDouble() +
                 bar.close.toDouble()) / 3.0;
  // ...
}

// Find specific bar
auto time = TimePoint(std::chrono::milliseconds(1703980800000));
if (auto* bar = storage.findBar(tf1m, time, 'b')) {
  std::cout << "Close: " << bar->close.toDouble() << "\n";
}
```

## Platform Support

Uses platform-specific memory mapping:
- Linux/macOS: `mmap()`
- Windows: `CreateFileMapping()` + `MapViewOfFile()`

## See Also

- [Bar](../aggregator/bar.md)
