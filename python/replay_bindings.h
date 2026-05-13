// python/replay_bindings.h

#pragma once

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

#include <pybind11/stl.h>
#include "flox/error/flox_error.h"

#include "flox/common.h"
#include "flox/replay/binary_format_v1.h"
#include "flox/replay/binary_log_recorder_hook.h"
#include "flox/replay/merged_tape_reader.h"
#include "flox/replay/readers/binary_log_reader.h"
#include "flox/replay/recording_metadata.h"
#include "flox/replay/writers/binary_log_writer.h"
#include "tape_aggregator_bindings.h"

#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace py = pybind11;

namespace flox_py
{

// Thin pybind wrapper over flox::replay::BinaryLogRecorderHook. Lives in
// flox_py namespace so strategy_bindings.h can take a shared_ptr to it in
// `runner.set_market_data_recorder(...)` overloads.
class PyBinaryLogRecorderHook
{
  std::unique_ptr<flox::replay::BinaryLogRecorderHook> _hook;

 public:
  PyBinaryLogRecorderHook(const std::string& outputDir, uint64_t maxSegmentMb,
                          uint8_t exchangeId, const std::string& compression,
                          const std::string& exchangeName,
                          const std::string& instrumentType)
  {
    flox::replay::BinaryLogRecorderHookConfig cfg{};
    cfg.output_dir = outputDir;
    cfg.max_segment_bytes = maxSegmentMb * (1024ull * 1024ull);
    cfg.exchange_id = exchangeId;
    if (compression == "none" || compression.empty())
    {
      cfg.compression = flox::replay::CompressionType::None;
    }
    else if (compression == "lz4")
    {
      cfg.compression = flox::replay::CompressionType::LZ4;
    }
    else
    {
      throw flox::FloxError(
          "E_INPUT_001",
          "Unknown compression: '" + compression + "'. Expected 'none' or 'lz4'.");
    }
    if (!exchangeName.empty() || !instrumentType.empty())
    {
      flox::replay::RecordingMetadata meta{};
      meta.exchange = exchangeName;
      meta.instrument_type = instrumentType;
      cfg.metadata = std::move(meta);
    }
    _hook = std::make_unique<flox::replay::BinaryLogRecorderHook>(std::move(cfg));
  }

  flox::replay::BinaryLogRecorderHook* raw() noexcept { return _hook.get(); }

  void addSymbol(uint32_t symbolId, const std::string& name, const std::string& base,
                 const std::string& quote, int8_t pricePrecision, int8_t qtyPrecision)
  {
    flox::replay::SymbolInfo info;
    info.symbol_id = symbolId;
    info.name = name;
    info.base_asset = base;
    info.quote_asset = quote;
    info.price_precision = pricePrecision;
    info.qty_precision = qtyPrecision;
    _hook->addSymbol(info);
  }

  void flush() { _hook->flush(); }

  // Idempotent — safe to call from user code OR via the engine's on_stop
  // lifecycle callback. Just stops the underlying writer.
  void close() { _hook->stop(); }

  pybind11::dict stats()
  {
    auto s = _hook->stats();
    pybind11::dict d;
    d["trades_written"] = s.trades_written;
    d["book_updates_written"] = s.book_updates_written;
    d["bytes_written"] = s.bytes_written;
    d["segments_created"] = s.segments_created;
    d["errors"] = s.errors;
    return d;
  }

  std::string currentSegmentPath() const { return _hook->currentSegmentPath().string(); }
};

}  // namespace flox_py

namespace
{

using namespace flox::replay;

// ─── Numpy-compatible trade struct ─────────────────────────────────────────────

#pragma pack(push, 1)
struct PyTrade
{
  int64_t exchange_ts_ns;
  int64_t recv_ts_ns;
  int64_t price_raw;
  int64_t qty_raw;
  uint64_t trade_id;
  uint32_t symbol_id;
  uint8_t side;
  uint8_t _pad[3];
};
#pragma pack(pop)
static_assert(sizeof(PyTrade) == 48);

// ─── Numpy-compatible book structs ─────────────────────────────────────────────

// Best bid/ask from a single book update event.
#pragma pack(push, 1)
struct PyBBO
{
  int64_t exchange_ts_ns;
  int64_t recv_ts_ns;
  int64_t seq;
  uint32_t symbol_id;
  uint8_t event_type;  // 2=snapshot, 3=delta
  uint8_t _pad[3];
  int64_t bid_price_raw;
  int64_t bid_qty_raw;
  int64_t ask_price_raw;
  int64_t ask_qty_raw;
};
#pragma pack(pop)
static_assert(sizeof(PyBBO) == 64);

// Per-event header for read_book_updates(); levels are in a separate flat array.
#pragma pack(push, 1)
struct PyBookUpdateHeader
{
  int64_t exchange_ts_ns;
  int64_t recv_ts_ns;
  int64_t seq;
  uint32_t symbol_id;
  uint16_t bid_count;
  uint16_t ask_count;
  uint32_t level_offset;  // index into the flat levels array for this event
  uint8_t event_type;     // 2=snapshot, 3=delta
  uint8_t _pad[3];
};
#pragma pack(pop)
static_assert(sizeof(PyBookUpdateHeader) == 40);

// One price level in the flat levels array.
#pragma pack(push, 1)
struct PyLevel
{
  int64_t price_raw;
  int64_t qty_raw;
  uint8_t side;  // 0=bid, 1=ask
  uint8_t _pad[7];
};
#pragma pack(pop)
static_assert(sizeof(PyLevel) == 24);

// ─── Helpers ───────────────────────────────────────────────────────────────────

inline PyTrade tradeRecordToPyTrade(const flox::replay::TradeRecord& tr)
{
  return {tr.exchange_ts_ns, tr.recv_ts_ns, tr.price_raw, tr.qty_raw, tr.trade_id, tr.symbol_id, tr.side, {}};
}

inline ReaderConfig makeReaderConfig(const std::string& dataDir, py::object fromNs,
                                     py::object toNs, py::object symbols,
                                     py::object reorderWindowNs)
{
  ReaderConfig cfg;
  cfg.data_dir = dataDir;

  if (!fromNs.is_none())
  {
    cfg.from_ns = fromNs.cast<int64_t>();
  }
  if (!toNs.is_none())
  {
    cfg.to_ns = toNs.cast<int64_t>();
  }
  if (!symbols.is_none())
  {
    auto symList = symbols.cast<std::vector<uint32_t>>();
    cfg.symbols.insert(symList.begin(), symList.end());
  }
  if (!reorderWindowNs.is_none())
  {
    cfg.reorder_window_ns = reorderWindowNs.cast<int64_t>();
  }

  return cfg;
}

inline WriterConfig makeWriterConfig(const std::string& outputDir, uint64_t maxSegmentMb,
                                     uint8_t exchangeId, const std::string& compression)
{
  WriterConfig cfg;
  cfg.output_dir = outputDir;
  cfg.max_segment_bytes = maxSegmentMb * (1024ull * 1024ull);
  cfg.exchange_id = exchangeId;

  if (compression == "lz4")
  {
    cfg.compression = CompressionType::LZ4;
  }
  else if (compression == "none" || compression.empty())
  {
    cfg.compression = CompressionType::None;
  }
  else
  {
    throw flox::FloxError(
        "E_INPUT_001",
        "Unknown compression type: '" + compression +
            "'. Expected 'none' or 'lz4'.");
  }

  return cfg;
}

// ─── PyDataReader ──────────────────────────────────────────────────────────────

class PyDataReader
{
  BinaryLogReader _reader;

