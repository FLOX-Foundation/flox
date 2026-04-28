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

#include "flox/indicator/adf.h"
#include "flox/indicator/correlation.h"
#include "flox/indicator/kurtosis.h"
#include "flox/indicator/parkinson_vol.h"
#include "flox/indicator/rogers_satchell_vol.h"
#include "flox/indicator/rolling_zscore.h"
#include "flox/indicator/shannon_entropy.h"
#include "flox/indicator/skewness.h"

#include <algorithm>
#include <cmath>
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

#define BATCH_SINGLE(name, Ind)                                   \
  inline Napi::Value batch_##name(const Napi::CallbackInfo& info) \
  {                                                               \
    size_t p = info[1].As<Napi::Number>().Uint32Value();          \
    return batchSingle(info, flox::indicator::Ind(p));            \
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

// ── Batch: statistical / volatility ─────────────────────────────────

inline Napi::Value batch_skewness(const Napi::CallbackInfo& info)
{
  auto in = info[0].As<Napi::Float64Array>();
  size_t p = info[1].As<Napi::Number>().Uint32Value();
  auto r = flox::indicator::Skewness(p).compute(arr2span(in));
  return vec2arr(info.Env(), r);
}

inline Napi::Value batch_kurtosis(const Napi::CallbackInfo& info)
{
  auto in = info[0].As<Napi::Float64Array>();
  size_t p = info[1].As<Napi::Number>().Uint32Value();
  auto r = flox::indicator::Kurtosis(p).compute(arr2span(in));
  return vec2arr(info.Env(), r);
}

inline Napi::Value batch_rolling_zscore(const Napi::CallbackInfo& info)
{
  auto in = info[0].As<Napi::Float64Array>();
  size_t p = info[1].As<Napi::Number>().Uint32Value();
  auto r = flox::indicator::RollingZScore(p).compute(arr2span(in));
  return vec2arr(info.Env(), r);
}

inline Napi::Value batch_shannon_entropy(const Napi::CallbackInfo& info)
{
  auto in = info[0].As<Napi::Float64Array>();
  size_t p = info[1].As<Napi::Number>().Uint32Value();
  size_t bins = info[2].As<Napi::Number>().Uint32Value();
  auto r = flox::indicator::ShannonEntropy(p, bins).compute(arr2span(in));
  return vec2arr(info.Env(), r);
}

inline Napi::Value batch_parkinson_vol(const Napi::CallbackInfo& info)
{
  auto h = info[0].As<Napi::Float64Array>();
  auto l = info[1].As<Napi::Float64Array>();
  size_t n = h.ElementLength(), p = info[2].As<Napi::Number>().Uint32Value();
  std::vector<double> out(n);
  flox::indicator::ParkinsonVol(p).compute({h.Data(), n}, {l.Data(), n}, {out.data(), n});
  return vec2arr(info.Env(), out);
}

inline Napi::Value batch_rogers_satchell_vol(const Napi::CallbackInfo& info)
{
  auto o = info[0].As<Napi::Float64Array>();
  auto h = info[1].As<Napi::Float64Array>();
  auto l = info[2].As<Napi::Float64Array>();
  auto c = info[3].As<Napi::Float64Array>();
  size_t n = o.ElementLength(), p = info[4].As<Napi::Number>().Uint32Value();
  std::vector<double> out(n);
  flox::indicator::RogersSatchellVol(p).compute({o.Data(), n}, {h.Data(), n}, {l.Data(), n},
                                                {c.Data(), n}, {out.data(), n});
  return vec2arr(info.Env(), out);
}

inline Napi::Value batch_correlation(const Napi::CallbackInfo& info)
{
  auto x = info[0].As<Napi::Float64Array>();
  auto y = info[1].As<Napi::Float64Array>();
  size_t n = x.ElementLength(), p = info[2].As<Napi::Number>().Uint32Value();
  std::vector<double> out(n);
  flox::indicator::Correlation(p).compute({x.Data(), n}, {y.Data(), n}, {out.data(), n});
  return vec2arr(info.Env(), out);
}

inline Napi::Value batch_adf(const Napi::CallbackInfo& info)
{
  auto in = info[0].As<Napi::Float64Array>();
  size_t n = in.ElementLength();
  size_t maxLag = info[1].As<Napi::Number>().Uint32Value();
  std::string regression = info[2].As<Napi::String>().Utf8Value();
  flox::indicator::AdfResult r =
      flox::indicator::adf(std::span<const double>(in.Data(), n), maxLag, regression);
  auto o = Napi::Object::New(info.Env());
  o.Set("test_stat", Napi::Number::New(info.Env(), r.test_stat));
  o.Set("p_value", Napi::Number::New(info.Env(), r.p_value));
  o.Set("used_lag", Napi::Number::New(info.Env(), static_cast<double>(r.used_lag)));
  return o;
}

// ── Streaming indicator classes ─────────────────────────────────────

#define STREAMING_SINGLE(Name, updExpr)                                                                                                \
  class Name##Wrap : public Napi::ObjectWrap<Name##Wrap>                                                                               \
  {                                                                                                                                    \
   public:                                                                                                                             \
    static Napi::Function Init(Napi::Env env)                                                                                          \
    {                                                                                                                                  \
      return DefineClass(env, #Name,                                                                                                   \
                         {InstanceMethod("update", &Name##Wrap::Update),                                                               \
                          InstanceAccessor("value", &Name##Wrap::Value, nullptr),                                                      \
                          InstanceAccessor("ready", &Name##Wrap::Ready, nullptr)});                                                    \
    }                                                                                                                                  \
    Name##Wrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<Name##Wrap>(info), _ind(info[0].As<Napi::Number>().Uint32Value()) {} \
                                                                                                                                       \
   private:                                                                                                                            \
    Napi::Value Update(const Napi::CallbackInfo& info)                                                                                 \
    {                                                                                                                                  \
      updExpr;                                                                                                                         \
      return Napi::Number::New(info.Env(), _val);                                                                                      \
    }                                                                                                                                  \
    Napi::Value Value(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _val); }                                  \
    Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _ready); }

// SMA
class SMAWrap : public Napi::ObjectWrap<SMAWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "SMA",
                       {InstanceMethod("update", &SMAWrap::Update),
                        InstanceMethod("reset", &SMAWrap::Reset),
                        InstanceAccessor("value", &SMAWrap::Value, nullptr),
                        InstanceAccessor("ready", &SMAWrap::Ready, nullptr)});
  }
  SMAWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<SMAWrap>(info),
                                            _period(info[0].As<Napi::Number>().Uint32Value()),
                                            _buf(_period, 0) {}

 private:
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double v = info[0].As<Napi::Number>().DoubleValue();
    if (_count < _period)
    {
      _buf[_count] = v;
      _sum += v;
      _count++;
    }
    else
    {
      _sum -= _buf[_idx];
      _buf[_idx] = v;
      _sum += v;
      _idx = (_idx + 1) % _period;
    }
    _val = _sum / _count;
    return Napi::Number::New(info.Env(), _val);
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _val); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _count >= _period); }
  void Reset(const Napi::CallbackInfo&)
  {
    _count = 0;
    _idx = 0;
    _sum = 0;
    _val = 0;
    std::fill(_buf.begin(), _buf.end(), 0);
  }
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
                        InstanceMethod("reset", &EMAWrap::Reset),
                        InstanceAccessor("value", &EMAWrap::Value, nullptr),
                        InstanceAccessor("ready", &EMAWrap::Ready, nullptr)});
  }
  EMAWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<EMAWrap>(info),
                                            _period(info[0].As<Napi::Number>().Uint32Value()),
                                            _mult(2.0 / (_period + 1)) {}

 private:
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double v = info[0].As<Napi::Number>().DoubleValue();
    if (_count < _period)
    {
      _sum += v;
      _count++;
      _val = _sum / _count;
    }
    else
    {
      _val = (v - _val) * _mult + _val;
    }
    return Napi::Number::New(info.Env(), _val);
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _val); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _count >= _period); }
  void Reset(const Napi::CallbackInfo&)
  {
    _count = 0;
    _sum = 0;
    _val = 0;
  }
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
                        InstanceMethod("reset", &RSIWrap::Reset),
                        InstanceAccessor("value", &RSIWrap::Value, nullptr),
                        InstanceAccessor("ready", &RSIWrap::Ready, nullptr)});
  }
  RSIWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<RSIWrap>(info),
                                            _period(info[0].As<Napi::Number>().Uint32Value()) {}

 private:
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double v = info[0].As<Napi::Number>().DoubleValue();
    if (_count == 0)
    {
      _prev = v;
      _count++;
      return Napi::Number::New(info.Env(), 50.0);
    }
    double change = v - _prev;
    _prev = v;
    double gain = change > 0 ? change : 0, loss = change < 0 ? -change : 0;
    if (_count <= _period)
    {
      _avgGain += gain / _period;
      _avgLoss += loss / _period;
      _count++;
    }
    else
    {
      _avgGain = (_avgGain * (_period - 1) + gain) / _period;
      _avgLoss = (_avgLoss * (_period - 1) + loss) / _period;
    }
    _val = _avgLoss == 0 ? 100.0 : 100.0 - 100.0 / (1.0 + _avgGain / _avgLoss);
    return Napi::Number::New(info.Env(), _val);
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _val); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _count > _period); }
  void Reset(const Napi::CallbackInfo&)
  {
    _count = 0;
    _prev = 0;
    _avgGain = 0;
    _avgLoss = 0;
    _val = 50;
  }
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
                        InstanceMethod("reset", &ATRWrap::Reset),
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
    _prevC = c;
    _count++;
    if (_count <= _period)
    {
      _sum += tr;
      _val = _sum / _count;
    }
    else
    {
      _val = (_val * (_period - 1) + tr) / _period;
    }
    return Napi::Number::New(info.Env(), _val);
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _val); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _count >= _period); }
  void Reset(const Napi::CallbackInfo&)
  {
    _count = 0;
    _prevC = 0;
    _sum = 0;
    _val = 0;
  }
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
                        InstanceMethod("reset", &MACDWrap::Reset),
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
  struct Ema
  {
    size_t p, c = 0;
    double m, s = 0, v = 0;
    Ema(size_t period) : p(period), m(2.0 / (period + 1)) {}
    double update(double x)
    {
      if (c < p)
      {
        s += x;
        c++;
        v = s / c;
      }
      else
      {
        v = (x - v) * m + v;
      }
      return v;
    }
    bool ready() const { return c >= p; }
  };
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double v = info[0].As<Napi::Number>().DoubleValue();
    double f = _fastEma.update(v), s = _slowEma.update(v);
    _line = f - s;
    if (_slowEma.ready())
    {
      _sig = _sigEma.update(_line);
      _rdy = _sigEma.ready();
    }
    _hist = _line - _sig;
    return Napi::Number::New(info.Env(), _line);
  }
  Napi::Value Line(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _line); }
  Napi::Value Signal(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _sig); }
  Napi::Value Histogram(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _hist); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _rdy); }
  void Reset(const Napi::CallbackInfo&)
  {
    _fastEma = Ema(_fastEma.p);
    _slowEma = Ema(_slowEma.p);
    _sigEma = Ema(_sigEma.p);
    _line = 0;
    _sig = 0;
    _hist = 0;
    _rdy = false;
  }
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
                        InstanceMethod("reset", &BollingerWrap::Reset),
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
  struct Sma
  {
    size_t p, c = 0, idx = 0;
    std::vector<double> buf;
    double sum = 0, val = 0;
    Sma(size_t period) : p(period), buf(period, 0) {}
    double update(double v)
    {
      if (c < p)
      {
        buf[c] = v;
        sum += v;
        c++;
      }
      else
      {
        sum -= buf[idx];
        buf[idx] = v;
        sum += v;
        idx = (idx + 1) % p;
      }
      val = sum / c;
      return val;
    }
    bool ready() const { return c >= p; }
  };
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double v = info[0].As<Napi::Number>().DoubleValue();
    _mid = _sma.update(v);
    _buf.push_back(v);
    if (_buf.size() > _period)
    {
      _buf.erase(_buf.begin());
    }
    if (_sma.ready())
    {
      double ss = 0;
      for (double x : _buf)
      {
        double d = x - _mid;
        ss += d * d;
      }
      double sd = std::sqrt(ss / _buf.size());
      _upper = _mid + _mult * sd;
      _lower = _mid - _mult * sd;
    }
    return Napi::Number::New(info.Env(), _mid);
  }
  Napi::Value Upper(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _upper); }
  Napi::Value Middle(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _mid); }
  Napi::Value Lower(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _lower); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _sma.ready()); }
  void Reset(const Napi::CallbackInfo&)
  {
    _sma = Sma(_period);
    _buf.clear();
    _upper = 0;
    _mid = 0;
    _lower = 0;
  }
  Sma _sma;
  size_t _period;
  double _mult, _upper = 0, _mid = 0, _lower = 0;
  std::vector<double> _buf;
};

