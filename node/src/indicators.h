// node/src/indicators.h -- batch + streaming indicator bindings

#pragma once
#include <napi.h>

#include "flox/indicator/adx.h"
#include "flox/indicator/atr.h"
#include "flox/indicator/bollinger.h"
#include "flox/indicator/cci.h"
#include "flox/indicator/chop.h"
#include "flox/indicator/cvd.h"
#include "flox/indicator/dema.h"
#include "flox/indicator/ema.h"
#include "flox/indicator/kama.h"
#include "flox/indicator/macd.h"
#include "flox/indicator/obv.h"
#include "flox/indicator/rma.h"
#include "flox/indicator/rsi.h"
#include "flox/indicator/slope.h"
#include "flox/indicator/sma.h"
#include "flox/indicator/stochastic.h"
#include "flox/indicator/vwap.h"

#include <cstring>
#include <span>

namespace node_flox
{

inline Napi::Float64Array vec2arr(Napi::Env env, const std::vector<double>& v)
{
  auto a = Napi::Float64Array::New(env, v.size());
  std::memcpy(a.Data(), v.data(), v.size() * sizeof(double));
  return a;
}

inline std::span<const double> arr2span(Napi::Float64Array& a)
{
  return {a.Data(), a.ElementLength()};
}

// ── Batch: single-input indicators ──────────────────────────────────

template <typename Indicator>
Napi::Value batchSingle(const Napi::CallbackInfo& info, Indicator ind)
{
  auto in = info[0].As<Napi::Float64Array>();
  size_t n = in.ElementLength();
  std::vector<double> out(n);
  ind.compute(std::span<const double>(in.Data(), n), std::span<double>(out.data(), n));
  return vec2arr(info.Env(), out);
}

#define BATCH_SINGLE(name, Ind)                                                                    \
  inline Napi::Value batch_##name(const Napi::CallbackInfo& info)                                  \
  {                                                                                                \
    size_t p = info[1].As<Napi::Number>().Uint32Value();                                           \
    return batchSingle(info, flox::indicator::Ind(p));                                             \
  }

BATCH_SINGLE(sma, SMA)
BATCH_SINGLE(ema, EMA)
BATCH_SINGLE(rsi, RSI)
BATCH_SINGLE(rma, RMA)
BATCH_SINGLE(slope, Slope)
BATCH_SINGLE(kama, KAMA)

#undef BATCH_SINGLE

inline Napi::Value batch_dema(const Napi::CallbackInfo& info)
{
  auto in = info[0].As<Napi::Float64Array>();
  size_t p = info[1].As<Napi::Number>().Uint32Value();
  auto r = flox::indicator::DEMA(p).compute(arr2span(in));
  return vec2arr(info.Env(), r);
}

inline Napi::Value batch_tema(const Napi::CallbackInfo& info)
{
  auto in = info[0].As<Napi::Float64Array>();
  size_t p = info[1].As<Napi::Number>().Uint32Value();
  auto r = flox::indicator::TEMA(p).compute(arr2span(in));
  return vec2arr(info.Env(), r);
}

// ── Batch: HLC indicators ───────────────────────────────────────────

inline Napi::Value batch_atr(const Napi::CallbackInfo& info)
{
  auto h = info[0].As<Napi::Float64Array>();
  auto l = info[1].As<Napi::Float64Array>();
  auto c = info[2].As<Napi::Float64Array>();
  size_t n = h.ElementLength(), p = info[3].As<Napi::Number>().Uint32Value();
  std::vector<double> out(n);
  flox::indicator::ATR(p).compute({h.Data(), n}, {l.Data(), n}, {c.Data(), n}, {out.data(), n});
  return vec2arr(info.Env(), out);
}

inline Napi::Value batch_adx(const Napi::CallbackInfo& info)
{
  auto h = info[0].As<Napi::Float64Array>();
  auto l = info[1].As<Napi::Float64Array>();
  auto c = info[2].As<Napi::Float64Array>();
  size_t n = h.ElementLength(), p = info[3].As<Napi::Number>().Uint32Value();
  auto r = flox::indicator::ADX(p).compute({h.Data(), n}, {l.Data(), n}, {c.Data(), n});
  auto o = Napi::Object::New(info.Env());
  o.Set("adx", vec2arr(info.Env(), r.adx));
  o.Set("plusDi", vec2arr(info.Env(), r.plus_di));
  o.Set("minusDi", vec2arr(info.Env(), r.minus_di));
  return o;
}

inline Napi::Value batch_stochastic(const Napi::CallbackInfo& info)
{
  auto h = info[0].As<Napi::Float64Array>();
  auto l = info[1].As<Napi::Float64Array>();
  auto c = info[2].As<Napi::Float64Array>();
  size_t n = h.ElementLength();
  size_t kp = info[3].As<Napi::Number>().Uint32Value();
  size_t dp = info[4].As<Napi::Number>().Uint32Value();
  auto r = flox::indicator::Stochastic(kp, dp).compute({h.Data(), n}, {l.Data(), n}, {c.Data(), n});
  auto o = Napi::Object::New(info.Env());
  o.Set("k", vec2arr(info.Env(), r.k));
  o.Set("d", vec2arr(info.Env(), r.d));
  return o;
}

inline Napi::Value batch_cci(const Napi::CallbackInfo& info)
{
  auto h = info[0].As<Napi::Float64Array>();
  auto l = info[1].As<Napi::Float64Array>();
  auto c = info[2].As<Napi::Float64Array>();
  size_t n = h.ElementLength(), p = info[3].As<Napi::Number>().Uint32Value();
  auto r = flox::indicator::CCI(p).compute({h.Data(), n}, {l.Data(), n}, {c.Data(), n});
  return vec2arr(info.Env(), r);
}

inline Napi::Value batch_chop(const Napi::CallbackInfo& info)
{
  auto h = info[0].As<Napi::Float64Array>();
  auto l = info[1].As<Napi::Float64Array>();
  auto c = info[2].As<Napi::Float64Array>();
  size_t n = h.ElementLength(), p = info[3].As<Napi::Number>().Uint32Value();
  auto r = flox::indicator::CHOP(p).compute({h.Data(), n}, {l.Data(), n}, {c.Data(), n});
  return vec2arr(info.Env(), r);
}

// ── Batch: multi-output / volume ────────────────────────────────────

inline Napi::Value batch_macd(const Napi::CallbackInfo& info)
{
  auto in = info[0].As<Napi::Float64Array>();
  size_t n = in.ElementLength();
  size_t f = info[1].As<Napi::Number>().Uint32Value();
  size_t s = info[2].As<Napi::Number>().Uint32Value();
  size_t sg = info[3].As<Napi::Number>().Uint32Value();
  std::vector<double> line(n), sig(n), hist(n);
  flox::indicator::MACD(f, s, sg).compute({in.Data(), n}, {line.data(), n}, {sig.data(), n},
                                          {hist.data(), n});
  auto o = Napi::Object::New(info.Env());
  o.Set("line", vec2arr(info.Env(), line));
  o.Set("signal", vec2arr(info.Env(), sig));
  o.Set("histogram", vec2arr(info.Env(), hist));
  return o;
}

inline Napi::Value batch_bollinger(const Napi::CallbackInfo& info)
{
  auto in = info[0].As<Napi::Float64Array>();
  size_t n = in.ElementLength(), p = info[1].As<Napi::Number>().Uint32Value();
  double sd = info[2].As<Napi::Number>().DoubleValue();
  auto r = flox::indicator::Bollinger(p, sd).compute({in.Data(), n});
  auto o = Napi::Object::New(info.Env());
  o.Set("upper", vec2arr(info.Env(), r.upper));
  o.Set("middle", vec2arr(info.Env(), r.middle));
  o.Set("lower", vec2arr(info.Env(), r.lower));
  return o;
}

inline Napi::Value batch_obv(const Napi::CallbackInfo& info)
{
  auto c = info[0].As<Napi::Float64Array>();
  auto v = info[1].As<Napi::Float64Array>();
  size_t n = c.ElementLength();
  std::vector<double> out(n);
  flox::indicator::OBV().compute({c.Data(), n}, {v.Data(), n}, {out.data(), n});
  return vec2arr(info.Env(), out);
}

inline Napi::Value batch_vwap(const Napi::CallbackInfo& info)
{
  auto c = info[0].As<Napi::Float64Array>();
  auto v = info[1].As<Napi::Float64Array>();
  size_t n = c.ElementLength(), w = info[2].As<Napi::Number>().Uint32Value();
  auto r = flox::indicator::VWAP(w).compute({c.Data(), n}, {v.Data(), n});
  return vec2arr(info.Env(), r);
}

inline Napi::Value batch_cvd(const Napi::CallbackInfo& info)
{
  auto o = info[0].As<Napi::Float64Array>();
  auto h = info[1].As<Napi::Float64Array>();
  auto l = info[2].As<Napi::Float64Array>();
  auto c = info[3].As<Napi::Float64Array>();
  auto v = info[4].As<Napi::Float64Array>();
  size_t n = c.ElementLength();
  auto r = flox::indicator::CVD().compute({o.Data(), n}, {h.Data(), n}, {l.Data(), n},
                                          {c.Data(), n}, {v.Data(), n});
  return vec2arr(info.Env(), r);
}

// ── Streaming indicator classes ─────────────────────────────────────

#define STREAMING_SINGLE(Name, updExpr)                                                            \
  class Name##Wrap : public Napi::ObjectWrap<Name##Wrap>                                           \
  {                                                                                                \
   public:                                                                                         \
    static Napi::Function Init(Napi::Env env)                                                      \
    {                                                                                              \
      return DefineClass(env, #Name,                                                               \
                         {InstanceMethod("update", &Name##Wrap::Update),                           \
                          InstanceAccessor("value", &Name##Wrap::Value, nullptr),                  \
                          InstanceAccessor("ready", &Name##Wrap::Ready, nullptr)});                \
    }                                                                                              \
    Name##Wrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<Name##Wrap>(info), _ind(info[0].As<Napi::Number>().Uint32Value()) {} \
   private:                                                                                        \
    Napi::Value Update(const Napi::CallbackInfo& info)                                             \
    {                                                                                              \
      updExpr;                                                                                     \
      return Napi::Number::New(info.Env(), _val);                                                  \
    }                                                                                              \
    Napi::Value Value(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _val); } \
    Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _ready); }

// SMA
class SMAWrap : public Napi::ObjectWrap<SMAWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "SMA",
                       {InstanceMethod("update", &SMAWrap::Update),
                        InstanceAccessor("value", &SMAWrap::Value, nullptr),
                        InstanceAccessor("ready", &SMAWrap::Ready, nullptr)});
  }
  SMAWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<SMAWrap>(info),
    _period(info[0].As<Napi::Number>().Uint32Value()), _buf(_period, 0) {}
 private:
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double v = info[0].As<Napi::Number>().DoubleValue();
    if (_count < _period) { _buf[_count] = v; _sum += v; _count++; }
    else { _sum -= _buf[_idx]; _buf[_idx] = v; _sum += v; _idx = (_idx + 1) % _period; }
    _val = _sum / _count;
    return Napi::Number::New(info.Env(), _val);
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _val); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _count >= _period); }
  size_t _period, _count = 0, _idx = 0;
  std::vector<double> _buf;
  double _sum = 0, _val = 0;
};

