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

#include <cstring>
#include <fstream>
#include <numeric>
#include <sstream>
#include <vector>

using namespace flox;

// ── Helpers ─────────────────────────────────────────────────────────

static std::vector<double> toDoubleVec(Napi::Env env, Napi::Value val)
{
  auto arr = val.As<Napi::Float64Array>();
  return {arr.Data(), arr.Data() + arr.ElementLength()};
}

static Napi::Float64Array fromDoubleVec(Napi::Env env, const std::vector<double>& v)
{
  auto buf = Napi::Float64Array::New(env, v.size());
  std::memcpy(buf.Data(), v.data(), v.size() * sizeof(double));
  return buf;
}

static Napi::Float64Array fromDoubleSpan(Napi::Env env, const double* data, size_t n)
{
  auto buf = Napi::Float64Array::New(env, n);
  std::memcpy(buf.Data(), data, n * sizeof(double));
  return buf;
}

static int64_t normalizeTs(int64_t t)
{
  if (t < static_cast<int64_t>(1e12))
  {
    return t * 1'000'000'000LL;
  }
  if (t < static_cast<int64_t>(1e15))
  {
    return t * 1'000'000LL;
  }
  if (t < static_cast<int64_t>(1e18))
  {
    return t * 1'000LL;
  }
  return t;
}

// ── Bar data ────────────────────────────────────────────────────────

struct OhlcvBar
{
  int64_t timestamp_ns;
  int64_t open_raw;
  int64_t high_raw;
  int64_t low_raw;
  int64_t close_raw;
  int64_t volume_raw;
};

// ── Signal ──────────────────────────────────────────────────────────

struct Signal
{
  int64_t timestamp_ns;
  int64_t quantity_raw;
  int64_t price_raw;
  uint8_t side;
  uint8_t order_type;
};

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

  const std::vector<Signal>& signals() const { return _signals; }

 private:
  void add(int64_t ts, uint8_t side, double qty, double price, uint8_t type)
  {
    _signals.push_back({.timestamp_ns = normalizeTs(ts),
                        .quantity_raw = Quantity::fromDouble(qty).raw(),
                        .price_raw = price > 0 ? Price::fromDouble(price).raw() : 0,
                        .side = side,
                        .order_type = type});
  }

  Napi::Value Buy(const Napi::CallbackInfo& info)
  {
    add(info[0].As<Napi::Number>().Int64Value(), 0, info[1].As<Napi::Number>().DoubleValue(), 0,
        0);
    return info.This();
  }

  Napi::Value Sell(const Napi::CallbackInfo& info)
  {
    add(info[0].As<Napi::Number>().Int64Value(), 1, info[1].As<Napi::Number>().DoubleValue(), 0,
        0);
    return info.This();
  }

  Napi::Value LimitBuy(const Napi::CallbackInfo& info)
  {
    add(info[0].As<Napi::Number>().Int64Value(), 0, info[2].As<Napi::Number>().DoubleValue(),
        info[1].As<Napi::Number>().DoubleValue(), 1);
    return info.This();
  }

  Napi::Value LimitSell(const Napi::CallbackInfo& info)
  {
    add(info[0].As<Napi::Number>().Int64Value(), 1, info[2].As<Napi::Number>().DoubleValue(),
        info[1].As<Napi::Number>().DoubleValue(), 1);
    return info.This();
  }

  Napi::Value Length(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), _signals.size()); }

  void Clear(const Napi::CallbackInfo&) { _signals.clear(); }

  std::vector<Signal> _signals;
};

// ── Stats ───────────────────────────────────────────────────────────