 public:
  PyDataReader(const std::string& dataDir, py::object fromNs, py::object toNs,
               py::object symbols, py::object reorderWindowNs)
      : _reader(makeReaderConfig(dataDir, fromNs, toNs, symbols, reorderWindowNs))
  {
  }

  py::dict summary()
  {
    DatasetSummary s;
    {
      py::gil_scoped_release release;
      s = _reader.summary();
    }

    py::dict d;
    d["data_dir"] = s.data_dir.string();
    d["first_event_ns"] = s.first_event_ns;
    d["last_event_ns"] = s.last_event_ns;
    d["total_events"] = s.total_events;
    d["segment_count"] = s.segment_count;
    d["total_bytes"] = s.total_bytes;
    d["duration_seconds"] = s.durationSeconds();
    d["fully_indexed"] = s.fullyIndexed();

    py::set syms;
    for (auto id : s.symbols)
    {
      syms.add(py::int_(id));
    }
    d["symbols"] = syms;

    return d;
  }

  uint64_t count()
  {
    uint64_t n;
    {
      py::gil_scoped_release release;
      n = _reader.count();
    }
    return n;
  }

  py::set symbols()
  {
    std::set<uint32_t> syms;
    {
      py::gil_scoped_release release;
      syms = _reader.availableSymbols();
    }
    py::set result;
    for (auto id : syms)
    {
      result.add(py::int_(id));
    }
    return result;
  }

  py::object timeRange()
  {
    auto range = _reader.timeRange();
    if (!range.has_value())
    {
      return py::none();
    }
    return py::make_tuple(range->first, range->second);
  }

  py::array_t<PyTrade> readTrades()
  {
    std::vector<PyTrade> trades;
    {
      py::gil_scoped_release release;
      _reader.forEach(
          [&](const ReplayEvent& ev) -> bool
          {
            if (ev.type == EventType::Trade)
            {
              trades.push_back(tradeRecordToPyTrade(ev.trade));
            }
            return true;
          });
    }

    py::array_t<PyTrade> result(trades.size());
    if (!trades.empty())
    {
      std::memcpy(result.mutable_data(), trades.data(), trades.size() * sizeof(PyTrade));
    }
    return result;
  }

  py::array_t<PyTrade> readTradesFrom(int64_t startTsNs)
  {
    std::vector<PyTrade> trades;
    {
      py::gil_scoped_release release;
      _reader.forEachFrom(startTsNs,
                          [&](const ReplayEvent& ev) -> bool
                          {
                            if (ev.type == EventType::Trade)
                            {
                              trades.push_back(tradeRecordToPyTrade(ev.trade));
                            }
                            return true;
                          });
    }

    py::array_t<PyTrade> result(trades.size());
    if (!trades.empty())
    {
      std::memcpy(result.mutable_data(), trades.data(), trades.size() * sizeof(PyTrade));
    }
    return result;
  }

  // Read best bid/ask from every book update as a structured numpy array.
  py::array_t<PyBBO> readBBO() { return readBBOInternal(/*from=*/std::nullopt); }

  py::array_t<PyBBO> readBBOFrom(int64_t startTsNs)
  {
    return readBBOInternal(startTsNs);
  }

