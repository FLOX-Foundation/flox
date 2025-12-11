# SegmentOps

`SegmentOps` provides utilities for manipulating binary log segments: merging, splitting, exporting, filtering, and recompressing.

```cpp
class SegmentOps
{
public:
  using ProgressCallback = std::function<void(uint64_t current, uint64_t total)>;

  // Merge
  static MergeResult merge(const std::vector<std::filesystem::path>& input_paths,
                           const MergeConfig& config);
  static MergeResult merge(const std::vector<std::filesystem::path>& input_paths,
                           const MergeConfig& config, ProgressCallback progress);
  static MergeResult mergeDirectory(const std::filesystem::path& input_dir,
                                    const MergeConfig& config);

  // Split
  static SplitResult split(const std::filesystem::path& input_path,
                           const SplitConfig& config);
  static SplitResult split(const std::filesystem::path& input_path,
                           const SplitConfig& config, ProgressCallback progress);
  static SplitResult splitDirectory(const std::filesystem::path& input_dir,
                                    const SplitConfig& config);

  // Export
  static ExportResult exportData(const std::filesystem::path& input_path,
                                 const ExportConfig& config);
  static ExportResult exportData(const std::filesystem::path& input_path,
                                 const ExportConfig& config, ProgressCallback progress);
  static ExportResult exportDirectory(const std::filesystem::path& input_dir,
                                      const ExportConfig& config);

  // Other operations
  static bool recompress(const std::filesystem::path& input_path,
                         const std::filesystem::path& output_path,
                         CompressionType new_compression);

  static uint64_t filter(const std::filesystem::path& input_path,
                         const std::filesystem::path& output_path,
                         const std::function<bool(const ReplayEvent&)>& predicate,
                         const WriterConfig& output_config);

  static uint64_t extractSymbols(const std::filesystem::path& input_path,
                                 const std::filesystem::path& output_path,
                                 const std::set<uint32_t>& symbols,
                                 const WriterConfig& config);

  static uint64_t extractTimeRange(const std::filesystem::path& input_path,
                                   const std::filesystem::path& output_path,
                                   int64_t from_ns, int64_t to_ns,
                                   const WriterConfig& config);
};
```

## Merge

Combines multiple segments into a single file, optionally sorting by timestamp.

```cpp
struct MergeConfig
{
  std::filesystem::path output_dir;
  std::string output_name;
  bool create_index{true};
  uint16_t index_interval{kDefaultIndexInterval};  // 1000
  CompressionType compression{CompressionType::None};
  bool preserve_timestamps{true};
  bool sort_by_timestamp{true};
  uint64_t max_output_size{0};  // 0 = no limit
};

struct MergeResult
{
  bool success{false};
  std::filesystem::path output_path;
  uint32_t segments_merged{0};
  uint64_t events_written{0};
  uint64_t bytes_written{0};
  std::vector<std::string> errors;
};
```

### Usage

```cpp
MergeConfig config{
    .output_dir = "/data/merged",
    .output_name = "combined",
    .compression = CompressionType::LZ4
};

auto result = SegmentOps::mergeDirectory("/data/segments", config);
```

## Split

Divides a segment into multiple files by time, event count, size, or symbol.

```cpp
enum class SplitMode
{
  ByTime,        // Split at time boundaries
  ByEventCount,  // Split after N events
  BySize,        // Split at size threshold
  BySymbol,      // One file per symbol
};

struct SplitConfig
{
  std::filesystem::path output_dir;
  SplitMode mode{SplitMode::ByTime};

  int64_t time_interval_ns{3600LL * 1000000000LL};  // 1 hour
  uint64_t events_per_file{1000000};
  uint64_t bytes_per_file{256ull << 20};            // 256 MB

  bool create_index{true};
  uint16_t index_interval{kDefaultIndexInterval};   // 1000
  CompressionType compression{CompressionType::None};
};

struct SplitResult
{
  bool success{false};
  std::vector<std::filesystem::path> output_paths;
  uint32_t segments_created{0};
  uint64_t events_written{0};
  std::vector<std::string> errors;
};
```

### Usage

```cpp
SplitConfig config{
    .output_dir = "/data/hourly",
    .mode = SplitMode::ByTime,
    .time_interval_ns = 3600LL * 1000000000LL
};

auto result = SegmentOps::split("/data/large.floxlog", config);
```

## Export

Converts binary logs to human-readable formats.

```cpp
enum class ExportFormat
{
  CSV,
  JSON,
  JSONLines,
  Binary,  // Copy with optional filtering
};

struct ExportConfig
{
  std::filesystem::path output_path;
  ExportFormat format{ExportFormat::CSV};

  // Filtering
  std::optional<int64_t> from_ts;
  std::optional<int64_t> to_ts;
  std::set<uint32_t> symbols;
  bool trades_only{false};
  bool books_only{false};

  // CSV options
  char delimiter{','};
  bool include_header{true};

  // JSON options
  bool pretty_print{false};
  int indent{2};

  // Binary format options (for ExportFormat::Binary)
  CompressionType compression{CompressionType::None};
  bool create_index{true};
  uint16_t index_interval{kDefaultIndexInterval};  // 1000
};

struct ExportResult
{
  bool success{false};
  std::filesystem::path output_path;
  uint64_t events_exported{0};
  uint64_t bytes_written{0};
  std::vector<std::string> errors;
};
```

### Usage

```cpp
ExportConfig config{
    .output_path = "/data/trades.csv",
    .format = ExportFormat::CSV,
    .trades_only = true
};

auto result = SegmentOps::exportData("/data/market.floxlog", config);
```

## Other Operations

### Recompress

Change compression of an existing segment:

```cpp
SegmentOps::recompress("/data/uncompressed.floxlog",
                       "/data/compressed.floxlog",
                       CompressionType::LZ4);
```

### Filter

Apply custom predicate to filter events:

```cpp
WriterConfig writer_config{.output_dir = "/data/filtered"};

auto count = SegmentOps::filter(
    "/data/input.floxlog",
    "/data/filtered/trades.floxlog",
    [](const ReplayEvent& e) { return e.type == EventType::Trade; },
    writer_config);
```

### Extract Symbols

Extract events for specific symbol IDs:

```cpp
WriterConfig writer_config{.output_dir = "/data/extracted"};

auto count = SegmentOps::extractSymbols(
    "/data/input.floxlog",
    "/data/extracted/btc_eth.floxlog",
    {1, 2},  // Symbol IDs
    writer_config);
```

### Extract Time Range

Extract events within a time window:

```cpp
WriterConfig writer_config{.output_dir = "/data/extracted"};

auto count = SegmentOps::extractTimeRange(
    "/data/input.floxlog",
    "/data/extracted/morning.floxlog",
    start_ns, end_ns,
    writer_config);
```

## Convenience Functions

```cpp
// Quick merge all segments in directory
auto result = replay::quickMerge("/data/segments", "/data/merged");

// Quick export to CSV
auto result = replay::quickExportCSV("/data/market.floxlog", "/data/market.csv");

// Split by hour
auto result = replay::quickSplitByHour("/data/day.floxlog", "/data/hourly");
```

## Notes

* All operations support progress callbacks for monitoring long-running tasks.
* Merge with `sort_by_timestamp=true` performs k-way merge sort.
* Export formats support filtering by time, symbols, and event type.
* Compression can be changed during any operation.
* `SplitMode::BySymbol` creates exactly one output file per unique symbol ID.