static Napi::Object statsToObject(Napi::Env env, const BacktestStats& s)
{
  auto obj = Napi::Object::New(env);
  obj.Set("totalTrades", Napi::Number::New(env, s.totalTrades));
  obj.Set("winningTrades", Napi::Number::New(env, s.winningTrades));
  obj.Set("losingTrades", Napi::Number::New(env, s.losingTrades));
  obj.Set("initialCapital", Napi::Number::New(env, s.initialCapital));
  obj.Set("finalCapital", Napi::Number::New(env, s.finalCapital));
  obj.Set("netPnl", Napi::Number::New(env, s.netPnl));
  obj.Set("totalPnl", Napi::Number::New(env, s.totalPnl));
  obj.Set("totalFees", Napi::Number::New(env, s.totalFees));
  obj.Set("grossProfit", Napi::Number::New(env, s.grossProfit));
  obj.Set("grossLoss", Napi::Number::New(env, s.grossLoss));
  obj.Set("maxDrawdown", Napi::Number::New(env, s.maxDrawdown));
  obj.Set("maxDrawdownPct", Napi::Number::New(env, s.maxDrawdownPct));
  obj.Set("winRate", Napi::Number::New(env, s.winRate));
  obj.Set("profitFactor", Napi::Number::New(env, s.profitFactor));
  obj.Set("avgWin", Napi::Number::New(env, s.avgWin));
  obj.Set("avgLoss", Napi::Number::New(env, s.avgLoss));
  obj.Set("sharpe", Napi::Number::New(env, s.sharpeRatio));
  obj.Set("sortino", Napi::Number::New(env, s.sortinoRatio));
  obj.Set("calmar", Napi::Number::New(env, s.calmarRatio));
  obj.Set("returnPct", Napi::Number::New(env, s.returnPct));
  return obj;
}

// ── Backtest execution ──────────────────────────────────────────────

