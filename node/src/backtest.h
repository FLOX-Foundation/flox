// node/src/backtest.h -- SimulatedExecutor + BacktestResult

#pragma once
#include <napi.h>
#include "flox/capi/flox_capi.h"

namespace node_flox
{

class SimulatedExecutorWrap : public Napi::ObjectWrap<SimulatedExecutorWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "SimulatedExecutor",
                       {InstanceMethod("submitOrder", &SimulatedExecutorWrap::SubmitOrder),
                        InstanceMethod("cancelOrder", &SimulatedExecutorWrap::CancelOrder),
                        InstanceMethod("cancelAll", &SimulatedExecutorWrap::CancelAll),
                        InstanceMethod("onBar", &SimulatedExecutorWrap::OnBar),
                        InstanceMethod("onTrade", &SimulatedExecutorWrap::OnTrade),
                        InstanceMethod("advanceClock", &SimulatedExecutorWrap::AdvanceClock),
                        InstanceMethod("setDefaultSlippage", &SimulatedExecutorWrap::SetDefaultSlippage),
                        InstanceMethod("setQueueModel", &SimulatedExecutorWrap::SetQueueModel),
                        InstanceAccessor("fillCount", &SimulatedExecutorWrap::FillCount, nullptr)});
  }

  SimulatedExecutorWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<SimulatedExecutorWrap>(info), _h(flox_executor_create()) {}
  ~SimulatedExecutorWrap()
  {
    if (_h)
    {
      flox_executor_destroy(_h);
    }
  }
  FloxExecutorHandle handle() const { return _h; }

 private:
  void SubmitOrder(const Napi::CallbackInfo& info)
  {
    uint64_t id = info[0].As<Napi::Number>().Int64Value();
    std::string side = info[1].As<Napi::String>().Utf8Value();
    double price = info[2].As<Napi::Number>().DoubleValue();
    double qty = info[3].As<Napi::Number>().DoubleValue();
    std::string type = info.Length() > 4 && info[4].IsString() ? info[4].As<Napi::String>().Utf8Value() : "market";
    uint32_t sym = info.Length() > 5 ? info[5].As<Napi::Number>().Uint32Value() : 1;
    uint8_t s = side == "buy" ? 0 : 1;
    uint8_t t = 0;
    if (type == "limit")
    {
      t = 1;
    }
    flox_executor_submit_order(_h, id, s, price, qty, t, sym);
  }
  void CancelOrder(const Napi::CallbackInfo& info) { flox_executor_cancel_order(_h, info[0].As<Napi::Number>().Int64Value()); }
  void CancelAll(const Napi::CallbackInfo& info) { flox_executor_cancel_all(_h, info[0].As<Napi::Number>().Uint32Value()); }
  void OnBar(const Napi::CallbackInfo& info) { flox_executor_on_bar(_h, info[0].As<Napi::Number>().Uint32Value(), info[1].As<Napi::Number>().DoubleValue()); }
  void OnTrade(const Napi::CallbackInfo& info) { flox_executor_on_trade(_h, info[0].As<Napi::Number>().Uint32Value(), info[1].As<Napi::Number>().DoubleValue(), info[2].As<Napi::Boolean>().Value() ? 1 : 0); }
  void AdvanceClock(const Napi::CallbackInfo& info) { flox_executor_advance_clock(_h, info[0].As<Napi::Number>().Int64Value()); }
  void SetDefaultSlippage(const Napi::CallbackInfo& info)
  {
    std::string model = info[0].As<Napi::String>().Utf8Value();
    uint8_t m = 0;
    if (model == "fixed_ticks")
    {
      m = 1;
    }
    else if (model == "fixed_bps")
    {
      m = 2;
    }
    else if (model == "volume_impact")
    {
      m = 3;
    }
    int32_t ticks = info.Length() > 1 ? info[1].As<Napi::Number>().Int32Value() : 0;
    double tickSize = info.Length() > 2 ? info[2].As<Napi::Number>().DoubleValue() : 0.0;
    double bps = info.Length() > 3 ? info[3].As<Napi::Number>().DoubleValue() : 0.0;
    double impact = info.Length() > 4 ? info[4].As<Napi::Number>().DoubleValue() : 0.0;
    flox_executor_set_default_slippage(_h, m, ticks, tickSize, bps, impact);
  }
  void SetQueueModel(const Napi::CallbackInfo& info)
  {
    std::string model = info[0].As<Napi::String>().Utf8Value();
    uint8_t m = 0;
    if (model == "tob")
    {
      m = 1;
    }
    else if (model == "full")
    {
      m = 2;
    }
    uint32_t depth = info.Length() > 1 ? info[1].As<Napi::Number>().Uint32Value() : 1;
    flox_executor_set_queue_model(_h, m, depth);
  }
  Napi::Value FillCount(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_executor_fill_count(_h)); }
  FloxExecutorHandle _h;
};