 private:
  py::array_t<PyBBO> readBBOInternal(std::optional<int64_t> fromTsNs)
  {
    std::vector<PyBBO> bbos;
    auto cb = [&](const ReplayEvent& ev) -> bool
    {
      if (ev.type != EventType::BookSnapshot && ev.type != EventType::BookDelta)
      {
        return true;
      }

      PyBBO b{};
      b.exchange_ts_ns = ev.book_header.exchange_ts_ns;
      b.recv_ts_ns = ev.book_header.recv_ts_ns;
      b.seq = ev.book_header.seq;
      b.symbol_id = ev.book_header.symbol_id;
      b.event_type = ev.book_header.type;
      if (!ev.bids.empty())
      {
        b.bid_price_raw = ev.bids[0].price_raw;
        b.bid_qty_raw = ev.bids[0].qty_raw;
      }
      if (!ev.asks.empty())
      {
        b.ask_price_raw = ev.asks[0].price_raw;
        b.ask_qty_raw = ev.asks[0].qty_raw;
      }
      bbos.push_back(b);
      return true;
    };

    {
      py::gil_scoped_release release;
      if (fromTsNs)
      {
        _reader.forEachFrom(*fromTsNs, cb);
      }
      else
      {
        _reader.forEach(cb);
      }
    }

    py::array_t<PyBBO> result(bbos.size());
    if (!bbos.empty())
    {
      std::memcpy(result.mutable_data(), bbos.data(), bbos.size() * sizeof(PyBBO));
    }
    return result;
  }

 public:
  // Read all book updates.
  // Returns (headers, levels):
  //   headers  — structured array of PyBookUpdateHeader
  //   levels   — flat structured array of PyLevel (bids then asks per event)
  // To slice levels for event i:
  //   offset = headers[i]['level_offset']
  //   bids   = levels[offset : offset + headers[i]['bid_count']]
  //   asks   = levels[offset + headers[i]['bid_count'] : offset + headers[i]['bid_count'] + headers[i]['ask_count']]
  py::tuple readBookUpdates() { return readBookUpdatesInternal(/*from=*/std::nullopt); }

  py::tuple readBookUpdatesFrom(int64_t startTsNs)
  {
    return readBookUpdatesInternal(startTsNs);
  }

 private:
  py::tuple readBookUpdatesInternal(std::optional<int64_t> fromTsNs)
  {
    std::vector<PyBookUpdateHeader> headers;
    std::vector<PyLevel> levels;
    auto cb = [&](const ReplayEvent& ev) -> bool
    {
      if (ev.type != EventType::BookSnapshot && ev.type != EventType::BookDelta)
      {
        return true;
      }

      PyBookUpdateHeader h{};
      h.exchange_ts_ns = ev.book_header.exchange_ts_ns;
      h.recv_ts_ns = ev.book_header.recv_ts_ns;
      h.seq = ev.book_header.seq;
      h.symbol_id = ev.book_header.symbol_id;
      h.bid_count = static_cast<uint16_t>(ev.bids.size());
      h.ask_count = static_cast<uint16_t>(ev.asks.size());
      h.level_offset = static_cast<uint32_t>(levels.size());
      h.event_type = ev.book_header.type;
      headers.push_back(h);

      for (const auto& bl : ev.bids)
      {
        levels.push_back({bl.price_raw, bl.qty_raw, 0, {}});
      }
      for (const auto& bl : ev.asks)
      {
        levels.push_back({bl.price_raw, bl.qty_raw, 1, {}});
      }

      return true;
    };

    {
      py::gil_scoped_release release;
      if (fromTsNs)
      {
        _reader.forEachFrom(*fromTsNs, cb);
      }
      else
      {
        _reader.forEach(cb);
      }
    }

    py::array_t<PyBookUpdateHeader> h_arr(headers.size());
    if (!headers.empty())
    {
      std::memcpy(h_arr.mutable_data(), headers.data(), headers.size() * sizeof(PyBookUpdateHeader));
    }

    py::array_t<PyLevel> l_arr(levels.size());
    if (!levels.empty())
    {
      std::memcpy(l_arr.mutable_data(), levels.data(), levels.size() * sizeof(PyLevel));
    }

    return py::make_tuple(h_arr, l_arr);
  }

 public:
  py::dict stats()
  {
    auto s = _reader.stats();
    py::dict d;
    d["files_read"] = s.files_read;
    d["events_read"] = s.events_read;
    d["trades_read"] = s.trades_read;
    d["book_updates_read"] = s.book_updates_read;
    d["bytes_read"] = s.bytes_read;
    d["crc_errors"] = s.crc_errors;
    return d;
  }

  // Streaming aggregator dispatch. Walks the tape once, forwarding each
  // event to every IPyAggregator wrapper's underlying IAggregator, then
  // calls finalize() on each. Empty list is a no-op (no decompression).
  // GIL released for the whole walk — concrete aggregators must be
  // self-contained (no Python crossings) for this to be safe.
  //
  // n_threads=1 (default) preserves single-threaded semantics.
  // n_threads>1 partitions the segment list across workers; each
  // worker clones the panel via cloneEmpty(), then results merge
  // into the user-visible instances before finalize().
  bool run(py::list py_aggregators, std::size_t n_threads)
  {
    auto raw = flox_py::collectAggregators(py_aggregators);
    bool ok;
    {
      py::gil_scoped_release release;
      ok = _reader.run(raw, n_threads);
    }
    return ok;
  }

  py::list segmentFiles()
  {
    auto files = _reader.segmentFiles();
    py::list result;
    for (const auto& f : files)
    {
      result.append(f.string());
    }
    return result;
  }

  py::list segments()
  {
    const auto& segs = _reader.segments();
    py::list result;
    for (const auto& seg : segs)
    {
      py::dict d;
      d["path"] = seg.path.string();
      d["first_event_ns"] = seg.first_event_ns;
      d["last_event_ns"] = seg.last_event_ns;
      d["event_count"] = seg.event_count;
      d["has_index"] = seg.has_index;
      result.append(d);
    }
    return result;
  }
};

// ─── PyDataWriter ──────────────────────────────────────────────────────────────

class PyDataWriter
{
  BinaryLogWriter _writer;

