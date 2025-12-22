# BinaryLogWriter

`BinaryLogWriter` writes market data to binary log segments. It handles segment rotation, compression, indexing, and CRC checksums.

```cpp
// Callback for custom segment naming on rotation
using RotationCallback = std::filesystem::path (*)(
    void* user_data,
    const std::filesystem::path& output_dir,
    uint32_t segment_number);

struct WriterConfig {
  std::filesystem::path output_dir;
  std::string output_filename;              // Optional: first segment name
  uint64_t max_segment_bytes{256ull << 20}; // 256 MB default
  uint64_t buffer_size{64ull << 10};        // 64 KB buffer
  uint8_t exchange_id{0};
  bool sync_on_rotate{true};
  bool create_index{true};
  uint16_t index_interval{1000};            // Events per index entry
  CompressionType compression{CompressionType::None};
  RotationCallback rotation_callback{nullptr};  // Custom naming on rotation
  void* rotation_user_data{nullptr};            // User data for callback
};

class BinaryLogWriter {
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
```

## Purpose

* Write market data (trades and book updates) to compact binary segments.
* Support automatic segment rotation, compression, and indexing.
* Provide high-throughput, low-latency recording for live market data.

## Configuration

| Field | Default | Description |
|-------|---------|-------------|
| `output_dir` | - | Directory for segment files. |
| `output_filename` | - | Optional filename for first segment. |
| `max_segment_bytes` | 256 MB | Maximum segment size before rotation. |
| `buffer_size` | 64 KB | Internal write buffer size. |
| `exchange_id` | 0 | Exchange identifier in segment header. |
| `sync_on_rotate` | true | Sync to disk on segment rotation. |
| `create_index` | true | Build seek index in segments. |
| `index_interval` | 1000 | Events between index entries. |
| `compression` | None | Compression type (`None` or `LZ4`). |
| `rotation_callback` | nullptr | Custom callback for segment naming on rotation. |
| `rotation_user_data` | nullptr | User data passed to rotation callback. |

## Methods

| Method | Description |
|--------|-------------|
| `writeTrade(trade)` | Write a trade record. Returns `true` on success. |
| `writeBook(header, bids, asks)` | Write a book update. Returns `true` on success. |
| `flush()` | Flush internal buffers to disk. |
| `close()` | Close current segment, write index and header. |
| `stats()` | Returns write statistics. |
| `currentSegmentPath()` | Returns path to current segment file. |

## Statistics

```cpp
struct WriterStats {
  uint64_t bytes_written{0};
  uint64_t events_written{0};
  uint64_t segments_created{0};
  uint64_t trades_written{0};
  uint64_t book_updates_written{0};
  uint64_t blocks_written{0};        // For compressed mode
  uint64_t uncompressed_bytes{0};
  uint64_t compressed_bytes{0};
};
```

## Usage

```cpp
replay::WriterConfig config{
  .output_dir = "/data/market",
  .max_segment_bytes = 512ull << 20,  // 512 MB segments
  .create_index = true,
  .compression = replay::CompressionType::LZ4
};

replay::BinaryLogWriter writer(config);

// Write trades
replay::TradeRecord trade{...};
writer.writeTrade(trade);

// Write book updates
replay::BookRecordHeader header{...};
std::vector<replay::BookLevel> bids{...}, asks{...};
writer.writeBook(header, bids, asks);

// Periodic flush
writer.flush();

// Close cleanly
writer.close();
```

## Custom Rotation Callback

By default, rotated segments use timestamp-based names. To customize naming:

```cpp
// Context for the callback
struct MyContext {
  std::string prefix;
  int sequence{1};
};

// Callback function (must be a plain function, not a lambda with captures)
std::filesystem::path my_rotation_cb(
    void* user_data,
    const std::filesystem::path& output_dir,
    uint32_t segment_number)
{
  auto* ctx = static_cast<MyContext*>(user_data);
  std::ostringstream fname;
  fname << ctx->prefix << "_" << std::setfill('0') << std::setw(3)
        << (ctx->sequence + segment_number - 1) << ".floxlog";
  return output_dir / fname.str();
}

// Usage
MyContext ctx{"2025-01-15", 1};

replay::WriterConfig config{
  .output_dir = "/data/market",
  .output_filename = "2025-01-15_001.floxlog",  // First segment
  .max_segment_bytes = 256ull << 20,
  .rotation_callback = my_rotation_cb,
  .rotation_user_data = &ctx,  // Must outlive writer!
};

replay::BinaryLogWriter writer(config);
// When segment rotates, callback generates "2025-01-15_002.floxlog", etc.
```

**Important:** The `rotation_user_data` pointer must remain valid for the lifetime of the writer.

## Notes

* Thread-safe via internal mutex.
* Segments are automatically rotated when `max_segment_bytes` is reached.
* Index enables fast timestamp-based seeking during replay.
* Compression reduces storage but adds CPU overhead.
* Call `close()` before destruction to ensure index is written.

## See Also

* [Binary Format](binary_format.md) — File format specification
* [BinaryLogReader](binary_log_reader.md) — Reading segments
* [MarketDataRecorder](market_data_recorder.md) — High-level recording interface
