#include <napi.h>

#include "flox/backtest/backtest_result.h"
#include "flox/backtest/simulated_clock.h"
#include "flox/backtest/simulated_executor.h"
#include "flox/common.h"
#include "flox/indicator/atr.h"
#include "flox/indicator/bollinger.h"
#include "flox/indicator/ema.h"
#include "flox/indicator/macd.h"
#include "flox/indicator/rsi.h"
#include "flox/indicator/sma.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <numeric>
#include <sstream>
#include <vector>

using namespace flox;

// ── Helpers ─────────────────────────────────────────────────────────

static Napi::Float64Array fromVec(Napi::Env env, const std::vector<double>& v)
{
  auto buf = Napi::Float64Array::New(env, v.size());
  std::memcpy(buf.Data(), v.data(), v.size() * sizeof(double));
  return buf;
}

static int64_t normalizeTs(int64_t t)
{
  if (t < static_cast<int64_t>(1e12))
    return t * 1'000'000'000LL;
  if (t < static_cast<int64_t>(1e15))
    return t * 1'000'000LL;
  if (t < static_cast<int64_t>(1e18))
    return t * 1'000LL;
  return t;
}

// ── Bar / Signal / SymbolData ───────────────────────────────────────

struct OhlcvBar
{
  int64_t timestamp_ns, open_raw, high_raw, low_raw, close_raw, volume_raw;
};

struct InternalSignal
{
  int64_t timestamp_ns, quantity_raw, price_raw;
  uint8_t side, order_type;
  uint32_t symbol_id;
};

struct SymbolData
{
  uint32_t id;
  std::vector<OhlcvBar> bars;
};

// ── CSV / Resample ──────────────────────────────────────────────────

static std::vector<OhlcvBar> parseCsv(const std::string& path)
{
  std::ifstream file(path);
  if (!file.is_open())
    throw std::runtime_error("cannot open: " + path);

  std::vector<OhlcvBar> bars;
  std::string line;
  std::getline(file, line);

  while (std::getline(file, line))
  {
    if (line.empty())
      continue;
    std::istringstream ss(line);
    std::string tok;
    std::getline(ss, tok, ',');
    int64_t ts = normalizeTs(std::stoll(tok));
    std::getline(ss, tok, ',');
    double o = std::stod(tok);
    std::getline(ss, tok, ',');
    double h = std::stod(tok);
    std::getline(ss, tok, ',');
    double l = std::stod(tok);
    std::getline(ss, tok, ',');
    double c = std::stod(tok);
    std::getline(ss, tok, ',');
    double v = std::stod(tok);
    bars.push_back({ts, Price::fromDouble(o).raw(), Price::fromDouble(h).raw(),
                    Price::fromDouble(l).raw(), Price::fromDouble(c).raw(),
                    Volume::fromDouble(v).raw()});
  }
  return bars;
}

static std::vector<OhlcvBar> resampleBars(const std::vector<OhlcvBar>& src, int64_t intervalNs)
{
  if (src.empty() || intervalNs <= 0)
    return {};
  std::vector<OhlcvBar> out;
  int64_t bucket = (src[0].timestamp_ns / intervalNs) * intervalNs;
  OhlcvBar cur = src[0];
  cur.timestamp_ns = bucket;
  for (size_t i = 1; i < src.size(); ++i)
  {
    int64_t b = (src[i].timestamp_ns / intervalNs) * intervalNs;
    if (b != bucket)
    {
      out.push_back(cur);
      bucket = b;
      cur = src[i];
      cur.timestamp_ns = b;
    }
    else
    {
      if (src[i].high_raw > cur.high_raw)
        cur.high_raw = src[i].high_raw;
      if (src[i].low_raw < cur.low_raw)
        cur.low_raw = src[i].low_raw;
      cur.close_raw = src[i].close_raw;
      cur.volume_raw += src[i].volume_raw;
    }
  }
  out.push_back(cur);
  return out;
}

static std::string inferSymbol(const std::string& path)
{
  auto pos = path.find_last_of('/');
  std::string f = (pos != std::string::npos) ? path.substr(pos + 1) : path;
  auto dot = f.find('.');
  if (dot != std::string::npos)
    f = f.substr(0, dot);
  for (const char* suf : {"_1m", "_5m", "_15m", "_1h", "_4h", "_1d"})
  {
    size_t sl = std::strlen(suf);
    if (f.size() > sl && f.substr(f.size() - sl) == suf)
    {
      f = f.substr(0, f.size() - sl);
      break;
    }
  }
  for (auto& c : f)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return f;
}

