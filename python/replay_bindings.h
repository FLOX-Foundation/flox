// python/replay_bindings.h

#pragma once

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "flox/common.h"
#include "flox/replay/binary_format_v1.h"
#include "flox/replay/market_data_recorder.h"
#include "flox/replay/readers/binary_log_reader.h"
#include "flox/replay/recording_metadata.h"
#include "flox/replay/writers/binary_log_writer.h"

#include <cstring>
#include <string>
#include <vector>

namespace py = pybind11;

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

// ─── Helpers ───────────────────────────────────────────────────────────────────

inline PyTrade tradeRecordToPyTrade(const flox::replay::TradeRecord& tr)
{
  return {tr.exchange_ts_ns, tr.recv_ts_ns, tr.price_raw, tr.qty_raw, tr.trade_id, tr.symbol_id, tr.side, {}};
}

inline ReaderConfig makeReaderConfig(const std::string& dataDir, py::object fromNs,
                                     py::object toNs, py::object symbols)
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
    throw std::invalid_argument("Unknown compression type: '" + compression +
                                "' (expected 'none' or 'lz4')");
  }

  return cfg;
}

inline MarketDataRecorderConfig makeRecorderConfig(const std::string& outputDir,
                                                   const std::string& exchangeName,
                                                   uint64_t maxSegmentMb)
{
  MarketDataRecorderConfig cfg;
  cfg.output_dir = outputDir;
  cfg.exchange_name = exchangeName;
  cfg.max_segment_bytes = maxSegmentMb * (1024ull * 1024ull);
  return cfg;
}

// ─── PyDataReader ──────────────────────────────────────────────────────────────

class PyDataReader
{
  BinaryLogReader _reader;

 public:
  PyDataReader(const std::string& dataDir, py::object fromNs, py::object toNs, py::object symbols)
      : _reader(makeReaderConfig(dataDir, fromNs, toNs, symbols))
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
      throw std::invalid_argument("All arrays must have the same length");
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

// ─── PyDataRecorder ────────────────────────────────────────────────────────────

class PyDataRecorder
{
  MarketDataRecorder _recorder;

 public:
  PyDataRecorder(const std::string& outputDir, const std::string& exchangeName,
                 uint64_t maxSegmentMb)
      : _recorder(makeRecorderConfig(outputDir, exchangeName, maxSegmentMb))
  {
  }

  void addSymbol(uint32_t symbolId, const std::string& name, const std::string& base,
                 const std::string& quote, int8_t pricePrecision, int8_t qtyPrecision)
  {
    _recorder.addSymbol(symbolId, name, base, quote, pricePrecision, qtyPrecision);
  }

  void start() { _recorder.start(); }
  void stop() { _recorder.stop(); }
  void flush() { _recorder.flush(); }
  bool isRecording() const { return _recorder.isRecording(); }

  py::dict stats()
  {
    auto s = _recorder.stats();
    py::dict d;
    d["trades_written"] = s.trades_written;
    d["book_updates_written"] = s.book_updates_written;
    d["bytes_written"] = s.bytes_written;
    d["files_created"] = s.files_created;
    d["errors"] = s.errors;
    return d;
  }
};

}  // namespace

// ─── Module binding ────────────────────────────────────────────────────────────

inline void bindReplay(py::module_& m)
{
  PYBIND11_NUMPY_DTYPE(PyTrade, exchange_ts_ns, recv_ts_ns, price_raw, qty_raw, trade_id,
                       symbol_id, side);

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
      .def(py::init<const std::string&, py::object, py::object, py::object>(),
           "Create a DataReader for a binary log data directory",
           py::arg("data_dir"),
           py::arg("from_ns") = py::none(),
           py::arg("to_ns") = py::none(),
           py::arg("symbols") = py::none())
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
      .def("stats", &PyDataReader::stats,
           "Return reader statistics as a dict")
      .def("segment_files", &PyDataReader::segmentFiles,
           "Return list of segment file paths")
      .def("segments", &PyDataReader::segments,
           "Return list of segment info dicts");

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

  // DataRecorder
  py::class_<PyDataRecorder>(m, "DataRecorder")
      .def(py::init<const std::string&, const std::string&, uint64_t>(),
           "Create a DataRecorder for market data recording",
           py::arg("output_dir"),
           py::arg("exchange_name"),
           py::arg("max_segment_mb") = 256)
      .def("add_symbol", &PyDataRecorder::addSymbol,
           "Register a symbol for recording metadata",
           py::arg("symbol_id"), py::arg("name"),
           py::arg("base") = "", py::arg("quote") = "",
           py::arg("price_precision") = 8, py::arg("qty_precision") = 8)
      .def("start", &PyDataRecorder::start,
           "Start recording")
      .def("stop", &PyDataRecorder::stop,
           "Stop recording and finalize output")
      .def("flush", &PyDataRecorder::flush,
           "Flush buffered data to disk")
      .def("is_recording", &PyDataRecorder::isRecording,
           "Return True if currently recording")
      .def("stats", &PyDataRecorder::stats,
           "Return recorder statistics as a dict");
}