// EMA
class EMAWrap : public Napi::ObjectWrap<EMAWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "EMA",
                       {InstanceMethod("update", &EMAWrap::Update),
                        InstanceAccessor("value", &EMAWrap::Value, nullptr),
                        InstanceAccessor("ready", &EMAWrap::Ready, nullptr)});
  }
  EMAWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<EMAWrap>(info),
    _period(info[0].As<Napi::Number>().Uint32Value()), _mult(2.0 / (_period + 1)) {}
 private:
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double v = info[0].As<Napi::Number>().DoubleValue();
    if (_count < _period) { _sum += v; _count++; _val = _sum / _count; }
    else { _val = (v - _val) * _mult + _val; }
    return Napi::Number::New(info.Env(), _val);
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _val); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _count >= _period); }
  size_t _period, _count = 0;
  double _mult, _sum = 0, _val = 0;
};

// RSI
class RSIWrap : public Napi::ObjectWrap<RSIWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "RSI",
                       {InstanceMethod("update", &RSIWrap::Update),
                        InstanceAccessor("value", &RSIWrap::Value, nullptr),
                        InstanceAccessor("ready", &RSIWrap::Ready, nullptr)});
  }
  RSIWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<RSIWrap>(info),
    _period(info[0].As<Napi::Number>().Uint32Value()) {}
 private:
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double v = info[0].As<Napi::Number>().DoubleValue();
    if (_count == 0) { _prev = v; _count++; return Napi::Number::New(info.Env(), 50.0); }
    double change = v - _prev; _prev = v;
    double gain = change > 0 ? change : 0, loss = change < 0 ? -change : 0;
    if (_count <= _period) { _avgGain += gain / _period; _avgLoss += loss / _period; _count++; }
    else { _avgGain = (_avgGain * (_period - 1) + gain) / _period; _avgLoss = (_avgLoss * (_period - 1) + loss) / _period; }
    _val = _avgLoss == 0 ? 100.0 : 100.0 - 100.0 / (1.0 + _avgGain / _avgLoss);
    return Napi::Number::New(info.Env(), _val);
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _val); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _count > _period); }
  size_t _period, _count = 0;
  double _prev = 0, _avgGain = 0, _avgLoss = 0, _val = 50;
};

