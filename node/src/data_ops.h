// node/src/data_ops.h -- DataWriter, DataReader, BinaryLogRecorderHook, Partitioner, segment ops

#pragma once
#include <napi.h>
#include <cstring>
#include <vector>
#include "flox/capi/flox_capi.h"

namespace node_flox
{

// JS Numbers are float64 and lose precision past 2^53, which truncates
// nanosecond timestamps in unpredictable ways (e.g. 1765615835519000000
// round-trips as 1765615835519000064). Accept BigInt for any int64_t arg
// that may hold a real ns timestamp; fall back to Number for callers that
// pass smaller values.
inline int64_t toInt64Ns(const Napi::Value& v)
{
  if (v.IsBigInt())
  {
    bool lossless = false;
    return v.As<Napi::BigInt>().Int64Value(&lossless);
  }
  return v.As<Napi::Number>().Int64Value();
}

// ── DataWriter ──────────────────────────────────────────────────────

class DataWriterWrap : public Napi::ObjectWrap<DataWriterWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "DataWriter",
                       {InstanceMethod("writeTrade", &DataWriterWrap::WriteTrade),
                        InstanceMethod("writeBook", &DataWriterWrap::WriteBook),
                        InstanceMethod("flush", &DataWriterWrap::Flush),
                        InstanceMethod("close", &DataWriterWrap::Close),
                        InstanceMethod("stats", &DataWriterWrap::Stats)});
  }
  DataWriterWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<DataWriterWrap>(info)
  {
    std::string dir = info[0].As<Napi::String>().Utf8Value();
    uint64_t maxMb = info.Length() > 1 ? info[1].As<Napi::Number>().Int64Value() : 256;
    uint8_t exId = info.Length() > 2 ? info[2].As<Napi::Number>().Uint32Value() : 0;
    _h = flox_data_writer_create(dir.c_str(), maxMb, exId);
  }
  ~DataWriterWrap()
  {
    if (_h)
    {
      flox_data_writer_close(_h);
      flox_data_writer_destroy(_h);
    }
  }

 private:
  Napi::Value WriteTrade(const Napi::CallbackInfo& info)
  {
    return Napi::Boolean::New(info.Env(), flox_data_writer_write_trade(_h,
                                                                       info[0].As<Napi::Number>().Int64Value(), info[1].As<Napi::Number>().Int64Value(),
                                                                       info[2].As<Napi::Number>().DoubleValue(), info[3].As<Napi::Number>().DoubleValue(),
                                                                       info[4].As<Napi::Number>().Int64Value(), info[5].As<Napi::Number>().Uint32Value(),
                                                                       info[6].As<Napi::Number>().Uint32Value()));
  }
  // bids/asks are flat BigInt64Array: [price_raw, qty_raw, price_raw, qty_raw, ...].
  // Reinterpret the buffer as FloxBookLevel* — layout matches (two int64 per level).
  Napi::Value WriteBook(const Napi::CallbackInfo& info)
  {
    int64_t exchTs = toInt64Ns(info[0]);
    int64_t recvTs = toInt64Ns(info[1]);
    int64_t seq = toInt64Ns(info[2]);
    uint32_t symId = info[3].As<Napi::Number>().Uint32Value();
    uint8_t isSnap = info[4].As<Napi::Boolean>().Value() ? 1u : 0u;
    auto bidsArr = info[5].As<Napi::BigInt64Array>();
    auto asksArr = info[6].As<Napi::BigInt64Array>();
    const FloxBookLevel* bids =
        reinterpret_cast<const FloxBookLevel*>(bidsArr.Data());
    const FloxBookLevel* asks =
        reinterpret_cast<const FloxBookLevel*>(asksArr.Data());
    uint32_t nBids = static_cast<uint32_t>(bidsArr.ElementLength() / 2);
    uint32_t nAsks = static_cast<uint32_t>(asksArr.ElementLength() / 2);
    return Napi::Boolean::New(info.Env(),
                              flox_data_writer_write_book(_h, exchTs, recvTs, seq, symId,
                                                          isSnap, bids, nBids, asks, nAsks));
  }
  void Flush(const Napi::CallbackInfo&) { flox_data_writer_flush(_h); }
  void Close(const Napi::CallbackInfo&) { flox_data_writer_close(_h); }
  Napi::Value Stats(const Napi::CallbackInfo& info)
  {
    auto s = flox_data_writer_stats(_h);
    auto o = Napi::Object::New(info.Env());
    o.Set("bytesWritten", (double)s.bytes_written);
    o.Set("eventsWritten", (double)s.events_written);
    o.Set("segmentsCreated", (double)s.segments_created);
    o.Set("tradesWritten", (double)s.trades_written);
    return o;
  }
  FloxDataWriterHandle _h = nullptr;
};