static BacktestStats executeSignals(const std::vector<OhlcvBar>& bars,
                                    const std::vector<Signal>& signals, SymbolId symbolId,
                                    const BacktestConfig& config)
{
  SimulatedClock clock;
  SimulatedExecutor executor(clock);
  OrderId nextOrderId = 1;

  std::vector<size_t> sigOrder(signals.size());
  std::iota(sigOrder.begin(), sigOrder.end(), 0);
  std::sort(sigOrder.begin(), sigOrder.end(),
            [&](size_t a, size_t b)
            { return signals[a].timestamp_ns < signals[b].timestamp_ns; });

  size_t sigIdx = 0;

  for (const auto& bar : bars)
  {
    clock.advanceTo(bar.timestamp_ns);
    executor.onBar(symbolId, Price::fromRaw(bar.close_raw));

    while (sigIdx < signals.size() &&
           signals[sigOrder[sigIdx]].timestamp_ns <= bar.timestamp_ns)
    {
      const auto& sig = signals[sigOrder[sigIdx]];
      Order order{.id = nextOrderId++,
                  .side = sig.side == 0 ? Side::BUY : Side::SELL,
                  .price = Price::fromRaw(sig.price_raw),
                  .quantity = Quantity::fromRaw(sig.quantity_raw),
                  .type = sig.order_type == 0 ? OrderType::MARKET : OrderType::LIMIT,
                  .symbol = symbolId};
      executor.submitOrder(order);
      ++sigIdx;
    }
  }

  BacktestResult result(config, executor.fills().size());
  for (const auto& fill : executor.fills())
  {
    result.recordFill(fill);
  }
  return result.computeStats();
}

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
         InstanceMethod("run", &EngineWrap::Run),
         InstanceAccessor("barCount", &EngineWrap::BarCount, nullptr),
         InstanceAccessor("ts", &EngineWrap::Timestamps, nullptr),
         InstanceAccessor("open", &EngineWrap::Opens, nullptr),
         InstanceAccessor("high", &EngineWrap::Highs, nullptr),
         InstanceAccessor("low", &EngineWrap::Lows, nullptr),
         InstanceAccessor("close", &EngineWrap::Closes, nullptr),
         InstanceAccessor("volume", &EngineWrap::Volumes, nullptr)});
  }

  EngineWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<EngineWrap>(info)
  {
    _config.initialCapital =
        info.Length() > 0 ? info[0].As<Napi::Number>().DoubleValue() : 100000.0;
    _config.feeRate = info.Length() > 1 ? info[1].As<Napi::Number>().DoubleValue() : 0.0001;
    _config.usePercentageFee = true;
  }

 private:
  void LoadCsv(const Napi::CallbackInfo& info)
  {
    std::string path = info[0].As<Napi::String>().Utf8Value();
    std::ifstream file(path);
    if (!file.is_open())
    {
      Napi::Error::New(info.Env(), "cannot open: " + path).ThrowAsJavaScriptException();
      return;
    }

    std::string line;
    std::getline(file, line);  // skip header

    _bars.clear();
    while (std::getline(file, line))
    {
      if (line.empty())
      {
        continue;
      }
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

      _bars.push_back({.timestamp_ns = ts,
                        .open_raw = Price::fromDouble(o).raw(),
                        .high_raw = Price::fromDouble(h).raw(),
                        .low_raw = Price::fromDouble(l).raw(),
                        .close_raw = Price::fromDouble(c).raw(),
                        .volume_raw = Volume::fromDouble(v).raw()});
    }
  }

  void LoadOhlcv(const Napi::CallbackInfo& info)
  {
    auto obj = info[0].As<Napi::Object>();
    auto ts = obj.Get("ts").As<Napi::Float64Array>();
    auto op = obj.Get("open").As<Napi::Float64Array>();
    auto hi = obj.Get("high").As<Napi::Float64Array>();
    auto lo = obj.Get("low").As<Napi::Float64Array>();
    auto cl = obj.Get("close").As<Napi::Float64Array>();
    auto vo = obj.Get("volume").As<Napi::Float64Array>();
    size_t n = ts.ElementLength();

    _bars.resize(n);
    for (size_t i = 0; i < n; ++i)
    {
      _bars[i] = {.timestamp_ns = normalizeTs(static_cast<int64_t>(ts[i])),
                   .open_raw = Price::fromDouble(op[i]).raw(),
                   .high_raw = Price::fromDouble(hi[i]).raw(),
                   .low_raw = Price::fromDouble(lo[i]).raw(),
                   .close_raw = Price::fromDouble(cl[i]).raw(),
                   .volume_raw = Volume::fromDouble(vo[i]).raw()};
    }
  }

  Napi::Value Run(const Napi::CallbackInfo& info)
  {
    auto* builder = Napi::ObjectWrap<SignalBuilderWrap>::Unwrap(info[0].As<Napi::Object>());
    uint32_t symbol = info.Length() > 1 ? info[1].As<Napi::Number>().Uint32Value() : 1;

    auto stats = executeSignals(_bars, builder->signals(), symbol, _config);
    return statsToObject(info.Env(), stats);
  }

  Napi::Value BarCount(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(), _bars.size());
  }

  Napi::Value Timestamps(const Napi::CallbackInfo& info)
  {
    auto buf = Napi::Float64Array::New(info.Env(), _bars.size());
    for (size_t i = 0; i < _bars.size(); ++i)
    {
      buf[i] = static_cast<double>(_bars[i].timestamp_ns);
    }
    return buf;
  }

  Napi::Value extractField(const Napi::CallbackInfo& info, int64_t OhlcvBar::*field)
  {
    auto buf = Napi::Float64Array::New(info.Env(), _bars.size());
    for (size_t i = 0; i < _bars.size(); ++i)
    {
      buf[i] = Price::fromRaw(_bars[i].*field).toDouble();
    }
    return buf;
  }

  Napi::Value Opens(const Napi::CallbackInfo& info) { return extractField(info, &OhlcvBar::open_raw); }
  Napi::Value Highs(const Napi::CallbackInfo& info) { return extractField(info, &OhlcvBar::high_raw); }
  Napi::Value Lows(const Napi::CallbackInfo& info) { return extractField(info, &OhlcvBar::low_raw); }
  Napi::Value Closes(const Napi::CallbackInfo& info) { return extractField(info, &OhlcvBar::close_raw); }
  Napi::Value Volumes(const Napi::CallbackInfo& info)
  {
    return extractField(info, &OhlcvBar::volume_raw);
  }

  BacktestConfig _config;
  std::vector<OhlcvBar> _bars;
};