// RMA
class RMAWrap : public Napi::ObjectWrap<RMAWrap>
{
 public:
  static Napi::Function Init(Napi::Env env) { return DefineClass(env, "RMA", {InstanceMethod("update", &RMAWrap::Update), InstanceMethod("reset", &RMAWrap::Reset), InstanceAccessor("value", &RMAWrap::Value, nullptr), InstanceAccessor("ready", &RMAWrap::Ready, nullptr)}); }
  RMAWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<RMAWrap>(info), _p(info[0].As<Napi::Number>().Uint32Value()), _a(1.0 / _p) {}

 private:
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double v = info[0].As<Napi::Number>().DoubleValue();
    if (_c < _p)
    {
      _s += v;
      _c++;
      _v = _s / _c;
    }
    else
    {
      _v = _a * v + (1 - _a) * _v;
    }
    return Napi::Number::New(info.Env(), _v);
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _v); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _c >= _p); }
  void Reset(const Napi::CallbackInfo&)
  {
    _c = 0;
    _s = 0;
    _v = 0;
  }
  size_t _p, _c = 0;
  double _a, _s = 0, _v = 0;
};

// DEMA streaming
class DEMAWrap : public Napi::ObjectWrap<DEMAWrap>
{
 public:
  static Napi::Function Init(Napi::Env env) { return DefineClass(env, "DEMA", {InstanceMethod("update", &DEMAWrap::Update), InstanceMethod("reset", &DEMAWrap::Reset), InstanceAccessor("value", &DEMAWrap::Value, nullptr), InstanceAccessor("ready", &DEMAWrap::Ready, nullptr)}); }
  DEMAWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<DEMAWrap>(info), _e1(info[0].As<Napi::Number>().Uint32Value()), _e2(info[0].As<Napi::Number>().Uint32Value()) {}