 public:
  PyDataWriter(const std::string& outputDir, uint64_t maxSegmentMb, uint8_t exchangeId,
               const std::string& compression)
      : _writer(makeWriterConfig(outputDir, maxSegmentMb, exchangeId, compression))
  {
  }

  bool writeTrade(int64_t exchangeTsNs, int64_t recvTsNs, double price, double qty,
                  uint64_t tradeId, uint32_t symbolId, uint8_t side)
  {
    flox::replay::TradeRecord tr{};
    tr.exchange_ts_ns = exchangeTsNs;
    tr.recv_ts_ns = recvTsNs;
    tr.price_raw = flox::Price::fromDouble(price).raw();
    tr.qty_raw = flox::Quantity::fromDouble(qty).raw();
    tr.trade_id = tradeId;
    tr.symbol_id = symbolId;
    tr.side = side;
    return _writer.writeTrade(tr);
  }

  uint64_t writeTrades(py::array_t<int64_t> exchangeTsNs, py::array_t<int64_t> recvTsNs,
                       py::array_t<double> prices, py::array_t<double> quantities,
                       py::array_t<uint64_t> tradeIds, py::array_t<uint32_t> symbolIds,
                       py::array_t<uint8_t> sides)
  {
    size_t n = exchangeTsNs.size();
    if (recvTsNs.size() != static_cast<py::ssize_t>(n) ||
        prices.size() != static_cast<py::ssize_t>(n) ||
        quantities.size() != static_cast<py::ssize_t>(n) ||
        tradeIds.size() != static_cast<py::ssize_t>(n) ||
        symbolIds.size() != static_cast<py::ssize_t>(n) ||
        sides.size() != static_cast<py::ssize_t>(n))
    {
      throw flox::FloxError(
          "E_LEN_001",
          "All input arrays must have the same length.");
    }

    const auto* ets = exchangeTsNs.data();
    const auto* rts = recvTsNs.data();
    const auto* px = prices.data();
    const auto* qt = quantities.data();
    const auto* tid = tradeIds.data();
    const auto* sid = symbolIds.data();
    const auto* sd = sides.data();

    uint64_t written = 0;
    {
      py::gil_scoped_release release;
      for (size_t i = 0; i < n; ++i)
      {
        flox::replay::TradeRecord tr{};
        tr.exchange_ts_ns = ets[i];
        tr.recv_ts_ns = rts[i];
        tr.price_raw = flox::Price::fromDouble(px[i]).raw();
        tr.qty_raw = flox::Quantity::fromDouble(qt[i]).raw();
        tr.trade_id = tid[i];
        tr.symbol_id = sid[i];
        tr.side = sd[i];
        if (_writer.writeTrade(tr))
        {
          ++written;
        }
      }
    }
    return written;
  }

  // Single book write. `bids`/`asks` are structured arrays with PyLevel
  // dtype (price_raw, qty_raw, side). `side` is ignored on write — bid/
  // ask is determined by which array a level is in. Returns True iff the
  // write succeeded.
  bool writeBook(int64_t exchangeTsNs, int64_t recvTsNs, int64_t seq,
                 uint32_t symbolId, bool isSnapshot,
                 py::array_t<PyLevel, py::array::c_style | py::array::forcecast> bids,
                 py::array_t<PyLevel, py::array::c_style | py::array::forcecast> asks)
  {
    auto n_bids = static_cast<uint32_t>(bids.size());
    auto n_asks = static_cast<uint32_t>(asks.size());

    BookRecordHeader header{};
    header.exchange_ts_ns = exchangeTsNs;
    header.recv_ts_ns = recvTsNs;
    header.seq = seq;
    header.symbol_id = symbolId;
    header.bid_count = static_cast<uint16_t>(n_bids);
    header.ask_count = static_cast<uint16_t>(n_asks);
    header.type = isSnapshot ? 0 : 1;

    // PyLevel includes a `side` byte; copy into contiguous BookLevel.
    std::vector<flox::replay::BookLevel> bid_levels;
    std::vector<flox::replay::BookLevel> ask_levels;
    bid_levels.reserve(n_bids);
    ask_levels.reserve(n_asks);
    const auto* b = bids.data();
    for (uint32_t i = 0; i < n_bids; ++i)
    {
      bid_levels.push_back({b[i].price_raw, b[i].qty_raw});
    }
    const auto* a = asks.data();
    for (uint32_t i = 0; i < n_asks; ++i)
    {
      ask_levels.push_back({a[i].price_raw, a[i].qty_raw});
    }
    return _writer.writeBook(header, bid_levels, ask_levels);
  }

  // Batched book writer. `headers` is a structured array with
  // PyBookUpdateHeader dtype; `levels` is a flat PyLevel array sliced
  // per event by header.level_offset / bid_count / ask_count. Round-trip
  // with DataReader.read_book_updates(). Returns count successfully written.
  uint64_t writeBooks(py::array_t<PyBookUpdateHeader, py::array::c_style | py::array::forcecast> headers,
                      py::array_t<PyLevel, py::array::c_style | py::array::forcecast> levels)
  {
    const auto n_events = static_cast<uint64_t>(headers.size());
    const auto* h = headers.data();
    const auto* lv = levels.data();
    std::vector<flox::replay::BookLevel> scratch;
    uint64_t written = 0;
    {
      py::gil_scoped_release release;
      for (uint64_t i = 0; i < n_events; ++i)
      {
        const auto& fh = h[i];
        const uint64_t total = static_cast<uint64_t>(fh.bid_count) + fh.ask_count;
        scratch.clear();
        scratch.reserve(total);
        for (uint64_t k = 0; k < total; ++k)
        {
          const auto& el = lv[fh.level_offset + k];
          scratch.push_back({el.price_raw, el.qty_raw});
        }

        BookRecordHeader rh{};
        rh.exchange_ts_ns = fh.exchange_ts_ns;
        rh.recv_ts_ns = fh.recv_ts_ns;
        rh.seq = fh.seq;
        rh.symbol_id = fh.symbol_id;
        rh.bid_count = fh.bid_count;
        rh.ask_count = fh.ask_count;
        rh.type = (fh.event_type == 2) ? 0 : 1;

        std::span<const flox::replay::BookLevel> bid_span(scratch.data(), fh.bid_count);
        std::span<const flox::replay::BookLevel> ask_span(scratch.data() + fh.bid_count, fh.ask_count);
        if (_writer.writeBook(rh, bid_span, ask_span))
        {
          ++written;
        }
      }
    }
    return written;
  }