static int64_t parseInterval(const std::string& s)
{
  size_t numEnd = 0;
  int64_t val = std::stoll(s, &numEnd);
  std::string u = s.substr(numEnd);
  if (u == "s")
    return val * 1'000'000'000LL;
  if (u == "m")
    return val * 60'000'000'000LL;
  if (u == "h")
    return val * 3'600'000'000'000LL;
  if (u == "d")
    return val * 86'400'000'000'000LL;
  throw std::invalid_argument("unknown interval: " + u);
}

// ── Stats helper ────────────────────────────────────────────────────

static Napi::Object statsObj(Napi::Env env, const BacktestStats& s)
{
  auto o = Napi::Object::New(env);
  o.Set("totalTrades", (double)s.totalTrades);
  o.Set("winningTrades", (double)s.winningTrades);
  o.Set("losingTrades", (double)s.losingTrades);
  o.Set("initialCapital", s.initialCapital);
  o.Set("finalCapital", s.finalCapital);
  o.Set("netPnl", s.netPnl);
  o.Set("totalPnl", s.totalPnl);
  o.Set("totalFees", s.totalFees);
  o.Set("grossProfit", s.grossProfit);
  o.Set("grossLoss", s.grossLoss);
  o.Set("maxDrawdown", s.maxDrawdown);
  o.Set("maxDrawdownPct", s.maxDrawdownPct);
  o.Set("winRate", s.winRate);
  o.Set("profitFactor", s.profitFactor);
  o.Set("avgWin", s.avgWin);
  o.Set("avgLoss", s.avgLoss);
  o.Set("sharpe", s.sharpeRatio);
  o.Set("sortino", s.sortinoRatio);
  o.Set("calmar", s.calmarRatio);
  o.Set("returnPct", s.returnPct);
  return o;
}

// ── SignalBuilder ───────────────────────────────────────────────────

class SignalBuilderWrap : public Napi::ObjectWrap<SignalBuilderWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "SignalBuilder",
                       {InstanceMethod("buy", &SignalBuilderWrap::Buy),
                        InstanceMethod("sell", &SignalBuilderWrap::Sell),
                        InstanceMethod("limitBuy", &SignalBuilderWrap::LimitBuy),
                        InstanceMethod("limitSell", &SignalBuilderWrap::LimitSell),
                        InstanceAccessor("length", &SignalBuilderWrap::Length, nullptr),
                        InstanceMethod("clear", &SignalBuilderWrap::Clear)});
  }

  SignalBuilderWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<SignalBuilderWrap>(info) {}

  const std::vector<InternalSignal>& signals() const { return _signals; }
  const std::vector<std::string>& symbolNames() const { return _names; }

 private:
  void add(int64_t ts, uint8_t side, double qty, double price, uint8_t type,
           const std::string& sym)
  {
    _signals.push_back({normalizeTs(ts), Quantity::fromDouble(qty).raw(),
                        price > 0 ? Price::fromDouble(price).raw() : 0LL, side, type, 0});
    _names.push_back(sym);
  }

  std::string getSym(const Napi::CallbackInfo& info, int idx)
  {
    if (info.Length() > idx && info[idx].IsString())
      return info[idx].As<Napi::String>().Utf8Value();
    return "";
  }

  Napi::Value Buy(const Napi::CallbackInfo& info)
  {
    add(info[0].As<Napi::Number>().Int64Value(), 0,
        info[1].As<Napi::Number>().DoubleValue(), 0, 0, getSym(info, 2));
    return info.This();
  }

  Napi::Value Sell(const Napi::CallbackInfo& info)
  {
    add(info[0].As<Napi::Number>().Int64Value(), 1,
        info[1].As<Napi::Number>().DoubleValue(), 0, 0, getSym(info, 2));
    return info.This();
  }

  Napi::Value LimitBuy(const Napi::CallbackInfo& info)
  {
    add(info[0].As<Napi::Number>().Int64Value(), 0,
        info[2].As<Napi::Number>().DoubleValue(),
        info[1].As<Napi::Number>().DoubleValue(), 1, getSym(info, 3));
    return info.This();
  }

  Napi::Value LimitSell(const Napi::CallbackInfo& info)
  {
    add(info[0].As<Napi::Number>().Int64Value(), 1,
        info[2].As<Napi::Number>().DoubleValue(),
        info[1].As<Napi::Number>().DoubleValue(), 1, getSym(info, 3));
    return info.This();
  }

  Napi::Value Length(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), _signals.size());
  }

  void Clear(const Napi::CallbackInfo&)
  {
    _signals.clear();
    _names.clear();
  }

  std::vector<InternalSignal> _signals;
  std::vector<std::string> _names;
};

