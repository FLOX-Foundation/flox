# BinaryLogRecorderHook

`flox::replay::BinaryLogRecorderHook` is the built-in market-data
recorder that plugs into the recorder slot on a `Runner` or
`LiveEngine` and writes both trades and book updates to a `.floxlog`
directory.

Lives at `include/flox/replay/binary_log_recorder_hook.h`; replaced
the older `MarketDataRecorder` class in v0.6.0.

```cpp
struct BinaryLogRecorderHookConfig {
  std::filesystem::path output_dir;
  uint64_t max_segment_bytes{256ull << 20};   // 256 MB
  uint8_t exchange_id{0};
  CompressionType compression{CompressionType::None};
  std::optional<RecordingMetadata> metadata;
};

class BinaryLogRecorderHook {
public:
  explicit BinaryLogRecorderHook(BinaryLogRecorderHookConfig config);

  void start();
  void stop();
  bool isRecording() const noexcept;

  void onTrade(uint32_t symbol_id, int64_t price_raw, int64_t qty_raw,
               bool is_buy, int64_t exchange_ts_ns,
               int64_t recv_ts_ns) noexcept;
  void onBookUpdate(uint32_t symbol_id, bool is_snapshot,
                    const BookLevel* bids, uint32_t n_bids,
                    const BookLevel* asks, uint32_t n_asks,
                    int64_t exchange_ts_ns,
                    int64_t recv_ts_ns) noexcept;

  void addSymbol(const SymbolInfo& info);
  void flush();
  RecorderStats stats() const noexcept;
  std::filesystem::path currentSegmentPath() const;
};
```

## Attaching to an engine

The hook is wired through the C-API:

```cpp
auto handle = flox_binary_log_recorder_hook_create(
    "/data/btcusdt", 256, /*exchange_id=*/0, /*compression=*/0);

flox_binary_log_recorder_hook_add_symbol(handle, 1, "BTCUSDT", "BTC",
                                         "USDT", 2, 3);

flox_runner_set_market_data_recorder(
    runner, flox_binary_log_recorder_hook_as_recorder(handle));

// ... runner runs ...

flox_binary_log_recorder_hook_destroy(handle);
```

`flox_runner_set_market_data_recorder` accepts the same handle type
that `flox_market_data_recorder_create` (user-callback flavour) yields,
so the two flavours of recorder are interchangeable at the attach
point. Internally `start()` / `stop()` fire on the engine's lifecycle.

## Bindings

| Language | Class |
|----------|-------|
| Python | `flox_py.BinaryLogRecorderHook` |
| Node.js | `flox.BinaryLogRecorderHook` |
| QuickJS | `BinaryLogRecorderHook` |
| Codon | `BinaryLogRecorderHook` |

All four accept the same constructor signature `(output_dir,
max_segment_mb, exchange_id, compression)` and expose `add_symbol`,
`flush`, `stats`, and `close`.

## Configuration

| Field | Default | Description |
|-------|---------|-------------|
| `output_dir` | — | Directory for recorded segments. |
| `max_segment_bytes` | 256 MB | Maximum segment size before rotation. |
| `exchange_id` | 0 | Exchange identifier in segment headers. |
| `compression` | `None` | `None` or `LZ4`. |
| `metadata` | `nullopt` | Optional `RecordingMetadata` written alongside segments. |

## See also

- [BinaryLogWriter](binary_log_writer.md) — low-level writer.
- [BinaryLogReader](binary_log_reader.md) — reading recorded data.
- [Recording Data Tutorial](../../../tutorials/recording-data.md).
- [`flox tape` CLI](../../../how-to/tape-record.md).