// ── DataReader ──────────────────────────────────────────────────────

class DataReaderWrap : public Napi::ObjectWrap<DataReaderWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "DataReader",
                       {InstanceAccessor("count", &DataReaderWrap::Count, nullptr),
                        InstanceMethod("summary", &DataReaderWrap::Summary),
                        InstanceMethod("stats", &DataReaderWrap::Stats),
                        InstanceMethod("readTrades", &DataReaderWrap::ReadTrades),
                        InstanceMethod("readTradesFrom", &DataReaderWrap::ReadTradesFrom),
                        InstanceMethod("readBBO", &DataReaderWrap::ReadBBO),
                        InstanceMethod("readBBOFrom", &DataReaderWrap::ReadBBOFrom),
                        InstanceMethod("readBookUpdates", &DataReaderWrap::ReadBookUpdates),
                        InstanceMethod("readBookUpdatesFrom", &DataReaderWrap::ReadBookUpdatesFrom)});
  }
  DataReaderWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<DataReaderWrap>(info)
  {
    std::string dir = info[0].As<Napi::String>().Utf8Value();
    int64_t from = info.Length() > 1 && (info[1].IsNumber() || info[1].IsBigInt()) ? toInt64Ns(info[1]) : 0;
    int64_t to = info.Length() > 2 && (info[2].IsNumber() || info[2].IsBigInt()) ? toInt64Ns(info[2]) : 0;
    // TODO: symbol filter array
    _h = flox_data_reader_create_filtered(dir.c_str(), from, to, nullptr, 0);
  }
  ~DataReaderWrap()
  {
    if (_h)
    {
      flox_data_reader_destroy(_h);
    }
  }

 private:
  Napi::Value Count(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), (double)flox_data_reader_count(_h)); }
  Napi::Value Summary(const Napi::CallbackInfo& info)
  {
    auto s = flox_data_reader_summary(_h);
    auto o = Napi::Object::New(info.Env());
    o.Set("firstEventNs", (double)s.first_event_ns);
    o.Set("lastEventNs", (double)s.last_event_ns);
    o.Set("totalEvents", (double)s.total_events);
    o.Set("segmentCount", (double)s.segment_count);
    o.Set("totalBytes", (double)s.total_bytes);
    o.Set("durationSeconds", s.duration_seconds);
    return o;
  }
  Napi::Value Stats(const Napi::CallbackInfo& info)
  {
    auto s = flox_data_reader_stats(_h);
    auto o = Napi::Object::New(info.Env());
    o.Set("filesRead", (double)s.files_read);
    o.Set("eventsRead", (double)s.events_read);
    o.Set("tradesRead", (double)s.trades_read);
    o.Set("bookUpdatesRead", (double)s.book_updates_read);
    o.Set("bytesRead", (double)s.bytes_read);
    o.Set("crcErrors", (double)s.crc_errors);
    return o;
  }
  Napi::Value ReadTrades(const Napi::CallbackInfo& info)
  {
    uint64_t maxTrades = info.Length() > 0 && info[0].IsNumber()
                             ? info[0].As<Napi::Number>().Int64Value()
                             : 0;
    return readTradesImpl(info.Env(), /*startTsNs=*/0, /*useFrom=*/false, maxTrades);
  }
  Napi::Value ReadTradesFrom(const Napi::CallbackInfo& info)
  {
    int64_t startTsNs = toInt64Ns(info[0]);
    uint64_t maxTrades = info.Length() > 1 && info[1].IsNumber()
                             ? info[1].As<Napi::Number>().Int64Value()
                             : 0;
    return readTradesImpl(info.Env(), startTsNs, /*useFrom=*/true, maxTrades);
  }
  Napi::Value ReadBBO(const Napi::CallbackInfo& info)
  {
    uint64_t maxEvents = info.Length() > 0 && info[0].IsNumber()
                             ? info[0].As<Napi::Number>().Int64Value()
                             : 0;
    return readBboImpl(info.Env(), /*startTsNs=*/0, /*useFrom=*/false, maxEvents);
  }
  Napi::Value ReadBBOFrom(const Napi::CallbackInfo& info)
  {
    int64_t startTsNs = toInt64Ns(info[0]);
    uint64_t maxEvents = info.Length() > 1 && info[1].IsNumber()
                             ? info[1].As<Napi::Number>().Int64Value()
                             : 0;
    return readBboImpl(info.Env(), startTsNs, /*useFrom=*/true, maxEvents);
  }

 private:
  Napi::Value readTradesImpl(Napi::Env env, int64_t startTsNs, bool useFrom, uint64_t maxTrades)
  {
    if (maxTrades == 0)
    {
      maxTrades = useFrom ? flox_data_reader_read_trades_from(_h, startTsNs, nullptr, 0)
                          : flox_data_reader_read_trades(_h, nullptr, 0);
    }
    std::vector<FloxTradeRecord> trades(maxTrades);
    uint64_t n = useFrom
                     ? flox_data_reader_read_trades_from(_h, startTsNs, trades.data(), maxTrades)
                     : flox_data_reader_read_trades(_h, trades.data(), maxTrades);
    auto arr = Napi::Array::New(env, n);
    for (uint64_t i = 0; i < n; i++)
    {
      auto o = Napi::Object::New(env);
      o.Set("exchangeTsNs", (double)trades[i].exchange_ts_ns);
      o.Set("recvTsNs", (double)trades[i].recv_ts_ns);
      o.Set("price", (double)trades[i].price_raw / 1e8);
      o.Set("qty", (double)trades[i].qty_raw / 1e8);
      o.Set("tradeId", (double)trades[i].trade_id);
      o.Set("symbolId", trades[i].symbol_id);
      o.Set("side", trades[i].side);
      arr.Set((uint32_t)i, o);
    }
    return arr;
  }

  Napi::Value readBboImpl(Napi::Env env, int64_t startTsNs, bool useFrom, uint64_t maxEvents)
  {
    if (maxEvents == 0)
    {
      maxEvents = useFrom ? flox_data_reader_read_bbo_from(_h, startTsNs, nullptr, 0)
                          : flox_data_reader_read_bbo(_h, nullptr, 0);
    }
    std::vector<FloxBBO> bbos(maxEvents);
    uint64_t n = useFrom
                     ? flox_data_reader_read_bbo_from(_h, startTsNs, bbos.data(), maxEvents)
                     : flox_data_reader_read_bbo(_h, bbos.data(), maxEvents);
    auto arr = Napi::Array::New(env, n);
    for (uint64_t i = 0; i < n; i++)
    {
      auto o = Napi::Object::New(env);
      o.Set("exchangeTsNs", (double)bbos[i].exchange_ts_ns);
      o.Set("recvTsNs", (double)bbos[i].recv_ts_ns);
      o.Set("seq", (double)bbos[i].seq);
      o.Set("symbolId", bbos[i].symbol_id);
      o.Set("eventType", bbos[i].event_type);
      o.Set("bidPrice", (double)bbos[i].bid_price_raw / 1e8);
      o.Set("bidQty", (double)bbos[i].bid_qty_raw / 1e8);
      o.Set("askPrice", (double)bbos[i].ask_price_raw / 1e8);
      o.Set("askQty", (double)bbos[i].ask_qty_raw / 1e8);
      arr.Set((uint32_t)i, o);
    }
    return arr;
  }

 public:
  // Returns an array of book update events. Each event:
  //   { exchangeTsNs, recvTsNs, seq, symbolId, eventType,
  //     bids: [{price, qty}, ...], asks: [{price, qty}, ...] }
  Napi::Value ReadBookUpdates(const Napi::CallbackInfo& info)
  {
    return readBookUpdatesImpl(info.Env(), /*startTsNs=*/0, /*useFrom=*/false);
  }
  Napi::Value ReadBookUpdatesFrom(const Napi::CallbackInfo& info)
  {
    int64_t startTsNs = toInt64Ns(info[0]);
    return readBookUpdatesImpl(info.Env(), startTsNs, /*useFrom=*/true);
  }

 private:
  Napi::Value readBookUpdatesImpl(Napi::Env env, int64_t startTsNs, bool useFrom)
  {
    uint64_t totalLevels = 0;
    uint64_t maxEvents = useFrom
                             ? flox_data_reader_count_book_updates_from(_h, startTsNs, &totalLevels)
                             : flox_data_reader_count_book_updates(_h, &totalLevels);

    std::vector<FloxBookUpdateHeader> headers(maxEvents);
    std::vector<FloxLevel> levels(totalLevels);
    uint64_t n = useFrom
                     ? flox_data_reader_read_book_updates_from(_h, startTsNs, headers.data(),
                                                               maxEvents, levels.data(),
                                                               totalLevels)
                     : flox_data_reader_read_book_updates(_h, headers.data(), maxEvents,
                                                          levels.data(), totalLevels);

    auto arr = Napi::Array::New(env, n);
    for (uint64_t i = 0; i < n; i++)
    {
      const auto& h = headers[i];
      auto o = Napi::Object::New(env);
      o.Set("exchangeTsNs", (double)h.exchange_ts_ns);
      o.Set("recvTsNs", (double)h.recv_ts_ns);
      o.Set("seq", (double)h.seq);
      o.Set("symbolId", h.symbol_id);
      o.Set("eventType", h.event_type);

      auto bids = Napi::Array::New(env, h.bid_count);
      for (uint16_t k = 0; k < h.bid_count; k++)
      {
        const auto& l = levels[h.level_offset + k];
        auto lo = Napi::Object::New(env);
        lo.Set("price", (double)l.price_raw / 1e8);
        lo.Set("qty", (double)l.qty_raw / 1e8);
        bids.Set((uint32_t)k, lo);
      }
      o.Set("bids", bids);

      auto asks = Napi::Array::New(env, h.ask_count);
      for (uint16_t k = 0; k < h.ask_count; k++)
      {
        const auto& l = levels[h.level_offset + h.bid_count + k];
        auto lo = Napi::Object::New(env);
        lo.Set("price", (double)l.price_raw / 1e8);
        lo.Set("qty", (double)l.qty_raw / 1e8);
        asks.Set((uint32_t)k, lo);
      }
      o.Set("asks", asks);

      arr.Set((uint32_t)i, o);
    }
    return arr;
  }

 public:
  FloxDataReaderHandle _h = nullptr;
};