class BacktestResultWrap : public Napi::ObjectWrap<BacktestResultWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(env, "BacktestResult",
                       {InstanceMethod("recordFill", &BacktestResultWrap::RecordFill),
                        InstanceMethod("ingestExecutor", &BacktestResultWrap::IngestExecutor),
                        InstanceMethod("stats", &BacktestResultWrap::Stats)});
  }
  BacktestResultWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<BacktestResultWrap>(info)
  {
    double cap = info.Length() > 0 ? info[0].As<Napi::Number>().DoubleValue() : 100000.0;
    double fee = info.Length() > 1 ? info[1].As<Napi::Number>().DoubleValue() : 0.0001;
    _h = flox_backtest_result_create(cap, fee, 1, 0.0, 0.0, 252.0);
  }
  ~BacktestResultWrap()
  {
    if (_h)
    {
      flox_backtest_result_destroy(_h);
    }
  }

 private:
  void RecordFill(const Napi::CallbackInfo& info)
  {
    flox_backtest_result_record_fill(_h,
                                     info[0].As<Napi::Number>().Int64Value(),
                                     info[1].As<Napi::Number>().Uint32Value(),
                                     info[2].As<Napi::String>().Utf8Value() == "buy" ? 0 : 1,
                                     info[3].As<Napi::Number>().DoubleValue(),
                                     info[4].As<Napi::Number>().DoubleValue(),
                                     info[5].As<Napi::Number>().Int64Value());
  }
  void IngestExecutor(const Napi::CallbackInfo& info)
  {
    auto* exec = Napi::ObjectWrap<SimulatedExecutorWrap>::Unwrap(info[0].As<Napi::Object>());
    flox_backtest_result_ingest_executor(_h, exec->handle());
  }
  Napi::Value Stats(const Napi::CallbackInfo& info)
  {
    FloxBacktestStats s{};
    flox_backtest_result_stats(_h, &s);
    auto o = Napi::Object::New(info.Env());
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
  FloxBacktestResultHandle _h;
};

inline void registerBacktest(Napi::Env env, Napi::Object exports)
{
  exports.Set("SimulatedExecutor", SimulatedExecutorWrap::Init(env));
  exports.Set("BacktestResult", BacktestResultWrap::Init(env));

  exports.Set("SLIPPAGE_NONE", Napi::Number::New(env, FLOX_SLIPPAGE_NONE));
  exports.Set("SLIPPAGE_FIXED_TICKS", Napi::Number::New(env, FLOX_SLIPPAGE_FIXED_TICKS));
  exports.Set("SLIPPAGE_FIXED_BPS", Napi::Number::New(env, FLOX_SLIPPAGE_FIXED_BPS));
  exports.Set("SLIPPAGE_VOLUME_IMPACT", Napi::Number::New(env, FLOX_SLIPPAGE_VOLUME_IMPACT));

  exports.Set("QUEUE_NONE", Napi::Number::New(env, FLOX_QUEUE_NONE));
  exports.Set("QUEUE_TOB", Napi::Number::New(env, FLOX_QUEUE_TOB));
  exports.Set("QUEUE_FULL", Napi::Number::New(env, FLOX_QUEUE_FULL));
}

}  // namespace node_flox
