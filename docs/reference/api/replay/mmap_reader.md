# MmapReader

`MmapReader` provides memory-mapped access to binary log segments for zero-copy reads. Cross-platform implementation supports both POSIX (Linux/macOS) and Windows.

```cpp
class MmapSegmentReader
{
public:
  explicit MmapSegmentReader(const std::filesystem::path& segment_path);
  ~MmapSegmentReader();

  MmapSegmentReader(MmapSegmentReader&&) noexcept;
  MmapSegmentReader& operator=(MmapSegmentReader&&) noexcept;

  bool isValid() const;
  bool isCompressed() const;
  bool hasIndex() const;

  const SegmentHeader& header() const;

  const std::byte* data() const;
  size_t dataSize() const;
  size_t totalSize() const;

  bool next(ReplayEvent& out);
  void reset();
  bool seekToTimestamp(int64_t target_ts_ns);
  size_t position() const;

  const FrameHeader* currentFrame() const;
  bool advanceFrame();

  bool loadIndex();
  const std::vector<IndexEntry>& indexEntries() const;
};
```

## Purpose

* Provide zero-copy access to segment data via memory mapping.
* Enable direct pointer access to event records without buffer copying.
* Support fast timestamp-based seeking using segment indexes.

## MmapReader (Multi-Segment)

```cpp
class MmapReader
{
public:
  struct Config
  {
    std::filesystem::path data_dir;
    bool preload_index{true};
    bool prefault_pages{false};
    std::optional<int64_t> from_ns;
    std::optional<int64_t> to_ns;
    std::set<uint32_t> symbols;
  };

  explicit MmapReader(const Config& config);

  using EventCallback = std::function<bool(const ReplayEvent&)>;
  uint64_t forEach(EventCallback callback);
  uint64_t forEachFrom(int64_t start_ts_ns, EventCallback callback);

  using RawTradeCallback = std::function<bool(const TradeRecord*)>;
  uint64_t forEachRawTrade(RawTradeCallback callback);

  MmapReaderStats stats() const;
  const std::vector<SegmentInfo>& segments() const;
  uint64_t totalEvents() const;
};
```

## Configuration

| Field          | Type                 | Description                              |
|----------------|----------------------|------------------------------------------|
| data_dir       | `filesystem::path`   | Directory containing segments            |
| preload_index  | `bool`               | Load indexes on construction             |
| prefault_pages | `bool`               | Touch pages to prefault into memory      |
| from_ns        | `optional<int64_t>`  | Start timestamp filter                   |
| to_ns          | `optional<int64_t>`  | End timestamp filter                     |
| symbols        | `set<uint32_t>`      | Symbol IDs to include                    |

## Statistics

```cpp
struct MmapReaderStats
{
  uint64_t segments_mapped;
  uint64_t bytes_mapped;
  uint64_t events_read;
  uint64_t page_faults;
};
```

## Usage

```cpp
replay::MmapReader::Config config{
    .data_dir = "/data/market",
    .preload_index = true
};

replay::MmapReader reader(config);

reader.forEach([](const replay::ReplayEvent& event) {
    // Process event
    return true;
});

// Direct access to raw trade records (zero-copy)
reader.forEachRawTrade([](const replay::TradeRecord* trade) {
    // Access trade fields directly from mapped memory
    return true;
});
```

## Quick Count

```cpp
uint64_t count = replay::mmapCount("/data/market");
```

## Platform Support

| Platform | Implementation                                    |
|----------|---------------------------------------------------|
| Linux    | `mmap()` / `munmap()` / `madvise()`               |
| macOS    | `mmap()` / `munmap()` / `madvise()`               |
| Windows  | `CreateFileMapping()` / `MapViewOfFile()`         |

## Notes

* Memory-mapped I/O avoids kernel-to-userspace copies.
* Segments remain mapped until the reader is destroyed.
* `prefault_pages` can improve first-access latency at the cost of startup time.
* Does not support compressed segments (use `BinaryLogReader` for those).