 private:
  struct E
  {
    size_t p, c = 0;
    double m, s = 0, v = 0;
    E(size_t pp) : p(pp), m(2.0 / (pp + 1)) {}
    double up(double x)
    {
      if (c < p)
      {
        s += x;
        c++;
        v = s / c;
      }
      else
      {
        v = (x - v) * m + v;
      }
      return v;
    }
    bool rdy() const { return c >= p; }
  };
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double v = info[0].As<Napi::Number>().DoubleValue();
    double e1 = _e1.up(v);
    double e2 = _e2.up(e1);
    _v = 2 * e1 - e2;
    return Napi::Number::New(info.Env(), _v);
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _v); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _e2.rdy()); }
  void Reset(const Napi::CallbackInfo&)
  {
    _e1 = E(_e1.p);
    _e2 = E(_e2.p);
    _v = 0;
  }
  E _e1, _e2;
  double _v = 0;
};

// TEMA streaming
class TEMAWrap : public Napi::ObjectWrap<TEMAWrap>
{
 public:
  static Napi::Function Init(Napi::Env env) { return DefineClass(env, "TEMA", {InstanceMethod("update", &TEMAWrap::Update), InstanceMethod("reset", &TEMAWrap::Reset), InstanceAccessor("value", &TEMAWrap::Value, nullptr), InstanceAccessor("ready", &TEMAWrap::Ready, nullptr)}); }
  TEMAWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<TEMAWrap>(info), _e1(info[0].As<Napi::Number>().Uint32Value()), _e2(info[0].As<Napi::Number>().Uint32Value()), _e3(info[0].As<Napi::Number>().Uint32Value()) {}

