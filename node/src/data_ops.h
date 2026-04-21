// node/src/data_ops.h -- DataWriter, DataReader, segment ops

#pragma once
#include <napi.h>
#include "flox/capi/flox_capi.h"

namespace node_flox
{

class DataWriterWrap : public Napi::ObjectWrap<DataWriterWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "DataWriter",
      {InstanceMethod("writeTrade", &DataWriterWrap::WriteTrade),
       InstanceMethod("flush", &DataWriterWrap::Flush),
       InstanceMethod("close", &DataWriterWrap::Close)});
  }
  DataWriterWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<DataWriterWrap>(info)
  {
    std::string dir = info[0].As<Napi::String>().Utf8Value();
    uint64_t maxMb = info.Length() > 1 ? info[1].As<Napi::Number>().Int64Value() : 256;
    uint8_t exId = info.Length() > 2 ? info[2].As<Napi::Number>().Uint32Value() : 0;
    _h = flox_data_writer_create(dir.c_str(), maxMb, exId);
  }
  ~DataWriterWrap() { if (_h) { flox_data_writer_close(_h); flox_data_writer_destroy(_h); } }
 private:
  Napi::Value WriteTrade(const Napi::CallbackInfo& info)
  {
    return Napi::Boolean::New(info.Env(), flox_data_writer_write_trade(_h,
      info[0].As<Napi::Number>().Int64Value(), info[1].As<Napi::Number>().Int64Value(),
      info[2].As<Napi::Number>().DoubleValue(), info[3].As<Napi::Number>().DoubleValue(),
      info[4].As<Napi::Number>().Int64Value(), info[5].As<Napi::Number>().Uint32Value(),
      info[6].As<Napi::Number>().Uint32Value()));
  }
  void Flush(const Napi::CallbackInfo&) { flox_data_writer_flush(_h); }
  void Close(const Napi::CallbackInfo&) { flox_data_writer_close(_h); }
  FloxDataWriterHandle _h = nullptr;
};

class DataReaderWrap : public Napi::ObjectWrap<DataReaderWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "DataReader",
      {InstanceAccessor("count", &DataReaderWrap::Count, nullptr)});
  }
  DataReaderWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<DataReaderWrap>(info)
  {
    _h = flox_data_reader_create(info[0].As<Napi::String>().Utf8Value().c_str());
  }
  ~DataReaderWrap() { if (_h) flox_data_reader_destroy(_h); }
 private:
  Napi::Value Count(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), (double)flox_data_reader_count(_h)); }
  FloxDataReaderHandle _h = nullptr;
};

inline Napi::Value seg_validate(const Napi::CallbackInfo& info)
{
  return Napi::Boolean::New(info.Env(), flox_segment_validate(info[0].As<Napi::String>().Utf8Value().c_str()));
}

inline Napi::Value seg_merge(const Napi::CallbackInfo& info)
{
  return Napi::Boolean::New(info.Env(), flox_segment_merge(
    info[0].As<Napi::String>().Utf8Value().c_str(),
    info[1].As<Napi::String>().Utf8Value().c_str()));
}

inline void registerDataOps(Napi::Env env, Napi::Object exports)
{
  exports.Set("DataWriter", DataWriterWrap::Init(env));
  exports.Set("DataReader", DataReaderWrap::Init(env));
  exports.Set("validateSegment", Napi::Function::New(env, seg_validate));
  exports.Set("mergeSegments", Napi::Function::New(env, seg_merge));
}

}  // namespace node_flox