// ── BinaryLogRecorderHook ───────────────────────────────────────────
//
// Built-in .floxlog recorder. Owns a BinaryLogWriter on the engine
// thread — events stay in C++ on the hot path, no JS callbacks.
// Attach via `runner.setMarketDataRecorder(hook)`.

class BinaryLogRecorderHookWrap : public Napi::ObjectWrap<BinaryLogRecorderHookWrap>
{
 public:
  // Cached at module init; used by RunnerNode::setMarketDataRecorder for
  // an InstanceOf check before unwrapping.
  static Napi::FunctionReference& Ctor()
  {
    static Napi::FunctionReference ref;
    return ref;
  }
  static Napi::Function Init(Napi::Env env)
  {
    Napi::Function fn = DefineClass(env, "BinaryLogRecorderHook",
                                    {InstanceMethod("addSymbol", &BinaryLogRecorderHookWrap::AddSymbol),
                                     InstanceMethod("flush", &BinaryLogRecorderHookWrap::Flush),
                                     InstanceMethod("stats", &BinaryLogRecorderHookWrap::Stats)});
    Ctor() = Napi::Persistent(fn);
    Ctor().SuppressDestruct();
    return fn;
  }
  BinaryLogRecorderHookWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<BinaryLogRecorderHookWrap>(info)
  {
    auto env = info.Env();
    std::string dir = info[0].As<Napi::String>().Utf8Value();
    uint64_t maxMb = info.Length() > 1 && info[1].IsNumber()
                         ? info[1].As<Napi::Number>().Int64Value()
                         : 256;
    uint8_t exId = info.Length() > 2 && info[2].IsNumber()
                       ? info[2].As<Napi::Number>().Uint32Value()
                       : 0;
    uint8_t compression = 0;
    if (info.Length() > 3 && info[3].IsString())
    {
      auto s = info[3].As<Napi::String>().Utf8Value();
      if (s == "none")
      {
        compression = 0;
      }
      else if (s == "lz4")
      {
        compression = 1;
      }
      else
      {
        Napi::TypeError::New(env, "compression must be \"none\" or \"lz4\"")
            .ThrowAsJavaScriptException();
        return;
      }
    }
    _h = flox_binary_log_recorder_hook_create(dir.c_str(), maxMb, exId, compression);
  }
  ~BinaryLogRecorderHookWrap()
  {
    if (_h)
    {
      flox_binary_log_recorder_hook_destroy(_h);
    }
  }