 private:
  struct E
  {
    size_t p, c = 0;
    double m, s = 0, v = 0;
    E(size_t pp) : p(pp), m(2.0 / (pp + 1)) {}
    double up(double x)
    {
      if (c < p)
      {
        s += x;
        c++;
        v = s / c;
      }
      else
      {
        v = (x - v) * m + v;
      }
      return v;
    }
    bool rdy() const { return c >= p; }
  };
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double v = info[0].As<Napi::Number>().DoubleValue();
    double e1 = _e1.up(v), e2 = _e2.up(e1), e3 = _e3.up(e2);
    _v = 3 * e1 - 3 * e2 + e3;
    return Napi::Number::New(info.Env(), _v);
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _v); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _e3.rdy()); }
  void Reset(const Napi::CallbackInfo&)
  {
    _e1 = E(_e1.p);
    _e2 = E(_e2.p);
    _e3 = E(_e3.p);
    _v = 0;
  }
  E _e1, _e2, _e3;
  double _v = 0;
};

// KAMA streaming
class KAMAWrap : public Napi::ObjectWrap<KAMAWrap>
{
 public:
  static Napi::Function Init(Napi::Env env) { return DefineClass(env, "KAMA", {InstanceMethod("update", &KAMAWrap::Update), InstanceMethod("reset", &KAMAWrap::Reset), InstanceAccessor("value", &KAMAWrap::Value, nullptr), InstanceAccessor("ready", &KAMAWrap::Ready, nullptr)}); }
  KAMAWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<KAMAWrap>(info), _p(info[0].As<Napi::Number>().Uint32Value()), _fsc(2.0 / ((info.Length() > 1 ? info[1].As<Napi::Number>().Uint32Value() : 2) + 1)), _ssc(2.0 / ((info.Length() > 2 ? info[2].As<Napi::Number>().Uint32Value() : 30) + 1)) {}

 private:
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double v = info[0].As<Napi::Number>().DoubleValue();
    _buf.push_back(v);
    _c++;
    if (_c <= _p)
    {
      _v = v;
      return Napi::Number::New(info.Env(), _v);
    }
    if (_buf.size() > _p + 1)
    {
      _buf.erase(_buf.begin());
    }
    double dir = std::abs(v - _buf.front()), vol = 0;
    for (size_t i = 1; i < _buf.size(); i++)
    {
      vol += std::abs(_buf[i] - _buf[i - 1]);
    }
    double er = vol != 0 ? dir / vol : 0, sc = er * (_fsc - _ssc) + _ssc;
    sc *= sc;
    _v += sc * (v - _v);
    return Napi::Number::New(info.Env(), _v);
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _v); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _c > _p); }
  void Reset(const Napi::CallbackInfo&)
  {
    _c = 0;
    _v = 0;
    _buf.clear();
  }
  size_t _p, _c = 0;
  double _fsc, _ssc, _v = 0;
  std::vector<double> _buf;
};

// Slope streaming
class SlopeWrap : public Napi::ObjectWrap<SlopeWrap>
{
 public:
  static Napi::Function Init(Napi::Env env) { return DefineClass(env, "Slope", {InstanceMethod("update", &SlopeWrap::Update), InstanceMethod("reset", &SlopeWrap::Reset), InstanceAccessor("value", &SlopeWrap::Value, nullptr), InstanceAccessor("ready", &SlopeWrap::Ready, nullptr)}); }
  SlopeWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<SlopeWrap>(info), _len(info[0].As<Napi::Number>().Uint32Value()) {}

 private:
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double v = info[0].As<Napi::Number>().DoubleValue();
    _buf.push_back(v);
    if (_buf.size() > _len + 1)
    {
      _buf.erase(_buf.begin());
    }
    if (_buf.size() > _len)
    {
      _v = (v - _buf.front()) / _len;
    }
    return Napi::Number::New(info.Env(), _v);
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _v); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _buf.size() > _len); }
  void Reset(const Napi::CallbackInfo&)
  {
    _v = 0;
    _buf.clear();
  }
  size_t _len;
  double _v = 0;
  std::vector<double> _buf;
};

// Stochastic streaming
class StochasticWrap : public Napi::ObjectWrap<StochasticWrap>
{
 public:
  static Napi::Function Init(Napi::Env env) { return DefineClass(env, "Stochastic", {InstanceMethod("update", &StochasticWrap::Update), InstanceMethod("reset", &StochasticWrap::Reset), InstanceAccessor("k", &StochasticWrap::K, nullptr), InstanceAccessor("d", &StochasticWrap::D, nullptr), InstanceAccessor("value", &StochasticWrap::K, nullptr), InstanceAccessor("ready", &StochasticWrap::Ready, nullptr)}); }
  StochasticWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<StochasticWrap>(info), _kp(info[0].As<Napi::Number>().Uint32Value()), _dsma(info.Length() > 1 ? info[1].As<Napi::Number>().Uint32Value() : 3) {}