// ATR
class ATRWrap : public Napi::ObjectWrap<ATRWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "ATR",
                       {InstanceMethod("update", &ATRWrap::Update),
                        InstanceAccessor("value", &ATRWrap::Value, nullptr),
                        InstanceAccessor("ready", &ATRWrap::Ready, nullptr)});
  }
  ATRWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<ATRWrap>(info),
    _period(info[0].As<Napi::Number>().Uint32Value()) {}
 private:
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double h = info[0].As<Napi::Number>().DoubleValue();
    double l = info[1].As<Napi::Number>().DoubleValue();
    double c = info[2].As<Napi::Number>().DoubleValue();
    double tr = _count == 0 ? h - l : std::max({h - l, std::abs(h - _prevC), std::abs(l - _prevC)});
    _prevC = c; _count++;
    if (_count <= _period) { _sum += tr; _val = _sum / _count; }
    else { _val = (_val * (_period - 1) + tr) / _period; }
    return Napi::Number::New(info.Env(), _val);
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _val); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _count >= _period); }
  size_t _period, _count = 0;
  double _prevC = 0, _sum = 0, _val = 0;
};

// MACD
class MACDWrap : public Napi::ObjectWrap<MACDWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "MACD",
                       {InstanceMethod("update", &MACDWrap::Update),
                        InstanceAccessor("line", &MACDWrap::Line, nullptr),
                        InstanceAccessor("signal", &MACDWrap::Signal, nullptr),
                        InstanceAccessor("histogram", &MACDWrap::Histogram, nullptr),
                        InstanceAccessor("value", &MACDWrap::Line, nullptr),
                        InstanceAccessor("ready", &MACDWrap::Ready, nullptr)});
  }
  MACDWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<MACDWrap>(info),
    _fastEma(info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 12),
    _slowEma(info.Length() > 1 ? info[1].As<Napi::Number>().Uint32Value() : 26),
    _sigEma(info.Length() > 2 ? info[2].As<Napi::Number>().Uint32Value() : 9) {}
 private:
  struct Ema { size_t p, c = 0; double m, s = 0, v = 0;
    Ema(size_t period) : p(period), m(2.0 / (period + 1)) {}
    double update(double x) { if (c < p) { s += x; c++; v = s / c; } else { v = (x - v) * m + v; } return v; }
    bool ready() const { return c >= p; }
  };
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double v = info[0].As<Napi::Number>().DoubleValue();
    double f = _fastEma.update(v), s = _slowEma.update(v);
    _line = f - s;
    if (_slowEma.ready()) { _sig = _sigEma.update(_line); _rdy = _sigEma.ready(); }
    _hist = _line - _sig;
    return Napi::Number::New(info.Env(), _line);
  }
  Napi::Value Line(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _line); }
  Napi::Value Signal(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _sig); }
  Napi::Value Histogram(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _hist); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _rdy); }
  Ema _fastEma, _slowEma, _sigEma;
  double _line = 0, _sig = 0, _hist = 0;
  bool _rdy = false;
};