  // Borrowed handle for runner attachment. Lifetime tied to this hook.
  FloxMarketDataRecorderHandle asRecorder() const
  {
    return _h ? flox_binary_log_recorder_hook_as_recorder(_h) : nullptr;
  }

 private:
  void AddSymbol(const Napi::CallbackInfo& info)
  {
    flox_binary_log_recorder_hook_add_symbol(
        _h,
        info[0].As<Napi::Number>().Uint32Value(),
        info[1].As<Napi::String>().Utf8Value().c_str(),
        info.Length() > 2 ? info[2].As<Napi::String>().Utf8Value().c_str() : "",
        info.Length() > 3 ? info[3].As<Napi::String>().Utf8Value().c_str() : "",
        static_cast<int8_t>(info.Length() > 4 ? info[4].As<Napi::Number>().Int32Value() : 8),
        static_cast<int8_t>(info.Length() > 5 ? info[5].As<Napi::Number>().Int32Value() : 8));
  }
  void Flush(const Napi::CallbackInfo&) { flox_binary_log_recorder_hook_flush(_h); }
  Napi::Value Stats(const Napi::CallbackInfo& info)
  {
    auto s = flox_binary_log_recorder_hook_stats(_h);
    auto env = info.Env();
    auto o = Napi::Object::New(env);
    o.Set("tradesWritten", Napi::BigInt::New(env, s.trades_written));
    o.Set("bookUpdatesWritten",
          Napi::BigInt::New(env, s.events_written - s.trades_written));
    o.Set("bytesWritten", Napi::BigInt::New(env, s.bytes_written));
    o.Set("segmentsCreated", Napi::BigInt::New(env, s.segments_created));
    o.Set("errors", Napi::BigInt::New(env, uint64_t{0}));
    return o;
  }
  FloxBinaryLogRecorderHookHandle _h = nullptr;
};