 private:
  struct Sma
  {
    size_t p, c = 0, idx = 0;
    std::vector<double> buf;
    double sum = 0, val = 0;
    Sma(size_t pp) : p(pp), buf(pp, 0) {}
    double up(double v)
    {
      if (c < p)
      {
        buf[c] = v;
        sum += v;
        c++;
      }
      else
      {
        sum -= buf[idx];
        buf[idx] = v;
        sum += v;
        idx = (idx + 1) % p;
      }
      val = sum / c;
      return val;
    }
    bool rdy() const { return c >= p; }
  };
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double h = info[0].As<Napi::Number>().DoubleValue(), l = info[1].As<Napi::Number>().DoubleValue(), c = info[2].As<Napi::Number>().DoubleValue();
    _h.push_back(h);
    _l.push_back(l);
    if (_h.size() > _kp)
    {
      _h.erase(_h.begin());
      _l.erase(_l.begin());
    }
    if (_h.size() >= _kp)
    {
      double hh = *std::max_element(_h.begin(), _h.end()), ll = *std::min_element(_l.begin(), _l.end());
      _k = (hh != ll) ? 100.0 * (c - ll) / (hh - ll) : 0;
      _d = _dsma.up(_k);
    }
    return Napi::Number::New(info.Env(), _k);
  }
  Napi::Value K(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _k); }
  Napi::Value D(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _d); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _h.size() >= _kp && _dsma.rdy()); }
  void Reset(const Napi::CallbackInfo&)
  {
    _h.clear();
    _l.clear();
    _dsma = Sma(_dsma.p);
    _k = 0;
    _d = 0;
  }
  size_t _kp;
  Sma _dsma;
  std::vector<double> _h, _l;
  double _k = 0, _d = 0;
};

// CCI streaming
class CCIWrap : public Napi::ObjectWrap<CCIWrap>
{
 public:
  static Napi::Function Init(Napi::Env env) { return DefineClass(env, "CCI", {InstanceMethod("update", &CCIWrap::Update), InstanceMethod("reset", &CCIWrap::Reset), InstanceAccessor("value", &CCIWrap::Value, nullptr), InstanceAccessor("ready", &CCIWrap::Ready, nullptr)}); }
  CCIWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<CCIWrap>(info), _p(info[0].As<Napi::Number>().Uint32Value()) {}

 private:
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double h = info[0].As<Napi::Number>().DoubleValue(), l = info[1].As<Napi::Number>().DoubleValue(), c = info[2].As<Napi::Number>().DoubleValue();
    double tp = (h + l + c) / 3.0;
    _buf.push_back(tp);
    if (_buf.size() > _p)
    {
      _buf.erase(_buf.begin());
    }
    if (_buf.size() >= _p)
    {
      double mean = 0;
      for (double v : _buf)
      {
        mean += v;
      }
      mean /= _buf.size();
      double dev = 0;
      for (double v : _buf)
      {
        dev += std::abs(v - mean);
      }
      dev /= _buf.size();
      _v = dev != 0 ? (tp - mean) / (0.015 * dev) : 0;
    }
    return Napi::Number::New(info.Env(), _v);
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _v); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _buf.size() >= _p); }
  void Reset(const Napi::CallbackInfo&)
  {
    _v = 0;
    _buf.clear();
  }
  size_t _p;
  double _v = 0;
  std::vector<double> _buf;
};

// OBV streaming
class OBVWrap : public Napi::ObjectWrap<OBVWrap>
{
 public:
  static Napi::Function Init(Napi::Env env) { return DefineClass(env, "OBV", {InstanceMethod("update", &OBVWrap::Update), InstanceMethod("reset", &OBVWrap::Reset), InstanceAccessor("value", &OBVWrap::Value, nullptr), InstanceAccessor("ready", &OBVWrap::Ready, nullptr)}); }
  OBVWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<OBVWrap>(info) {}

 private:
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double c = info[0].As<Napi::Number>().DoubleValue(), v = info[1].As<Napi::Number>().DoubleValue();
    if (_n == 0)
    {
      _v = v;
    }
    else if (c > _pc)
    {
      _v += v;
    }
    else if (c < _pc)
    {
      _v -= v;
    }
    _pc = c;
    _n++;
    return Napi::Number::New(info.Env(), _v);
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _v); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _n > 0); }
  void Reset(const Napi::CallbackInfo&)
  {
    _n = 0;
    _pc = 0;
    _v = 0;
  }
  size_t _n = 0;
  double _pc = 0, _v = 0;
};

// VWAP streaming
class VWAPWrap : public Napi::ObjectWrap<VWAPWrap>
{
 public:
  static Napi::Function Init(Napi::Env env) { return DefineClass(env, "VWAP", {InstanceMethod("update", &VWAPWrap::Update), InstanceMethod("reset", &VWAPWrap::Reset), InstanceAccessor("value", &VWAPWrap::Value, nullptr), InstanceAccessor("ready", &VWAPWrap::Ready, nullptr)}); }
  VWAPWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<VWAPWrap>(info), _w(info[0].As<Napi::Number>().Uint32Value()) {}

 private:
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double c = info[0].As<Napi::Number>().DoubleValue(), vol = info[1].As<Napi::Number>().DoubleValue();
    _p.push_back(c);
    _vol.push_back(vol);
    if (_p.size() > _w)
    {
      _p.erase(_p.begin());
      _vol.erase(_vol.begin());
    }
    double pv = 0, vs = 0;
    for (size_t i = 0; i < _p.size(); i++)
    {
      pv += _p[i] * _vol[i];
      vs += _vol[i];
    }
    _v = vs > 0 ? pv / vs : 0;
    return Napi::Number::New(info.Env(), _v);
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _v); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _p.size() >= _w); }
  void Reset(const Napi::CallbackInfo&)
  {
    _v = 0;
    _p.clear();
    _vol.clear();
  }
  size_t _w;
  double _v = 0;
  std::vector<double> _p, _vol;
};

// CVD streaming
class CVDWrap : public Napi::ObjectWrap<CVDWrap>
{
 public:
  static Napi::Function Init(Napi::Env env) { return DefineClass(env, "CVD", {InstanceMethod("update", &CVDWrap::Update), InstanceMethod("reset", &CVDWrap::Reset), InstanceAccessor("value", &CVDWrap::Value, nullptr), InstanceAccessor("ready", &CVDWrap::Ready, nullptr)}); }
  CVDWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<CVDWrap>(info) {}