// ── Engine ──────────────────────────────────────────────────────────

class EngineWrap : public Napi::ObjectWrap<EngineWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(
        env, "Engine",
        {InstanceMethod("loadCsv", &EngineWrap::LoadCsv),
         InstanceMethod("loadOhlcv", &EngineWrap::LoadOhlcv),
         InstanceMethod("resample", &EngineWrap::Resample),
         InstanceMethod("run", &EngineWrap::Run),
         InstanceMethod("barCount", &EngineWrap::BarCount),
         InstanceMethod("ts", &EngineWrap::Timestamps),
         InstanceMethod("open", &EngineWrap::Opens),
         InstanceMethod("high", &EngineWrap::Highs),
         InstanceMethod("low", &EngineWrap::Lows),
         InstanceMethod("close", &EngineWrap::Closes),
         InstanceMethod("volume", &EngineWrap::Volumes),
         InstanceAccessor("symbols", &EngineWrap::Symbols, nullptr)});
  }

  EngineWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<EngineWrap>(info)
  {
    _config.initialCapital =
        info.Length() > 0 ? info[0].As<Napi::Number>().DoubleValue() : 100000.0;
    _config.feeRate = info.Length() > 1 ? info[1].As<Napi::Number>().DoubleValue() : 0.0001;
    _config.usePercentageFee = true;
  }

 private:
  SymbolData& getOrCreate(const std::string& name)
  {
    auto it = _syms.find(name);
    if (it != _syms.end())
      return it->second;
    uint32_t id = static_cast<uint32_t>(_syms.size()) + 1;
    _order.push_back(name);
    return _syms.emplace(name, SymbolData{id, {}}).first->second;
  }

  const SymbolData& resolve(const Napi::CallbackInfo& info, int idx = 0) const
  {
    std::string sym;
    if (info.Length() > idx && info[idx].IsString())
      sym = info[idx].As<Napi::String>().Utf8Value();
    if (sym.empty())
    {
      if (_order.empty())
        throw std::runtime_error("no data loaded");
      return _syms.at(_order.front());
    }
    auto it = _syms.find(sym);
    if (it == _syms.end())
      throw std::invalid_argument("unknown symbol: " + sym);
    return it->second;
  }

  Napi::Value extractField(const Napi::CallbackInfo& info, int64_t OhlcvBar::*f)
  {
    auto& bars = resolve(info).bars;
    auto buf = Napi::Float64Array::New(info.Env(), bars.size());
    for (size_t i = 0; i < bars.size(); ++i)
      buf[i] = Price::fromRaw(bars[i].*f).toDouble();
    return buf;
  }

  // -- Methods --

  void LoadCsv(const Napi::CallbackInfo& info)
  {
    std::string path = info[0].As<Napi::String>().Utf8Value();
    std::string sym = (info.Length() > 1 && info[1].IsString())
                          ? info[1].As<Napi::String>().Utf8Value()
                          : inferSymbol(path);
    getOrCreate(sym).bars = parseCsv(path);
  }

  void LoadOhlcv(const Napi::CallbackInfo& info)
  {
    auto obj = info[0].As<Napi::Object>();
    std::string sym = (info.Length() > 1 && info[1].IsString())
                          ? info[1].As<Napi::String>().Utf8Value()
                          : "default";

    auto ts = obj.Get("ts").As<Napi::Float64Array>();
    auto op = obj.Get("open").As<Napi::Float64Array>();
    auto hi = obj.Get("high").As<Napi::Float64Array>();
    auto lo = obj.Get("low").As<Napi::Float64Array>();
    auto cl = obj.Get("close").As<Napi::Float64Array>();
    auto vo = obj.Get("volume").As<Napi::Float64Array>();
    size_t n = ts.ElementLength();

    auto& sd = getOrCreate(sym);
    sd.bars.resize(n);
    for (size_t i = 0; i < n; ++i)
      sd.bars[i] = {normalizeTs(static_cast<int64_t>(ts[i])),
                     Price::fromDouble(op[i]).raw(), Price::fromDouble(hi[i]).raw(),
                     Price::fromDouble(lo[i]).raw(), Price::fromDouble(cl[i]).raw(),
                     Volume::fromDouble(vo[i]).raw()};
  }

  void Resample(const Napi::CallbackInfo& info)
  {
    std::string src = info[0].As<Napi::String>().Utf8Value();
    std::string dst = info[1].As<Napi::String>().Utf8Value();
    std::string interval = info[2].As<Napi::String>().Utf8Value();
    auto it = _syms.find(src);
    if (it == _syms.end())
      throw std::invalid_argument("unknown symbol: " + src);
    getOrCreate(dst).bars = resampleBars(it->second.bars, parseInterval(interval));
  }

  Napi::Value Run(const Napi::CallbackInfo& info)
  {
    auto* builder = Napi::ObjectWrap<SignalBuilderWrap>::Unwrap(info[0].As<Napi::Object>());
    auto sigs = builder->signals();
    const auto& names = builder->symbolNames();

    uint32_t defSym = _order.empty() ? 1 : _syms.at(_order.front()).id;
    for (size_t i = 0; i < sigs.size(); ++i)
    {
      if (names[i].empty())
      {
        sigs[i].symbol_id = defSym;
      }
      else
      {
        auto it = _syms.find(names[i]);
        if (it == _syms.end())
          throw std::invalid_argument("unknown symbol: " + names[i]);
        sigs[i].symbol_id = it->second.id;
      }
    }

    std::vector<size_t> sigOrder(sigs.size());
    std::iota(sigOrder.begin(), sigOrder.end(), 0);
    std::sort(sigOrder.begin(), sigOrder.end(),
              [&](size_t a, size_t b)
              { return sigs[a].timestamp_ns < sigs[b].timestamp_ns; });

    struct MB
    {
      int64_t ts;
      int64_t close;
      uint32_t sym;
    };
    std::vector<MB> merged;
    for (auto& [_, sd] : _syms)
      for (auto& b : sd.bars)
        merged.push_back({b.timestamp_ns, b.close_raw, sd.id});
    std::sort(merged.begin(), merged.end(), [](const MB& a, const MB& b)
              { return a.ts < b.ts; });

    SimulatedClock clock;
    SimulatedExecutor executor(clock);
    OrderId nextId = 1;
    size_t si = 0;

    for (const auto& mb : merged)
    {
      clock.advanceTo(mb.ts);
      executor.onBar(mb.sym, Price::fromRaw(mb.close));
      while (si < sigs.size() && sigs[sigOrder[si]].timestamp_ns <= mb.ts)
      {
        const auto& sg = sigs[sigOrder[si]];
        Order order{.id = nextId++,
                    .side = sg.side == 0 ? Side::BUY : Side::SELL,
                    .price = Price::fromRaw(sg.price_raw),
                    .quantity = Quantity::fromRaw(sg.quantity_raw),
                    .type = sg.order_type == 0 ? OrderType::MARKET : OrderType::LIMIT,
                    .symbol = sg.symbol_id};
        executor.submitOrder(order);
        ++si;
      }
    }

    BacktestResult result(_config, executor.fills().size());
    for (const auto& fill : executor.fills())
      result.recordFill(fill);
    return statsObj(info.Env(), result.computeStats());
  }

  Napi::Value BarCount(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), resolve(info).bars.size());
  }

  Napi::Value Timestamps(const Napi::CallbackInfo& info)
  {
    auto& bars = resolve(info).bars;
    auto buf = Napi::Float64Array::New(info.Env(), bars.size());
    for (size_t i = 0; i < bars.size(); ++i)
      buf[i] = static_cast<double>(bars[i].timestamp_ns);
    return buf;
  }

  Napi::Value Opens(const Napi::CallbackInfo& info) { return extractField(info, &OhlcvBar::open_raw); }
  Napi::Value Highs(const Napi::CallbackInfo& info) { return extractField(info, &OhlcvBar::high_raw); }
  Napi::Value Lows(const Napi::CallbackInfo& info) { return extractField(info, &OhlcvBar::low_raw); }
  Napi::Value Closes(const Napi::CallbackInfo& info) { return extractField(info, &OhlcvBar::close_raw); }
  Napi::Value Volumes(const Napi::CallbackInfo& info) { return extractField(info, &OhlcvBar::volume_raw); }

  Napi::Value Symbols(const Napi::CallbackInfo& info)
  {
    auto arr = Napi::Array::New(info.Env(), _order.size());
    for (size_t i = 0; i < _order.size(); ++i)
      arr.Set(i, _order[i]);
    return arr;
  }

  BacktestConfig _config;
  std::map<std::string, SymbolData> _syms;
  std::vector<std::string> _order;
};

