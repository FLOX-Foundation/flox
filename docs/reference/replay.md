# Replay System Reference

Recording, playback, and segment operations.

## Binary Format

FLOX uses a custom binary format (`.floxlog`) optimized for:
- Sequential writes (recording)
- Sequential and indexed reads (replay)
- Optional LZ4 compression
- CRC32 integrity verification

### File Structure

```
┌──────────────────────────────────────┐
│  SegmentHeader (64 bytes)            │
├──────────────────────────────────────┤
│  Frame 1 (FrameHeader + payload)     │
│  Frame 2                              │
│  ...                                  │
│  Frame N                              │
├──────────────────────────────────────┤
│  Index Section (optional)            │
│  - SegmentIndexHeader (32 bytes)     │
│  - IndexEntry[] (16 bytes each)      │
└──────────────────────────────────────┘
```

### SegmentHeader

**Header:** `flox/replay/binary_format_v1.h`

```cpp
struct alignas(8) SegmentHeader  // 64 bytes
{
  uint32_t magic{0x584F4C46};     // "FLOX"
  uint16_t version{1};
  uint8_t flags{0};               // HasIndex, Compressed, Encrypted
  uint8_t exchange_id{0};
  int64_t created_ns{0};
  int64_t first_event_ns{0};
  int64_t last_event_ns{0};
  uint32_t event_count{0};
  uint32_t symbol_count{0};
  uint64_t index_offset{0};
  uint8_t compression{0};         // CompressionType
  uint8_t reserved[15]{};

  bool isValid() const;
  bool hasIndex() const;
  bool isCompressed() const;
  CompressionType compressionType() const;
};
```

### Flags

```cpp
namespace SegmentFlags {
  inline constexpr uint8_t HasIndex = 0x01;
  inline constexpr uint8_t Compressed = 0x02;
  inline constexpr uint8_t Encrypted = 0x04;  // Reserved
}

enum class CompressionType : uint8_t {
  None = 0,
  LZ4 = 1
};
```

### Event Types

```cpp
enum class EventType : uint8_t {
  Trade = 1,
  BookSnapshot = 2,
  BookDelta = 3
};
```

---

## Record Structures

### TradeRecord (48 bytes)

```cpp
struct alignas(8) TradeRecord
{
  int64_t exchange_ts_ns{0};
  int64_t recv_ts_ns{0};
  int64_t price_raw{0};         // Fixed-point
  int64_t qty_raw{0};           // Fixed-point
  uint64_t trade_id{0};
  uint32_t symbol_id{0};
  uint8_t side{0};              // 0=sell, 1=buy
  uint8_t instrument{0};        // InstrumentType
  uint16_t reserved{0};
};
```

### BookRecordHeader (40 bytes)

```cpp
struct alignas(8) BookRecordHeader
{
  int64_t exchange_ts_ns{0};
  int64_t recv_ts_ns{0};
  int64_t seq{0};
  uint32_t symbol_id{0};
  uint16_t bid_count{0};
  uint16_t ask_count{0};
  uint8_t type{0};              // SNAPSHOT or DELTA
  uint8_t instrument{0};
  uint16_t reserved{0};
  uint32_t _pad{0};
};

// Followed by:
// - BookLevel[bid_count]
// - BookLevel[ask_count]
```

### BookLevel (16 bytes)

```cpp
struct alignas(8) BookLevel
{
  int64_t price_raw{0};
  int64_t qty_raw{0};
};
```

---

## BinaryLogWriter

**Header:** `flox/replay/writers/binary_log_writer.h`

Writes market data to `.floxlog` files.

### Configuration

```cpp
struct WriterConfig
{
  std::filesystem::path output_dir;
  std::string output_filename;          // Optional: override auto-generated name
  uint64_t max_segment_bytes{256<<20};  // 256 MB
  uint64_t buffer_size{64<<10};         // 64 KB
  uint8_t exchange_id{0};
  bool sync_on_rotate{true};            // fsync before rotation
  bool create_index{true};
  uint16_t index_interval{1000};        // Events between index entries
  CompressionType compression{None};
};
```

### API

```cpp
class BinaryLogWriter
{
public:
  explicit BinaryLogWriter(WriterConfig config);
  ~BinaryLogWriter();

  bool writeTrade(const TradeRecord& trade);
  bool writeBook(const BookRecordHeader& header,
                 std::span<const BookLevel> bids,
                 std::span<const BookLevel> asks);

  void flush();
  void close();

  WriterStats stats() const;
  std::filesystem::path currentSegmentPath() const;
};

struct WriterStats
{
  uint64_t bytes_written{0};
  uint64_t events_written{0};
  uint64_t segments_created{0};
  uint64_t trades_written{0};
  uint64_t book_updates_written{0};
  uint64_t blocks_written{0};
  uint64_t uncompressed_bytes{0};
  uint64_t compressed_bytes{0};
};
```

---

## BinaryLogReader

**Header:** `flox/replay/readers/binary_log_reader.h`

Reads market data from `.floxlog` files.

### Configuration

```cpp
struct ReaderConfig
{
  std::filesystem::path data_dir;
  std::optional<int64_t> from_ns;   // Time range filter
  std::optional<int64_t> to_ns;
  std::set<uint32_t> symbols;       // Symbol filter
  bool verify_crc{true};
};
```

### API