 private:
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double o = info[0].As<Napi::Number>().DoubleValue(), h = info[1].As<Napi::Number>().DoubleValue(), l = info[2].As<Napi::Number>().DoubleValue(), c = info[3].As<Napi::Number>().DoubleValue(), v = info[4].As<Napi::Number>().DoubleValue();
    double r = h - l;
    _v += r > 0 ? v * (c - o) / r : 0;
    _n++;
    return Napi::Number::New(info.Env(), _v);
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _v); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _n > 0); }
  void Reset(const Napi::CallbackInfo&)
  {
    _n = 0;
    _v = 0;
  }
  size_t _n = 0;
  double _v = 0;
};

// Skewness streaming
class SkewnessWrap : public Napi::ObjectWrap<SkewnessWrap>
{
 public:
  static Napi::Function Init(Napi::Env env) { return DefineClass(env, "Skewness", {InstanceMethod("update", &SkewnessWrap::Update), InstanceMethod("reset", &SkewnessWrap::Reset), InstanceAccessor("value", &SkewnessWrap::Value, nullptr), InstanceAccessor("ready", &SkewnessWrap::Ready, nullptr)}); }
  SkewnessWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<SkewnessWrap>(info), _p(info[0].As<Napi::Number>().Uint32Value()) {}

 private:
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double v = info[0].As<Napi::Number>().DoubleValue();
    _buf.push_back(v);
    if (_buf.size() > _p)
    {
      _buf.erase(_buf.begin());
    }
    if (_buf.size() >= _p)
    {
      double p = static_cast<double>(_p);
      double mean = 0;
      for (double x : _buf)
      {
        mean += x;
      }
      mean /= p;
      double m2 = 0, m3 = 0;
      for (double x : _buf)
      {
        double d = x - mean;
        double d2 = d * d;
        m2 += d2;
        m3 += d2 * d;
      }
      double var = m2 / (p - 1.0);
      if (var == 0.0)
      {
        return info.Env().Undefined();
      }
      double s = std::sqrt(var);
      _v = (p / ((p - 1.0) * (p - 2.0))) * (m3 / (s * s * s));
    }
    return Napi::Number::New(info.Env(), _v);
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _v); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _buf.size() >= _p); }
  void Reset(const Napi::CallbackInfo&)
  {
    _v = 0;
    _buf.clear();
  }
  size_t _p;
  double _v = 0;
  std::vector<double> _buf;
};

// Kurtosis streaming
class KurtosisWrap : public Napi::ObjectWrap<KurtosisWrap>
{
 public:
  static Napi::Function Init(Napi::Env env) { return DefineClass(env, "Kurtosis", {InstanceMethod("update", &KurtosisWrap::Update), InstanceMethod("reset", &KurtosisWrap::Reset), InstanceAccessor("value", &KurtosisWrap::Value, nullptr), InstanceAccessor("ready", &KurtosisWrap::Ready, nullptr)}); }
  KurtosisWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<KurtosisWrap>(info), _p(info[0].As<Napi::Number>().Uint32Value()) {}

 private:
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double v = info[0].As<Napi::Number>().DoubleValue();
    _buf.push_back(v);
    if (_buf.size() > _p)
    {
      _buf.erase(_buf.begin());
    }
    if (_buf.size() >= _p)
    {
      double p = static_cast<double>(_p);
      double mean = 0;
      for (double x : _buf)
      {
        mean += x;
      }
      mean /= p;
      double m2 = 0, m4 = 0;
      for (double x : _buf)
      {
        double d = x - mean;
        double d2 = d * d;
        m2 += d2;
        m4 += d2 * d2;
      }
      double var = m2 / (p - 1.0);
      if (var == 0.0)
      {
        return info.Env().Undefined();
      }
      double s2 = var;
      double t1 = (p * (p + 1.0)) / ((p - 1.0) * (p - 2.0) * (p - 3.0));
      double t2 = m4 / (s2 * s2);
      double t3 = (3.0 * (p - 1.0) * (p - 1.0)) / ((p - 2.0) * (p - 3.0));
      _v = t1 * t2 - t3;
    }
    return Napi::Number::New(info.Env(), _v);
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _v); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _buf.size() >= _p); }
  void Reset(const Napi::CallbackInfo&)
  {
    _v = 0;
    _buf.clear();
  }
  size_t _p;
  double _v = 0;
  std::vector<double> _buf;
};

// RollingZScore streaming
class RollingZScoreWrap : public Napi::ObjectWrap<RollingZScoreWrap>
{
 public:
  static Napi::Function Init(Napi::Env env) { return DefineClass(env, "RollingZScore", {InstanceMethod("update", &RollingZScoreWrap::Update), InstanceMethod("reset", &RollingZScoreWrap::Reset), InstanceAccessor("value", &RollingZScoreWrap::Value, nullptr), InstanceAccessor("ready", &RollingZScoreWrap::Ready, nullptr)}); }
  RollingZScoreWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<RollingZScoreWrap>(info), _p(info[0].As<Napi::Number>().Uint32Value()) {}