// ── Partitioner ─────────────────────────────────────────────────────

class PartitionerWrap : public Napi::ObjectWrap<PartitionerWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "Partitioner",
                       {InstanceMethod("byTime", &PartitionerWrap::ByTime),
                        InstanceMethod("byDuration", &PartitionerWrap::ByDuration),
                        InstanceMethod("byCalendar", &PartitionerWrap::ByCalendar),
                        InstanceMethod("bySymbol", &PartitionerWrap::BySymbol),
                        InstanceMethod("perSymbol", &PartitionerWrap::PerSymbol),
                        InstanceMethod("byEventCount", &PartitionerWrap::ByEventCount)});
  }
  PartitionerWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<PartitionerWrap>(info)
  {
    _h = flox_partitioner_create(info[0].As<Napi::String>().Utf8Value().c_str());
  }
  ~PartitionerWrap()
  {
    if (_h)
    {
      flox_partitioner_destroy(_h);
    }
  }

 private:
  Napi::Value toArray(const Napi::CallbackInfo& info, FloxPartition* parts, uint32_t n)
  {
    auto arr = Napi::Array::New(info.Env(), n);
    for (uint32_t i = 0; i < n; i++)
    {
      auto o = Napi::Object::New(info.Env());
      o.Set("partitionId", parts[i].partition_id);
      o.Set("fromNs", (double)parts[i].from_ns);
      o.Set("toNs", (double)parts[i].to_ns);
      o.Set("warmupFromNs", (double)parts[i].warmup_from_ns);
      o.Set("estimatedEvents", (double)parts[i].estimated_events);
      o.Set("estimatedBytes", (double)parts[i].estimated_bytes);
      arr.Set(i, o);
    }
    return arr;
  }
  Napi::Value ByTime(const Napi::CallbackInfo& info)
  {
    uint32_t n = info[0].As<Napi::Number>().Uint32Value();
    int64_t warmup = info.Length() > 1 ? info[1].As<Napi::Number>().Int64Value() : 0;
    uint32_t count = flox_partitioner_by_time(_h, n, warmup, nullptr, 0);
    std::vector<FloxPartition> parts(count);
    flox_partitioner_by_time(_h, n, warmup, parts.data(), count);
    return toArray(info, parts.data(), count);
  }
  Napi::Value ByDuration(const Napi::CallbackInfo& info)
  {
    int64_t dur = info[0].As<Napi::Number>().Int64Value();
    int64_t warmup = info.Length() > 1 ? info[1].As<Napi::Number>().Int64Value() : 0;
    uint32_t count = flox_partitioner_by_duration(_h, dur, warmup, nullptr, 0);
    std::vector<FloxPartition> parts(count);
    flox_partitioner_by_duration(_h, dur, warmup, parts.data(), count);
    return toArray(info, parts.data(), count);
  }
  Napi::Value ByCalendar(const Napi::CallbackInfo& info)
  {
    std::string u = info[0].As<Napi::String>().Utf8Value();
    uint8_t unit = 0;
    if (u == "day")
    {
      unit = 1;
    }
    else if (u == "week")
    {
      unit = 2;
    }
    else if (u == "month")
    {
      unit = 3;
    }
    int64_t warmup = info.Length() > 1 ? info[1].As<Napi::Number>().Int64Value() : 0;
    uint32_t count = flox_partitioner_by_calendar(_h, unit, warmup, nullptr, 0);
    std::vector<FloxPartition> parts(count);
    flox_partitioner_by_calendar(_h, unit, warmup, parts.data(), count);
    return toArray(info, parts.data(), count);
  }
  Napi::Value BySymbol(const Napi::CallbackInfo& info)
  {
    uint32_t n = info[0].As<Napi::Number>().Uint32Value();
    uint32_t count = flox_partitioner_by_symbol(_h, n, nullptr, 0);
    std::vector<FloxPartition> parts(count);
    flox_partitioner_by_symbol(_h, n, parts.data(), count);
    return toArray(info, parts.data(), count);
  }
  Napi::Value PerSymbol(const Napi::CallbackInfo& info)
  {
    uint32_t count = flox_partitioner_per_symbol(_h, nullptr, 0);
    std::vector<FloxPartition> parts(count);
    flox_partitioner_per_symbol(_h, parts.data(), count);
    return toArray(info, parts.data(), count);
  }
  Napi::Value ByEventCount(const Napi::CallbackInfo& info)
  {
    uint32_t n = info[0].As<Napi::Number>().Uint32Value();
    uint32_t count = flox_partitioner_by_event_count(_h, n, nullptr, 0);
    std::vector<FloxPartition> parts(count);
    flox_partitioner_by_event_count(_h, n, parts.data(), count);
    return toArray(info, parts.data(), count);
  }
  FloxPartitionerHandle _h = nullptr;
};