  void flush() { _writer.flush(); }

  void close() { _writer.close(); }

  py::dict stats()
  {
    auto s = _writer.stats();
    py::dict d;
    d["bytes_written"] = s.bytes_written;
    d["events_written"] = s.events_written;
    d["segments_created"] = s.segments_created;
    d["trades_written"] = s.trades_written;
    d["book_updates_written"] = s.book_updates_written;
    d["blocks_written"] = s.blocks_written;
    d["uncompressed_bytes"] = s.uncompressed_bytes;
    d["compressed_bytes"] = s.compressed_bytes;
    return d;
  }

  std::string currentSegmentPath() { return _writer.currentSegmentPath().string(); }
};

// ─── PyMergedTapeReader ────────────────────────────────────────────────────────

class PyMergedTapeReader
{
  flox::replay::MergedTapeReader _reader;

  static flox::replay::MergedTapeReaderConfig
  buildConfig(const std::vector<std::string>& paths, py::object from_ns,
              py::object to_ns, py::object symbols)
  {
    flox::replay::MergedTapeReaderConfig cfg{};
    cfg.tape_dirs.reserve(paths.size());
    for (const auto& p : paths)
    {
      cfg.tape_dirs.emplace_back(p);
    }
    if (!from_ns.is_none())
    {
      cfg.from_ns = from_ns.cast<int64_t>();
    }
    if (!to_ns.is_none())
    {
      cfg.to_ns = to_ns.cast<int64_t>();
    }
    if (!symbols.is_none())
    {
      for (auto h : symbols.cast<py::list>())
      {
        cfg.symbol_filter.push_back(h.cast<uint32_t>());
      }
    }
    return cfg;
  }

 public:
  PyMergedTapeReader(const std::vector<std::string>& paths, py::object from_ns,
                     py::object to_ns, py::object symbols)
      : _reader(buildConfig(paths, std::move(from_ns), std::move(to_ns),
                            std::move(symbols)))
  {
  }

  py::list symbolTable() const
  {
    py::list out;
    for (const auto& s : _reader.symbols())
    {
      py::dict d;
      d["global_id"] = s.global_id;
      d["exchange"] = s.exchange;
      d["name"] = s.name;
      d["price_precision"] = s.price_precision;
      d["qty_precision"] = s.qty_precision;
      out.append(d);
    }
    return out;
  }

  py::tuple timeRange() const
  {
    auto [a, b] = _reader.timeRange();
    return py::make_tuple(a, b);
  }

  py::dict summary() const
  {
    auto s = _reader.summary();
    py::dict d;
    d["first_event_ns"] = s.first_event_ns;
    d["last_event_ns"] = s.last_event_ns;
    d["total_events"] = s.total_events;
    d["tape_count"] = s.tape_count;
    d["symbol_count"] = s.symbol_count;
    return d;
  }

  py::array readTrades()
  {
    auto rows = _reader.readTrades();
    py::array_t<PyTrade> arr(static_cast<py::ssize_t>(rows.size()));
    auto* out = arr.mutable_data();
    for (size_t i = 0; i < rows.size(); ++i)
    {
      const auto& r = rows[i];
      out[i].exchange_ts_ns = r.exchange_ts_ns;
      out[i].recv_ts_ns = r.recv_ts_ns;
      out[i].price_raw = r.price_raw;
      out[i].qty_raw = r.qty_raw;
      out[i].trade_id = r.trade_id;
      out[i].symbol_id = r.global_symbol_id;
      out[i].side = r.side;
      out[i]._pad[0] = out[i]._pad[1] = out[i]._pad[2] = 0;
    }
    return std::move(arr);
  }

  py::tuple readBooks()
  {
    auto [rows, levels] = _reader.readBooks();
    py::array_t<PyBookUpdateHeader> headers(static_cast<py::ssize_t>(rows.size()));
    auto* hout = headers.mutable_data();
    for (size_t i = 0; i < rows.size(); ++i)
    {
      const auto& r = rows[i];
      hout[i].exchange_ts_ns = r.exchange_ts_ns;
      hout[i].recv_ts_ns = r.recv_ts_ns;
      hout[i].seq = r.seq;
      hout[i].symbol_id = r.global_symbol_id;
      hout[i].bid_count = r.bid_count;
      hout[i].ask_count = r.ask_count;
      hout[i].level_offset = r.level_offset;
      hout[i].event_type = r.event_type;
    }

    py::array_t<PyLevel> lvls(static_cast<py::ssize_t>(levels.size()));
    auto* lout = lvls.mutable_data();
    size_t k = 0;
    for (size_t i = 0; i < rows.size(); ++i)
    {
      const auto& r = rows[i];
      for (uint16_t b = 0; b < r.bid_count; ++b, ++k)
      {
        lout[k].price_raw = levels[k].price_raw;
        lout[k].qty_raw = levels[k].qty_raw;
        lout[k].side = 0;
      }
      for (uint16_t a = 0; a < r.ask_count; ++a, ++k)
      {
        lout[k].price_raw = levels[k].price_raw;
        lout[k].qty_raw = levels[k].qty_raw;
        lout[k].side = 1;
      }
    }
    return py::make_tuple(std::move(headers), std::move(lvls));
  }

