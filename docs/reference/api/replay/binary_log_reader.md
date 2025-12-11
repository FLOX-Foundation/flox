# BinaryLogReader

`BinaryLogReader` provides sequential access to market data stored in the binary log format. It handles segment discovery, time filtering, symbol filtering, and CRC verification.

```cpp
struct ReaderConfig
{
  std::filesystem::path data_dir;
  std::optional<int64_t> from_ns;
  std::optional<int64_t> to_ns;
  std::set<uint32_t> symbols;
  bool verify_crc{true};
};

class BinaryLogReader
{
public:
  explicit BinaryLogReader(ReaderConfig config);

  static DatasetSummary inspect(const std::filesystem::path& data_dir);
  static DatasetSummary inspectWithSymbols(const std::filesystem::path& data_dir);

  DatasetSummary summary();
  uint64_t count();
  std::set<uint32_t> availableSymbols();

  using EventCallback = std::function<bool(const ReplayEvent&)>;
  bool forEach(EventCallback callback);
  bool forEachFrom(int64_t start_ts_ns, EventCallback callback);

  std::optional<std::pair<int64_t, int64_t>> timeRange() const;
  ReaderStats stats() const;
  const std::vector<SegmentInfo>& segments() const;
};
```

## Purpose

* Read market data from binary log segments in chronological order.
* Filter events by time range and symbol set.
* Support both full scans and timestamp-based seeks.

## Configuration

| Field      | Type                    | Description                              |
|------------|-------------------------|------------------------------------------|
| data_dir   | `filesystem::path`      | Directory containing `.floxseg` files    |
| from_ns    | `optional<int64_t>`     | Start timestamp filter (inclusive)       |
| to_ns      | `optional<int64_t>`     | End timestamp filter (inclusive)         |
| symbols    | `set<uint32_t>`         | Symbol IDs to include (empty = all)      |
| verify_crc | `bool`                  | Verify CRC32 checksums (default: true)   |

## Core Methods

| Method             | Description                                              |
|--------------------|----------------------------------------------------------|
| `inspect()`        | Static scan of directory, returns metadata without reading events |
| `inspectWithSymbols()` | Like `inspect()` but also collects symbol IDs        |
| `summary()`        | Returns dataset metadata after scanning                  |
| `count()`          | Returns total event count across all segments            |
| `forEach()`        | Iterate all events matching filters                      |
| `forEachFrom()`    | Iterate events starting from a timestamp                 |
| `timeRange()`      | Returns (first_event_ns, last_event_ns) pair             |
| `stats()`          | Returns read statistics (events, bytes, errors)          |

## Event Structure

```cpp
struct ReplayEvent
{
  EventType type;           // Trade, BookSnapshot, or BookDelta
  int64_t timestamp_ns;     // Event timestamp

  TradeRecord trade;        // Populated for Trade events

  BookRecordHeader book_header;  // Populated for Book events
  std::vector<BookLevel> bids;
  std::vector<BookLevel> asks;
};
```

## Usage

```cpp
replay::ReaderConfig config{
    .data_dir = "/data/market",
    .from_ns = start_timestamp,
    .to_ns = end_timestamp,
    .symbols = {1, 2, 3}
};

replay::BinaryLogReader reader(config);

reader.forEach([](const replay::ReplayEvent& event) {
    if (event.type == replay::EventType::Trade) {
        // Process trade
    } else {
        // Process book update
    }
    return true;  // Continue iteration
});
```

## Statistics

```cpp
struct ReaderStats
{
  uint64_t files_read;
  uint64_t events_read;
  uint64_t trades_read;
  uint64_t book_updates_read;
  uint64_t bytes_read;
  uint64_t crc_errors;
};
```

## BinaryLogIterator

Low-level iterator for reading a single segment file:

```cpp
class BinaryLogIterator
{
public:
  explicit BinaryLogIterator(const std::filesystem::path& segment_path);

  bool next(ReplayEvent& out);
  bool seekToTimestamp(int64_t target_ts_ns);
  bool loadIndex();

  const SegmentHeader& header() const;
  bool isValid() const;
  bool isCompressed() const;
  bool hasIndex() const;
};
```

## Notes

* Segments are automatically discovered and sorted by timestamp.
* Compressed segments (LZ4) are transparently decompressed.
* Seeking uses segment indexes when available for O(log n) lookup.
* The callback returning `false` stops iteration early.