 private:
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double v = info[0].As<Napi::Number>().DoubleValue();
    _buf.push_back(v);
    if (_buf.size() > _p)
    {
      _buf.erase(_buf.begin());
    }
    if (_buf.size() >= _p)
    {
      double p = static_cast<double>(_p);
      double mean = 0;
      for (double x : _buf)
      {
        mean += x;
      }
      mean /= p;
      double sumSq = 0;
      for (double x : _buf)
      {
        double d = x - mean;
        sumSq += d * d;
      }
      double s = std::sqrt(sumSq / (p - 1.0));
      if (s == 0.0)
      {
        return info.Env().Undefined();
      }
      _v = (v - mean) / s;
    }
    return Napi::Number::New(info.Env(), _v);
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _v); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _buf.size() >= _p); }
  void Reset(const Napi::CallbackInfo&)
  {
    _v = 0;
    _buf.clear();
  }
  size_t _p;
  double _v = 0;
  std::vector<double> _buf;
};

// ShannonEntropy streaming
class ShannonEntropyWrap : public Napi::ObjectWrap<ShannonEntropyWrap>
{
 public:
  static Napi::Function Init(Napi::Env env) { return DefineClass(env, "ShannonEntropy", {InstanceMethod("update", &ShannonEntropyWrap::Update), InstanceMethod("reset", &ShannonEntropyWrap::Reset), InstanceAccessor("value", &ShannonEntropyWrap::Value, nullptr), InstanceAccessor("ready", &ShannonEntropyWrap::Ready, nullptr)}); }
  ShannonEntropyWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<ShannonEntropyWrap>(info), _p(info[0].As<Napi::Number>().Uint32Value()), _bins(info.Length() > 1 ? info[1].As<Napi::Number>().Uint32Value() : 10) {}

 private:
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double v = info[0].As<Napi::Number>().DoubleValue();
    _buf.push_back(v);
    if (_buf.size() > _p)
    {
      _buf.erase(_buf.begin());
    }
    if (_buf.size() >= _p)
    {
      double lo = *std::min_element(_buf.begin(), _buf.end());
      double hi = *std::max_element(_buf.begin(), _buf.end());
      if (lo == hi)
      {
        _v = 0.0;
        return Napi::Number::New(info.Env(), _v);
      }
      std::vector<size_t> counts(_bins, 0);
      double range = hi - lo;
      for (double x : _buf)
      {
        size_t bin = static_cast<size_t>(((x - lo) / range) * _bins);
        if (bin >= _bins)
        {
          bin = _bins - 1;
        }
        counts[bin]++;
      }
      double ent = 0;
      double denom = static_cast<double>(_p);
      for (size_t b = 0; b < _bins; ++b)
      {
        if (counts[b] > 0)
        {
          double p = static_cast<double>(counts[b]) / denom;
          ent -= p * std::log(p);
        }
      }
      _v = ent / std::log(static_cast<double>(_bins));
    }
    return Napi::Number::New(info.Env(), _v);
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _v); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _buf.size() >= _p); }
  void Reset(const Napi::CallbackInfo&)
  {
    _v = 0;
    _buf.clear();
  }
  size_t _p, _bins;
  double _v = 0;
  std::vector<double> _buf;
};

// ParkinsonVol streaming
class ParkinsonVolWrap : public Napi::ObjectWrap<ParkinsonVolWrap>
{
 public:
  static Napi::Function Init(Napi::Env env) { return DefineClass(env, "ParkinsonVol", {InstanceMethod("update", &ParkinsonVolWrap::Update), InstanceMethod("reset", &ParkinsonVolWrap::Reset), InstanceAccessor("value", &ParkinsonVolWrap::Value, nullptr), InstanceAccessor("ready", &ParkinsonVolWrap::Ready, nullptr)}); }
  ParkinsonVolWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<ParkinsonVolWrap>(info), _p(info[0].As<Napi::Number>().Uint32Value()) {}

 private:
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double h = info[0].As<Napi::Number>().DoubleValue(), l = info[1].As<Napi::Number>().DoubleValue();
    if (l <= 0.0 || h < l)
    {
      _hlsq.push_back(std::nan(""));
    }
    else
    {
      double lnhl = std::log(h / l);
      _hlsq.push_back(lnhl * lnhl);
    }
    if (_hlsq.size() > _p)
    {
      _hlsq.erase(_hlsq.begin());
    }
    if (_hlsq.size() >= _p)
    {
      double sum = 0;
      bool valid = true;
      for (double x : _hlsq)
      {
        if (std::isnan(x))
        {
          valid = false;
          break;
        }
        sum += x;
      }
      if (valid)
      {
        _v = std::sqrt(sum / static_cast<double>(_p) / (4.0 * std::log(2.0)));
      }
    }
    return Napi::Number::New(info.Env(), _v);
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _v); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _hlsq.size() >= _p); }
  void Reset(const Napi::CallbackInfo&)
  {
    _v = 0;
    _hlsq.clear();
  }
  size_t _p;
  double _v = 0;
  std::vector<double> _hlsq;
};

// RogersSatchellVol streaming
class RogersSatchellVolWrap : public Napi::ObjectWrap<RogersSatchellVolWrap>
{
 public:
  static Napi::Function Init(Napi::Env env) { return DefineClass(env, "RogersSatchellVol", {InstanceMethod("update", &RogersSatchellVolWrap::Update), InstanceMethod("reset", &RogersSatchellVolWrap::Reset), InstanceAccessor("value", &RogersSatchellVolWrap::Value, nullptr), InstanceAccessor("ready", &RogersSatchellVolWrap::Ready, nullptr)}); }
  RogersSatchellVolWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<RogersSatchellVolWrap>(info), _p(info[0].As<Napi::Number>().Uint32Value()) {}