// ── Batch indicators ────────────────────────────────────────────────

static Napi::Value JsSma(const Napi::CallbackInfo& info)
{
  auto in = info[0].As<Napi::Float64Array>();
  size_t p = info[1].As<Napi::Number>().Uint32Value();
  return fromVec(info.Env(),
                 indicator::SMA(p).compute(std::span<const double>(in.Data(), in.ElementLength())));
}

static Napi::Value JsEma(const Napi::CallbackInfo& info)
{
  auto in = info[0].As<Napi::Float64Array>();
  size_t n = in.ElementLength(), p = info[1].As<Napi::Number>().Uint32Value();
  std::vector<double> out(n);
  indicator::EMA(p).compute({in.Data(), n}, {out.data(), n});
  return fromVec(info.Env(), out);
}

static Napi::Value JsRsi(const Napi::CallbackInfo& info)
{
  auto in = info[0].As<Napi::Float64Array>();
  size_t n = in.ElementLength(), p = info[1].As<Napi::Number>().Uint32Value();
  std::vector<double> out(n);
  indicator::RSI(p).compute({in.Data(), n}, {out.data(), n});
  return fromVec(info.Env(), out);
}

static Napi::Value JsAtr(const Napi::CallbackInfo& info)
{
  auto h = info[0].As<Napi::Float64Array>();
  auto l = info[1].As<Napi::Float64Array>();
  auto c = info[2].As<Napi::Float64Array>();
  size_t n = h.ElementLength(), p = info[3].As<Napi::Number>().Uint32Value();
  std::vector<double> out(n);
  indicator::ATR(p).compute({h.Data(), n}, {l.Data(), n}, {c.Data(), n}, {out.data(), n});
  return fromVec(info.Env(), out);
}