// Bollinger
class BollingerWrap : public Napi::ObjectWrap<BollingerWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "Bollinger",
                       {InstanceMethod("update", &BollingerWrap::Update),
                        InstanceAccessor("upper", &BollingerWrap::Upper, nullptr),
                        InstanceAccessor("middle", &BollingerWrap::Middle, nullptr),
                        InstanceAccessor("lower", &BollingerWrap::Lower, nullptr),
                        InstanceAccessor("value", &BollingerWrap::Middle, nullptr),
                        InstanceAccessor("ready", &BollingerWrap::Ready, nullptr)});
  }
  BollingerWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<BollingerWrap>(info),
    _sma(info[0].As<Napi::Number>().Uint32Value()),
    _period(info[0].As<Napi::Number>().Uint32Value()),
    _mult(info.Length() > 1 ? info[1].As<Napi::Number>().DoubleValue() : 2.0) {}
 private:
  struct Sma { size_t p, c = 0, idx = 0; std::vector<double> buf; double sum = 0, val = 0;
    Sma(size_t period) : p(period), buf(period, 0) {}
    double update(double v) { if (c < p) { buf[c] = v; sum += v; c++; } else { sum -= buf[idx]; buf[idx] = v; sum += v; idx = (idx + 1) % p; } val = sum / c; return val; }
    bool ready() const { return c >= p; }
  };
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double v = info[0].As<Napi::Number>().DoubleValue();
    _mid = _sma.update(v);
    _buf.push_back(v); if (_buf.size() > _period) _buf.erase(_buf.begin());
    if (_sma.ready()) {
      double ss = 0; for (double x : _buf) { double d = x - _mid; ss += d * d; }
      double sd = std::sqrt(ss / _buf.size());
      _upper = _mid + _mult * sd; _lower = _mid - _mult * sd;
    }
    return Napi::Number::New(info.Env(), _mid);
  }
  Napi::Value Upper(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _upper); }
  Napi::Value Middle(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _mid); }
  Napi::Value Lower(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _lower); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _sma.ready()); }
  Sma _sma; size_t _period; double _mult, _upper = 0, _mid = 0, _lower = 0;
  std::vector<double> _buf;
};