 private:
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double o = info[0].As<Napi::Number>().DoubleValue(), h = info[1].As<Napi::Number>().DoubleValue(), l = info[2].As<Napi::Number>().DoubleValue(), c = info[3].As<Napi::Number>().DoubleValue();
    if (o <= 0.0 || c <= 0.0)
    {
      _rsq.push_back(std::nan(""));
    }
    else
    {
      double v = std::log(h / c) * std::log(h / o) + std::log(l / c) * std::log(l / o);
      _rsq.push_back(v);
    }
    if (_rsq.size() > _p)
    {
      _rsq.erase(_rsq.begin());
    }
    if (_rsq.size() >= _p)
    {
      double sum = 0;
      bool valid = true;
      for (double x : _rsq)
      {
        if (std::isnan(x))
        {
          valid = false;
          break;
        }
        sum += x;
      }
      if (valid)
      {
        double avg = sum / static_cast<double>(_p);
        _v = avg >= 0.0 ? std::sqrt(avg) : 0.0;
      }
    }
    return Napi::Number::New(info.Env(), _v);
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _v); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _rsq.size() >= _p); }
  void Reset(const Napi::CallbackInfo&)
  {
    _v = 0;
    _rsq.clear();
  }
  size_t _p;
  double _v = 0;
  std::vector<double> _rsq;
};

// Correlation streaming
class CorrelationWrap : public Napi::ObjectWrap<CorrelationWrap>
{
 public:
  static Napi::Function Init(Napi::Env env) { return DefineClass(env, "Correlation", {InstanceMethod("update", &CorrelationWrap::Update), InstanceMethod("reset", &CorrelationWrap::Reset), InstanceAccessor("value", &CorrelationWrap::Value, nullptr), InstanceAccessor("ready", &CorrelationWrap::Ready, nullptr)}); }
  CorrelationWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<CorrelationWrap>(info), _p(info[0].As<Napi::Number>().Uint32Value()) {}

 private:
  Napi::Value Update(const Napi::CallbackInfo& info)
  {
    double x = info[0].As<Napi::Number>().DoubleValue(), y = info[1].As<Napi::Number>().DoubleValue();
    _xbuf.push_back(x);
    _ybuf.push_back(y);
    if (_xbuf.size() > _p)
    {
      _xbuf.erase(_xbuf.begin());
      _ybuf.erase(_ybuf.begin());
    }
    if (_xbuf.size() >= _p)
    {
      double p = static_cast<double>(_p);
      double sx = 0, sy = 0, sxy = 0, sx2 = 0, sy2 = 0;
      for (size_t i = 0; i < _p; ++i)
      {
        sx += _xbuf[i];
        sy += _ybuf[i];
        sxy += _xbuf[i] * _ybuf[i];
        sx2 += _xbuf[i] * _xbuf[i];
        sy2 += _ybuf[i] * _ybuf[i];
      }
      double num = p * sxy - sx * sy;
      double den = std::sqrt((p * sx2 - sx * sx) * (p * sy2 - sy * sy));
      if (den == 0.0)
      {
        return info.Env().Undefined();
      }
      _v = num / den;
    }
    return Napi::Number::New(info.Env(), _v);
  }
  Napi::Value Value(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _v); }
  Napi::Value Ready(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), _xbuf.size() >= _p); }
  void Reset(const Napi::CallbackInfo&)
  {
    _v = 0;
    _xbuf.clear();
    _ybuf.clear();
  }
  size_t _p;
  double _v = 0;
  std::vector<double> _xbuf, _ybuf;
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
  exports.Set("skewness", Napi::Function::New(env, batch_skewness));
  exports.Set("kurtosis", Napi::Function::New(env, batch_kurtosis));
  exports.Set("rolling_zscore", Napi::Function::New(env, batch_rolling_zscore));
  exports.Set("shannon_entropy", Napi::Function::New(env, batch_shannon_entropy));
  exports.Set("parkinson_vol", Napi::Function::New(env, batch_parkinson_vol));
  exports.Set("rogers_satchell_vol", Napi::Function::New(env, batch_rogers_satchell_vol));
  exports.Set("correlation", Napi::Function::New(env, batch_correlation));
  exports.Set("adf", Napi::Function::New(env, batch_adf));

  // Streaming
  exports.Set("SMA", SMAWrap::Init(env));
  exports.Set("EMA", EMAWrap::Init(env));
  exports.Set("RSI", RSIWrap::Init(env));
  exports.Set("ATR", ATRWrap::Init(env));
  exports.Set("MACD", MACDWrap::Init(env));
  exports.Set("Bollinger", BollingerWrap::Init(env));
  exports.Set("RMA", RMAWrap::Init(env));
  exports.Set("DEMA", DEMAWrap::Init(env));
  exports.Set("TEMA", TEMAWrap::Init(env));
  exports.Set("KAMA", KAMAWrap::Init(env));
  exports.Set("Slope", SlopeWrap::Init(env));
  exports.Set("Stochastic", StochasticWrap::Init(env));
  exports.Set("CCI", CCIWrap::Init(env));
  exports.Set("OBV", OBVWrap::Init(env));
  exports.Set("VWAP", VWAPWrap::Init(env));
  exports.Set("CVD", CVDWrap::Init(env));
  exports.Set("Skewness", SkewnessWrap::Init(env));
  exports.Set("Kurtosis", KurtosisWrap::Init(env));
  exports.Set("RollingZScore", RollingZScoreWrap::Init(env));
  exports.Set("ShannonEntropy", ShannonEntropyWrap::Init(env));
  exports.Set("ParkinsonVol", ParkinsonVolWrap::Init(env));
  exports.Set("RogersSatchellVol", RogersSatchellVolWrap::Init(env));
  exports.Set("Correlation", CorrelationWrap::Init(env));
}

}  // namespace node_flox