// ── Batch indicators ────────────────────────────────────────────────

static Napi::Value JsSma(const Napi::CallbackInfo& info)
{
  auto input = info[0].As<Napi::Float64Array>();
  size_t period = info[1].As<Napi::Number>().Uint32Value();
  size_t n = input.ElementLength();

  auto result = indicator::SMA(period).compute(std::span<const double>(input.Data(), n));
  return fromDoubleVec(info.Env(), result);
}

static Napi::Value JsEma(const Napi::CallbackInfo& info)
{
  auto input = info[0].As<Napi::Float64Array>();
  size_t period = info[1].As<Napi::Number>().Uint32Value();
  size_t n = input.ElementLength();

  std::vector<double> out(n);
  indicator::EMA(period).compute(std::span<const double>(input.Data(), n),
                                 std::span<double>(out.data(), n));
  return fromDoubleVec(info.Env(), out);
}

static Napi::Value JsRsi(const Napi::CallbackInfo& info)
{
  auto input = info[0].As<Napi::Float64Array>();
  size_t period = info[1].As<Napi::Number>().Uint32Value();
  size_t n = input.ElementLength();

  std::vector<double> out(n);
  indicator::RSI(period).compute(std::span<const double>(input.Data(), n),
                                 std::span<double>(out.data(), n));
  return fromDoubleVec(info.Env(), out);
}

static Napi::Value JsAtr(const Napi::CallbackInfo& info)
{
  auto high = info[0].As<Napi::Float64Array>();
  auto low = info[1].As<Napi::Float64Array>();
  auto close = info[2].As<Napi::Float64Array>();
  size_t period = info[3].As<Napi::Number>().Uint32Value();
  size_t n = high.ElementLength();

  std::vector<double> out(n);
  indicator::ATR(period).compute(std::span<const double>(high.Data(), n),
                                 std::span<const double>(low.Data(), n),
                                 std::span<const double>(close.Data(), n),
                                 std::span<double>(out.data(), n));
  return fromDoubleVec(info.Env(), out);
}

static Napi::Value JsMacd(const Napi::CallbackInfo& info)
{
  auto input = info[0].As<Napi::Float64Array>();
  size_t fast = info[1].As<Napi::Number>().Uint32Value();
  size_t slow = info[2].As<Napi::Number>().Uint32Value();
  size_t signal = info[3].As<Napi::Number>().Uint32Value();
  size_t n = input.ElementLength();

  std::vector<double> line(n), sig(n), hist(n);
  indicator::MACD(fast, slow, signal)
      .compute(std::span<const double>(input.Data(), n), std::span<double>(line.data(), n),
               std::span<double>(sig.data(), n), std::span<double>(hist.data(), n));

  auto obj = Napi::Object::New(info.Env());
  obj.Set("line", fromDoubleVec(info.Env(), line));
  obj.Set("signal", fromDoubleVec(info.Env(), sig));
  obj.Set("histogram", fromDoubleVec(info.Env(), hist));
  return obj;
}

static Napi::Value JsBollinger(const Napi::CallbackInfo& info)
{
  auto input = info[0].As<Napi::Float64Array>();
  size_t period = info[1].As<Napi::Number>().Uint32Value();
  double stddev = info[2].As<Napi::Number>().DoubleValue();
  size_t n = input.ElementLength();

  auto result =
      indicator::Bollinger(period, stddev).compute(std::span<const double>(input.Data(), n));

  auto obj = Napi::Object::New(info.Env());
  obj.Set("upper", fromDoubleVec(info.Env(), result.upper));
  obj.Set("middle", fromDoubleVec(info.Env(), result.middle));
  obj.Set("lower", fromDoubleVec(info.Env(), result.lower));
  return obj;
}

// ── Module init ─────────────────────────────────────────────────────

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