  py::list perTapeStats() const
  {
    py::list out;
    for (const auto& s : _reader.perTapeStats())
    {
      py::dict d;
      d["path"] = s.path.string();
      d["trades"] = s.trades;
      d["books"] = s.books;
      d["first_event_ns"] = s.first_event_ns;
      d["last_event_ns"] = s.last_event_ns;
      out.append(d);
    }
    return out;
  }

  // Streaming walk via the C++ N-way heap merge. O(N tapes) peak
  // memory regardless of total event count. on_trade / on_book are
  // optional Python callables; returning False from either aborts.
  // Lossless raw int64 prices/qty preserved (no float conversion).
  void streamEvents(py::object on_trade, py::object on_book)
  {
    _reader.streamEvents(
        [&](uint32_t tape_index, const flox::replay::ReplayEvent& ev) -> bool
        {
          if (ev.type == flox::replay::EventType::Trade)
          {
            if (on_trade.is_none())
            {
              return true;
            }
            py::object rv = on_trade(int64_t{ev.trade.exchange_ts_ns},
                                     int64_t{ev.trade.recv_ts_ns},
                                     int64_t{ev.trade.price_raw},
                                     int64_t{ev.trade.qty_raw},
                                     uint32_t{ev.trade.symbol_id},
                                     uint32_t{tape_index},
                                     uint8_t{ev.trade.side});
            if (!rv.is_none() && !rv.cast<bool>())
            {
              return false;
            }
            return true;
          }
          if (ev.type == flox::replay::EventType::BookSnapshot ||
              ev.type == flox::replay::EventType::BookDelta)
          {
            if (on_book.is_none())
            {
              return true;
            }
            const bool is_snap = ev.type == flox::replay::EventType::BookSnapshot;
            py::list bids;
            for (const auto& b : ev.bids)
            {
              bids.append(py::make_tuple(int64_t{b.price_raw}, int64_t{b.qty_raw}));
            }
            py::list asks;
            for (const auto& a : ev.asks)
            {
              asks.append(py::make_tuple(int64_t{a.price_raw}, int64_t{a.qty_raw}));
            }
            py::object rv = on_book(int64_t{ev.book_header.exchange_ts_ns},
                                    int64_t{ev.book_header.recv_ts_ns},
                                    uint32_t{ev.book_header.symbol_id},
                                    uint32_t{tape_index},
                                    is_snap,
                                    bids, asks);
            if (!rv.is_none() && !rv.cast<bool>())
            {
              return false;
            }
            return true;
          }
          return true;
        });
  }

  // Streaming aggregator dispatch over the merged stream. Same
  // contract as PyDataReader::run — empty list is a no-op, otherwise
  // every aggregator's onEvent fires once per merged event and
  // finalize() once at the end. GIL released around the whole walk.
  bool run(py::list py_aggregators)
  {
    auto raw = flox_py::collectAggregators(py_aggregators);
    bool ok;
    {
      py::gil_scoped_release release;
      ok = _reader.run(raw);
    }
    return ok;
  }
};

}  // namespace

// ─── Module binding ────────────────────────────────────────────────────────────