```cpp
class BinaryLogReader
{
public:
  explicit BinaryLogReader(ReaderConfig config);
  ~BinaryLogReader();

  // Static inspection (no full scan)
  static DatasetSummary inspect(const std::filesystem::path& data_dir);
  static DatasetSummary inspectWithSymbols(const std::filesystem::path& data_dir);

  // Instance methods
  DatasetSummary summary();
  uint64_t count();
  std::set<uint32_t> availableSymbols();

  // Iteration
  using EventCallback = std::function<bool(const ReplayEvent&)>;
  bool forEach(EventCallback callback);
  bool forEachFrom(int64_t start_ts_ns, EventCallback callback);

  // Metadata
  std::optional<std::pair<int64_t, int64_t>> timeRange() const;
  ReaderStats stats() const;
  std::vector<std::filesystem::path> segmentFiles() const;
  const std::vector<SegmentInfo>& segments() const;
};
```

### DatasetSummary

```cpp
struct DatasetSummary
{
  std::filesystem::path data_dir;
  int64_t first_event_ns{0};
  int64_t last_event_ns{0};
  uint64_t total_events{0};
  uint32_t segment_count{0};
  uint64_t total_bytes{0};
  std::set<uint32_t> symbols;
  uint32_t segments_with_index{0};
  uint32_t segments_without_index{0};

  bool empty() const;
  std::chrono::nanoseconds duration() const;
  double durationSeconds() const;
  double durationMinutes() const;
  double durationHours() const;
  bool fullyIndexed() const;
};
```

### ReplayEvent

```cpp
struct ReplayEvent
{
  EventType type{};
  int64_t timestamp_ns{0};

  TradeRecord trade{};              // If type == Trade

  BookRecordHeader book_header{};   // If type == Book*
  std::vector<BookLevel> bids;
  std::vector<BookLevel> asks;
};
```

---

## BinaryLogIterator

**Header:** `flox/replay/readers/binary_log_reader.h`

Low-level single-segment iterator.

```cpp
class BinaryLogIterator
{
public:
  explicit BinaryLogIterator(const std::filesystem::path& segment_path);
  ~BinaryLogIterator();

  bool next(ReplayEvent& out);

  const SegmentHeader& header() const;
  bool isValid() const;
  bool isCompressed() const;

  bool seekToTimestamp(int64_t target_ts_ns);
  bool loadIndex();
  bool hasIndex() const;
};
```

---

## ReplayConnector

**Header:** `flox/replay/replay_connector.h`

Replays recorded data as if it were live.

```cpp
struct ReplayConnectorConfig
{
  std::filesystem::path data_dir;
  ReplaySpeed speed{ReplaySpeed::max()};
  std::optional<int64_t> from_ns;
  std::optional<int64_t> to_ns;
  std::set<uint32_t> symbols;
};

class ReplayConnector : public IReplaySource
{
public:
  explicit ReplayConnector(ReplayConnectorConfig config);
  ~ReplayConnector() override;

  void start() override;
  void stop() override;
  std::string exchangeId() const override;  // Returns "replay"

  // Replay control
  std::optional<TimeRange> dataRange() const override;
  void setSpeed(ReplaySpeed speed) override;
  bool seekTo(int64_t timestamp_ns) override;
  bool isFinished() const override;
  int64_t currentPosition() const override;
};
```

### ReplaySpeed

```cpp
struct ReplaySpeed
{
  static ReplaySpeed max();           // No throttling
  static ReplaySpeed realtime();      // 1x speed
  static ReplaySpeed multiplied(double factor);  // Nx speed
};
```

---

## Index Structures

### SegmentIndexHeader (32 bytes)

```cpp
struct alignas(8) SegmentIndexHeader
{
  uint32_t magic{0x58444E49};    // "INDX"
  uint16_t version{1};
  uint16_t interval{0};          // Events between entries
  uint32_t entry_count{0};
  uint32_t crc32{0};
  int64_t first_ts_ns{0};
  int64_t last_ts_ns{0};

  bool isValid() const;
};
```

### IndexEntry (16 bytes)

```cpp
struct alignas(8) IndexEntry
{
  int64_t timestamp_ns{0};
  uint64_t file_offset{0};
};
```

Enables O(log n) seeking to any timestamp.

---

## Compression

### Block Format (LZ4)

```cpp
struct alignas(8) CompressedBlockHeader  // 16 bytes
{
  uint32_t magic{0x4B4C4246};    // "FBLK"
  uint32_t compressed_size{0};
  uint32_t original_size{0};
  uint16_t event_count{0};
  uint16_t flags{0};

  bool isValid() const;
};
```

Enable LZ4 compression:

```bash
cmake .. -DFLOX_ENABLE_LZ4=ON
```

```cpp
WriterConfig config;
config.compression = CompressionType::LZ4;
```

---

## Time Utilities

**Header:** `flox/replay/readers/binary_log_reader.h`

```cpp
namespace flox::replay::time_utils {
  int64_t toNanos(std::chrono::system_clock::time_point tp);
  std::chrono::system_clock::time_point fromNanos(int64_t ns);
  int64_t nowNanos();
  int64_t secondsToNanos(int64_t seconds);
  int64_t millisToNanos(int64_t millis);
  int64_t microsToNanos(int64_t micros);
  double nanosToSeconds(int64_t ns);
}
```

---

## See Also

- [Recording Data Tutorial](../tutorials/recording-data.md)
- [Backtesting Tutorial](../tutorials/backtesting.md)