// ── Segment operations ──────────────────────────────────────────────

inline Napi::Value seg_validate(const Napi::CallbackInfo& info)
{
  return Napi::Boolean::New(info.Env(), flox_segment_validate(info[0].As<Napi::String>().Utf8Value().c_str()));
}

inline Napi::Value seg_validate_full(const Napi::CallbackInfo& info)
{
  auto r = flox_segment_validate_full(info[0].As<Napi::String>().Utf8Value().c_str(), 1, 1);
  auto o = Napi::Object::New(info.Env());
  o.Set("valid", (bool)r.valid);
  o.Set("headerValid", (bool)r.header_valid);
  o.Set("reportedEventCount", (double)r.reported_event_count);
  o.Set("actualEventCount", (double)r.actual_event_count);
  o.Set("hasIndex", (bool)r.has_index);
  o.Set("indexValid", (bool)r.index_valid);
  o.Set("tradesFound", (double)r.trades_found);
  o.Set("bookUpdatesFound", (double)r.book_updates_found);
  o.Set("crcErrors", r.crc_errors);
  o.Set("timestampAnomalies", r.timestamp_anomalies);
  return o;
}

inline Napi::Value seg_validate_dataset(const Napi::CallbackInfo& info)
{
  auto r = flox_dataset_validate(info[0].As<Napi::String>().Utf8Value().c_str());
  auto o = Napi::Object::New(info.Env());
  o.Set("valid", (bool)r.valid);
  o.Set("totalSegments", r.total_segments);
  o.Set("validSegments", r.valid_segments);
  o.Set("corruptedSegments", r.corrupted_segments);
  o.Set("totalEvents", (double)r.total_events);
  o.Set("totalBytes", (double)r.total_bytes);
  o.Set("firstTimestamp", (double)r.first_timestamp);
  o.Set("lastTimestamp", (double)r.last_timestamp);
  return o;
}