// ── Registration ────────────────────────────────────────────────────

inline void registerIndicators(Napi::Env env, Napi::Object exports)
{
  // Batch
  exports.Set("sma", Napi::Function::New(env, batch_sma));
  exports.Set("ema", Napi::Function::New(env, batch_ema));
  exports.Set("rsi", Napi::Function::New(env, batch_rsi));
  exports.Set("rma", Napi::Function::New(env, batch_rma));
  exports.Set("dema", Napi::Function::New(env, batch_dema));
  exports.Set("tema", Napi::Function::New(env, batch_tema));
  exports.Set("kama", Napi::Function::New(env, batch_kama));
  exports.Set("slope", Napi::Function::New(env, batch_slope));
  exports.Set("atr", Napi::Function::New(env, batch_atr));
  exports.Set("adx", Napi::Function::New(env, batch_adx));
  exports.Set("macd", Napi::Function::New(env, batch_macd));
  exports.Set("bollinger", Napi::Function::New(env, batch_bollinger));
  exports.Set("stochastic", Napi::Function::New(env, batch_stochastic));
  exports.Set("cci", Napi::Function::New(env, batch_cci));
  exports.Set("chop", Napi::Function::New(env, batch_chop));
  exports.Set("obv", Napi::Function::New(env, batch_obv));
  exports.Set("vwap", Napi::Function::New(env, batch_vwap));
  exports.Set("cvd", Napi::Function::New(env, batch_cvd));

  // Streaming
  exports.Set("SMA", SMAWrap::Init(env));
  exports.Set("EMA", EMAWrap::Init(env));
  exports.Set("RSI", RSIWrap::Init(env));
  exports.Set("ATR", ATRWrap::Init(env));
  exports.Set("MACD", MACDWrap::Init(env));
  exports.Set("Bollinger", BollingerWrap::Init(env));
}

}  // namespace node_flox
