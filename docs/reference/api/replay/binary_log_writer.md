# BinaryLogWriter

`BinaryLogWriter` writes market data to binary log segments. It handles segment rotation, compression, indexing, and CRC checksums.

```cpp
struct WriterConfig {
  std::filesystem::path output_dir;
  std::string output_filename;              // Optional: override generated name
  uint64_t max_segment_bytes{256ull << 20}; // 256 MB default
  uint64_t buffer_size{64ull << 10};        // 64 KB buffer
  uint8_t exchange_id{0};
  bool sync_on_rotate{true};
  bool create_index{true};
  uint16_t index_interval{1000};            // Events per index entry
  CompressionType compression{CompressionType::None};
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
| `output_filename` | - | Optional fixed filename (disables rotation). |
| `max_segment_bytes` | 256 MB | Maximum segment size before rotation. |
| `buffer_size` | 64 KB | Internal write buffer size. |
| `exchange_id` | 0 | Exchange identifier in segment header. |
| `sync_on_rotate` | true | Sync to disk on segment rotation. |
| `create_index` | true | Build seek index in segments. |
| `index_interval` | 1000 | Events between index entries. |
| `compression` | None | Compression type (`None` or `LZ4`). |

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