inline Napi::Value seg_merge(const Napi::CallbackInfo& info)
{
  return Napi::Boolean::New(info.Env(), flox_segment_merge(
                                            info[0].As<Napi::String>().Utf8Value().c_str(),
                                            info[1].As<Napi::String>().Utf8Value().c_str()));
}

inline Napi::Value seg_merge_dir(const Napi::CallbackInfo& info)
{
  auto r = flox_segment_merge_dir(info[0].As<Napi::String>().Utf8Value().c_str(),
                                  info[1].As<Napi::String>().Utf8Value().c_str());
  auto o = Napi::Object::New(info.Env());
  o.Set("success", (bool)r.success);
  o.Set("segmentsMerged", (double)r.segments_merged);
  o.Set("eventsWritten", (double)r.events_written);
  o.Set("bytesWritten", (double)r.bytes_written);
  return o;
}

inline Napi::Value seg_split(const Napi::CallbackInfo& info)
{
  uint8_t mode = 0;  // time
  std::string m = info.Length() > 2 ? info[2].As<Napi::String>().Utf8Value() : "time";
  if (m == "event_count")
  {
    mode = 1;
  }
  else if (m == "size")
  {
    mode = 2;
  }
  else if (m == "symbol")
  {
    mode = 3;
  }
  auto r = flox_segment_split(
      info[0].As<Napi::String>().Utf8Value().c_str(),
      info[1].As<Napi::String>().Utf8Value().c_str(),
      mode,
      info.Length() > 3 ? info[3].As<Napi::Number>().Int64Value() : 3600000000000LL,
      info.Length() > 4 ? info[4].As<Napi::Number>().Int64Value() : 1000000);
  auto o = Napi::Object::New(info.Env());
  o.Set("success", (bool)r.success);
  o.Set("segmentsCreated", r.segments_created);
  o.Set("eventsWritten", (double)r.events_written);
  return o;
}