static Napi::Value JsMacd(const Napi::CallbackInfo& info)
{
  auto in = info[0].As<Napi::Float64Array>();
  size_t n = in.ElementLength();
  size_t f = info[1].As<Napi::Number>().Uint32Value();
  size_t s = info[2].As<Napi::Number>().Uint32Value();
  size_t sg = info[3].As<Napi::Number>().Uint32Value();
  std::vector<double> line(n), sig(n), hist(n);
  indicator::MACD(f, s, sg).compute({in.Data(), n}, {line.data(), n}, {sig.data(), n},
                                    {hist.data(), n});
  auto o = Napi::Object::New(info.Env());
  o.Set("line", fromVec(info.Env(), line));
  o.Set("signal", fromVec(info.Env(), sig));
  o.Set("histogram", fromVec(info.Env(), hist));
  return o;
}

static Napi::Value JsBollinger(const Napi::CallbackInfo& info)
{
  auto in = info[0].As<Napi::Float64Array>();
  size_t n = in.ElementLength(), p = info[1].As<Napi::Number>().Uint32Value();
  double sd = info[2].As<Napi::Number>().DoubleValue();
  auto r = indicator::Bollinger(p, sd).compute({in.Data(), n});
  auto o = Napi::Object::New(info.Env());
  o.Set("upper", fromVec(info.Env(), r.upper));
  o.Set("middle", fromVec(info.Env(), r.middle));
  o.Set("lower", fromVec(info.Env(), r.lower));
  return o;
}

// ── Module ──────────────────────────────────────────────────────────

Napi::Object Init(Napi::Env env, Napi::Object exports)
{
  exports.Set("Engine", EngineWrap::Init(env));
  exports.Set("SignalBuilder", SignalBuilderWrap::Init(env));
  exports.Set("sma", Napi::Function::New(env, JsSma));
  exports.Set("ema", Napi::Function::New(env, JsEma));
  exports.Set("rsi", Napi::Function::New(env, JsRsi));
  exports.Set("atr", Napi::Function::New(env, JsAtr));
  exports.Set("macd", Napi::Function::New(env, JsMacd));
  exports.Set("bollinger", Napi::Function::New(env, JsBollinger));
  return exports;
}

NODE_API_MODULE(flox_node, Init)