inline void bindReplay(py::module_& m)
{
  PYBIND11_NUMPY_DTYPE(PyTrade, exchange_ts_ns, recv_ts_ns, price_raw, qty_raw, trade_id,
                       symbol_id, side);
  PYBIND11_NUMPY_DTYPE(PyBBO, exchange_ts_ns, recv_ts_ns, seq, symbol_id, event_type,
                       bid_price_raw, bid_qty_raw, ask_price_raw, ask_qty_raw);
  PYBIND11_NUMPY_DTYPE(PyBookUpdateHeader, exchange_ts_ns, recv_ts_ns, seq, symbol_id,
                       bid_count, ask_count, level_offset, event_type);
  PYBIND11_NUMPY_DTYPE(PyLevel, price_raw, qty_raw, side);

  // Fixed-point scales — match flox::Price/Quantity/Volume in flox/common.h.
  // Use these instead of hardcoding 1e8 in client code.
  m.attr("PRICE_SCALE") = py::int_(flox::Price::Scale);
  m.attr("QUANTITY_SCALE") = py::int_(flox::Quantity::Scale);
  m.attr("VOLUME_SCALE") = py::int_(flox::Volume::Scale);

  // Vectorized raw → double converters. Operate on numpy int64 arrays of any
  // shape, including non-contiguous views (e.g. `bars["close_raw"]` taken from
  // a structured array). Output is always a fresh contiguous array of the same
  // shape as the input.
  auto rawToDouble = [](py::array raw, int64_t scale) -> py::array_t<double>
  {
    // Force int64 dtype + contiguous layout. If the caller passed a strided
    // view (e.g. a structured-array field), this materialises a contiguous
    // copy; if it was already contiguous int64, no copy.
    py::array_t<int64_t, py::array::c_style | py::array::forcecast> in(raw);
    const int64_t* in_ptr = in.data();

    std::vector<py::ssize_t> shape(in.shape(), in.shape() + in.ndim());
    py::array_t<double> out(shape);
    double* out_ptr = out.mutable_data();

    const double inv_scale = 1.0 / static_cast<double>(scale);
    const py::ssize_t n = in.size();
    for (py::ssize_t i = 0; i < n; ++i)
    {
      out_ptr[i] = static_cast<double>(in_ptr[i]) * inv_scale;
    }
    return out;
  };

  m.def(
      "prices_to_double",
      [rawToDouble](py::array_t<int64_t> raw)
      { return rawToDouble(raw, flox::Price::Scale); },
      "Convert raw int64 price array to float64 array (divides by PRICE_SCALE).",
      py::arg("raw"));

  m.def(
      "quantities_to_double",
      [rawToDouble](py::array_t<int64_t> raw)
      { return rawToDouble(raw, flox::Quantity::Scale); },
      "Convert raw int64 quantity array to float64 array (divides by QUANTITY_SCALE).",
      py::arg("raw"));

  m.def(
      "volumes_to_double",
      [rawToDouble](py::array_t<int64_t> raw)
      { return rawToDouble(raw, flox::Volume::Scale); },
      "Convert raw int64 volume array to float64 array (divides by VOLUME_SCALE).",
      py::arg("raw"));

  // Module-level inspect function
  m.def(
      "inspect",
      [](const std::string& dataDir) -> py::dict
      {
        flox::replay::DatasetSummary s;
        {
          py::gil_scoped_release release;
          s = flox::replay::BinaryLogReader::inspect(dataDir);
        }

        py::dict d;
        d["first_event_ns"] = s.first_event_ns;
        d["last_event_ns"] = s.last_event_ns;
        d["total_events"] = s.total_events;
        d["segment_count"] = s.segment_count;
        d["total_bytes"] = s.total_bytes;
        d["duration_seconds"] = s.durationSeconds();
        d["fully_indexed"] = s.fullyIndexed();
        return d;
      },
      "Inspect a data directory and return summary statistics",
      py::arg("data_dir"));

  // DataReader
  py::class_<PyDataReader>(m, "DataReader")
      .def(py::init<const std::string&, py::object, py::object, py::object,
                    py::object>(),
           "Create a DataReader for a binary log data directory. "
           "`reorder_window_ns` controls the bounded reorder buffer applied "
           "to segments without the Sorted flag (default 10s).",
           py::arg("data_dir"),
           py::arg("from_ns") = py::none(),
           py::arg("to_ns") = py::none(),
           py::arg("symbols") = py::none(),
           py::arg("reorder_window_ns") = py::none())
      .def("summary", &PyDataReader::summary,
           "Return a dict summarizing the dataset")
      .def("count", &PyDataReader::count,
           "Return the total number of events")
      .def("symbols", &PyDataReader::symbols,
           "Return the set of available symbol IDs")
      .def("time_range", &PyDataReader::timeRange,
           "Return (start_ns, end_ns) tuple or None")
      .def("read_trades", &PyDataReader::readTrades,
           "Read all trades as a numpy structured array (PyTrade dtype)")
      .def("read_trades_from", &PyDataReader::readTradesFrom,
           "Read trades starting from a given timestamp (nanoseconds)",
           py::arg("start_ts_ns"))
      .def("read_bbo", &PyDataReader::readBBO,
           "Read best bid/ask from every book update as a numpy structured array (PyBBO dtype)")
      .def("read_bbo_from", &PyDataReader::readBBOFrom,
           "Read BBO starting from a given timestamp (nanoseconds)",
           py::arg("start_ts_ns"))
      .def("read_book_updates", &PyDataReader::readBookUpdates,
           "Read all book updates. Returns (headers, levels) tuple of numpy structured arrays. "
           "headers dtype: PyBookUpdateHeader; levels dtype: PyLevel. "
           "For event i: bids=levels[h['level_offset']:h['level_offset']+h['bid_count']], "
           "asks=levels[h['level_offset']+h['bid_count']:h['level_offset']+h['bid_count']+h['ask_count']]")
      .def("read_book_updates_from", &PyDataReader::readBookUpdatesFrom,
           "Read book updates starting from a given timestamp (nanoseconds). "
           "Same return shape as read_book_updates().",
           py::arg("start_ts_ns"))
      .def("stats", &PyDataReader::stats,
           "Return reader statistics as a dict")
      .def("segment_files", &PyDataReader::segmentFiles,
           "Return list of segment file paths")
      .def("segments", &PyDataReader::segments,
           "Return list of segment info dicts")
      .def("run", &PyDataReader::run,
           "Run a panel of streaming aggregators over the tape in a single "
           "decompression pass. Each aggregator's onEvent fires once per "
           "event, then finalize() fires once at the end. An empty list is "
           "a no-op (no decompression). GIL released for the whole walk. "
           "n_threads=0 (default) is auto, resolved to "
           "min(blocks_per_segment/2, hardware_concurrency); n_threads=1 "
           "forces explicit single-thread; n_threads>1 partitions each "
           "segment at the compressed-block level via "
           "IAggregator::cloneEmpty()/merge() and is capped to the "
           "effective block count.",
           py::arg("aggregators"), py::arg("n_threads") = std::size_t{0});

  // DataWriter
  py::class_<PyDataWriter>(m, "DataWriter")
      .def(py::init<const std::string&, uint64_t, uint8_t, const std::string&>(),
           "Create a DataWriter for binary log output",
           py::arg("output_dir"),
           py::arg("max_segment_mb") = 256,
           py::arg("exchange_id") = 0,
           py::arg("compression") = "none")
      .def("write_trade", &PyDataWriter::writeTrade,
           "Write a single trade record",
           py::arg("exchange_ts_ns"), py::arg("recv_ts_ns"),
           py::arg("price"), py::arg("qty"),
           py::arg("trade_id"), py::arg("symbol_id"), py::arg("side"))
      .def("write_book", &PyDataWriter::writeBook,
           "Write a single book update. bids/asks are PyLevel structured arrays.",
           py::arg("exchange_ts_ns"), py::arg("recv_ts_ns"),
           py::arg("seq"), py::arg("symbol_id"), py::arg("is_snapshot"),
           py::arg("bids"), py::arg("asks"))
      .def("write_books", &PyDataWriter::writeBooks,
           "Batched book writer. Round-trip with DataReader.read_book_updates.",
           py::arg("headers"), py::arg("levels"))
      .def("write_trades", &PyDataWriter::writeTrades,
           "Write trades from numpy arrays (vectorized). Returns number of trades written.",
           py::arg("exchange_ts_ns"), py::arg("recv_ts_ns"),
           py::arg("prices"), py::arg("quantities"),
           py::arg("trade_ids"), py::arg("symbol_ids"), py::arg("sides"))
      .def("flush", &PyDataWriter::flush,
           "Flush buffered data to disk")
      .def("close", &PyDataWriter::close,
           "Close the writer and finalize all segments")
      .def("stats", &PyDataWriter::stats,
           "Return writer statistics as a dict")
      .def("current_segment_path", &PyDataWriter::currentSegmentPath,
           "Return the path of the current segment being written");

  // BinaryLogRecorderHook — built-in `.floxlog` sink. Plug into a runner
  // via `runner.set_market_data_recorder(hook)`; lifecycle (start/stop) is
  // driven automatically by the engine.
  py::class_<flox_py::PyBinaryLogRecorderHook, std::shared_ptr<flox_py::PyBinaryLogRecorderHook>>(
      m, "BinaryLogRecorderHook")
      .def(py::init<const std::string&, uint64_t, uint8_t, const std::string&,
                    const std::string&, const std::string&>(),
           "Create a binary-log recorder hook. Trades and books are written "
           "via BinaryLogWriter on the engine thread — no Python callback "
           "per event. exchange_name + instrument_type are stamped into "
           "the recording's metadata.json so MergedTapeReader can key by "
           "(exchange, name) across multiple tapes.",
           py::arg("output_dir"),
           py::arg("max_segment_mb") = 256,
           py::arg("exchange_id") = 0,
           py::arg("compression") = "none",
           py::arg("exchange_name") = "",
           py::arg("instrument_type") = "")
      .def("add_symbol", &flox_py::PyBinaryLogRecorderHook::addSymbol,
           "Register a symbol for recording metadata",
           py::arg("symbol_id"), py::arg("name"),
           py::arg("base") = "", py::arg("quote") = "",
           py::arg("price_precision") = 8, py::arg("qty_precision") = 8)
      .def("flush", &flox_py::PyBinaryLogRecorderHook::flush,
           "Flush buffered data to disk")
      .def("close", &flox_py::PyBinaryLogRecorderHook::close,
           "Stop recording (idempotent; engine on_stop also calls this)")
      .def("stats", &flox_py::PyBinaryLogRecorderHook::stats,
           "Return recorder statistics as a dict")
      .def("current_segment_path", &flox_py::PyBinaryLogRecorderHook::currentSegmentPath,
           "Return the path of the segment currently being written");

  // MergedTapeReader — k-tape consumption primitive.
  py::class_<PyMergedTapeReader>(m, "MergedTapeReader")
      .def(py::init<const std::vector<std::string>&, py::object, py::object,
                    py::object>(),
           "Open N `.floxlog` directories and expose a merged trade / book "
           "stream. Symbols are rekeyed into a global id space, keyed by "
           "(metadata.exchange, name). Ties on exchange_ts_ns are broken by "
           "tape order in `paths`. read_trades / read_books are eager — "
           "they materialise the merged arrays.",
           py::arg("paths"), py::arg("from_ns") = py::none(),
           py::arg("to_ns") = py::none(), py::arg("symbols") = py::none())
      .def("symbol_table", &PyMergedTapeReader::symbolTable,
           "List of dicts: global_id, exchange, name, price_precision, "
           "qty_precision.")
      .def("time_range", &PyMergedTapeReader::timeRange,
           "(min first_event_ns, max last_event_ns) across all tapes.")
      .def("summary", &PyMergedTapeReader::summary,
           "Aggregate stats: first_event_ns, last_event_ns, "
           "total_events (populated after readTrades/readBooks), "
           "tape_count, symbol_count.")
      .def("read_trades", &PyMergedTapeReader::readTrades,
           "Merged trades as PyTrade structured numpy array, sorted by "
           "exchange_ts_ns; tie-break by tape order.")
      .def("read_books", &PyMergedTapeReader::readBooks,
           "(headers, levels) tuple. Headers carry global symbol_id; "
           "level_offset slices the levels array per event.")
      .def("per_tape_stats", &PyMergedTapeReader::perTapeStats,
           "Per-tape breakdown — useful for debugging an empty input.")
      .def("stream_events", &PyMergedTapeReader::streamEvents,
           "Walk the merged stream via N-way heap merge (O(N tapes) "
           "peak memory). Calls on_trade(exchange_ts_ns, recv_ts_ns, "
           "price_raw, qty_raw, symbol_id, tape_index, side) and "
           "on_book(exchange_ts_ns, recv_ts_ns, symbol_id, "
           "tape_index, is_snapshot, bids, asks). bids/asks are "
           "lists of (price_raw, qty_raw) tuples. Returning False "
           "from either aborts the walk.",
           py::arg("on_trade") = py::none(),
           py::arg("on_book") = py::none())
      .def("run", &PyMergedTapeReader::run,
           "Run a panel of streaming aggregators over the merged tape "
           "stream in a single decompression pass. Same semantics as "
           "DataReader.run — events are seen with global-rewritten "
           "symbol ids; per-tape provenance is not surfaced to "
           "aggregators (reach for stream_events directly when needed).",
           py::arg("aggregators"));
}