inline Napi::Value seg_export(const Napi::CallbackInfo& info)
{
  uint8_t fmt = 0;
  if (info.Length() > 2 && info[2].IsString())
  {
    std::string f = info[2].As<Napi::String>().Utf8Value();
    if (f == "json")
    {
      fmt = 1;
    }
    else if (f == "jsonlines")
    {
      fmt = 2;
    }
    else if (f == "binary")
    {
      fmt = 3;
    }
  }
  auto r = flox_segment_export(
      info[0].As<Napi::String>().Utf8Value().c_str(),
      info[1].As<Napi::String>().Utf8Value().c_str(),
      fmt,
      info.Length() > 3 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int64Value() : 0,
      info.Length() > 4 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int64Value() : 0,
      nullptr, 0);
  auto o = Napi::Object::New(info.Env());
  o.Set("success", (bool)r.success);
  o.Set("eventsExported", (double)r.events_exported);
  o.Set("bytesWritten", (double)r.bytes_written);
  return o;
}

inline Napi::Value seg_recompress(const Napi::CallbackInfo& info)
{
  uint8_t c = 1;  // lz4
  if (info.Length() > 2)
  {
    std::string s = info[2].As<Napi::String>().Utf8Value();
    if (s == "none")
    {
      c = 0;
    }
  }
  return Napi::Boolean::New(info.Env(), flox_segment_recompress(
                                            info[0].As<Napi::String>().Utf8Value().c_str(),
                                            info[1].As<Napi::String>().Utf8Value().c_str(), c));
}

inline Napi::Value seg_extract_symbols(const Napi::CallbackInfo& info)
{
  auto syms = info[2].As<Napi::Uint32Array>();
  return Napi::Number::New(info.Env(), (double)flox_segment_extract_symbols(
                                           info[0].As<Napi::String>().Utf8Value().c_str(),
                                           info[1].As<Napi::String>().Utf8Value().c_str(),
                                           syms.Data(), syms.ElementLength()));
}

inline Napi::Value seg_extract_time_range(const Napi::CallbackInfo& info)
{
  return Napi::Number::New(info.Env(), (double)flox_segment_extract_time_range(
                                           info[0].As<Napi::String>().Utf8Value().c_str(),
                                           info[1].As<Napi::String>().Utf8Value().c_str(),
                                           info[2].As<Napi::Number>().Int64Value(),
                                           info[3].As<Napi::Number>().Int64Value()));
}

// ── Registration ────────────────────────────────────────────────────

inline Napi::Value seg_inspect(const Napi::CallbackInfo& info)
{
  auto h = flox_data_reader_create(info[0].As<Napi::String>().Utf8Value().c_str());
  auto s = flox_data_reader_summary(h);
  flox_data_reader_destroy(h);
  auto o = Napi::Object::New(info.Env());
  o.Set("firstEventNs", (double)s.first_event_ns);
  o.Set("lastEventNs", (double)s.last_event_ns);
  o.Set("totalEvents", (double)s.total_events);
  o.Set("segmentCount", (double)s.segment_count);
  o.Set("totalBytes", (double)s.total_bytes);
  o.Set("durationSeconds", s.duration_seconds);
  return o;
}

inline void registerDataOps(Napi::Env env, Napi::Object exports)
{
  exports.Set("DataWriter", DataWriterWrap::Init(env));
  exports.Set("DataReader", DataReaderWrap::Init(env));
  exports.Set("BinaryLogRecorderHook", BinaryLogRecorderHookWrap::Init(env));
  exports.Set("Partitioner", PartitionerWrap::Init(env));

  exports.Set("validateSegment", Napi::Function::New(env, seg_validate));
  exports.Set("validate", Napi::Function::New(env, seg_validate_full));
  exports.Set("validateDataset", Napi::Function::New(env, seg_validate_dataset));
  exports.Set("mergeSegments", Napi::Function::New(env, seg_merge));
  exports.Set("mergeDir", Napi::Function::New(env, seg_merge_dir));
  exports.Set("split", Napi::Function::New(env, seg_split));
  exports.Set("exportData", Napi::Function::New(env, seg_export));
  exports.Set("recompress", Napi::Function::New(env, seg_recompress));
  exports.Set("extractSymbols", Napi::Function::New(env, seg_extract_symbols));
  exports.Set("extractTimeRange", Napi::Function::New(env, seg_extract_time_range));
  exports.Set("inspect", Napi::Function::New(env, seg_inspect));
}

}  // namespace node_flox
