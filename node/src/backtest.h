// node/src/backtest.h -- SimulatedExecutor + BacktestResult

#pragma once
#include <napi.h>
#include "flox/capi/flox_capi.h"
#include "rate_limit.h"

namespace node_flox
{

class LatencyDistributionWrap : public Napi::ObjectWrap<LatencyDistributionWrap>
{
 public:
  static Napi::Function Init(Napi::Env env)
  {
    return DefineClass(
        env, "LatencyDistribution",
        {InstanceMethod("setConstant", &LatencyDistributionWrap::SetConstant),
         InstanceMethod("setUniform", &LatencyDistributionWrap::SetUniform),
         InstanceMethod("setLognormal", &LatencyDistributionWrap::SetLognormal),
         InstanceMethod("setEmpirical", &LatencyDistributionWrap::SetEmpirical),
         InstanceMethod("setBurstCorrelation",
                        &LatencyDistributionWrap::SetBurstCorrelation),
         InstanceMethod("medianNs", &LatencyDistributionWrap::MedianNs)});
  }

  LatencyDistributionWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<LatencyDistributionWrap>(info),
        _h(flox_latency_distribution_create())
  {
  }
  ~LatencyDistributionWrap()
  {
    if (_h)
    {
      flox_latency_distribution_destroy(_h);
    }
  }
  FloxLatencyDistributionHandle handle() const { return _h; }

 private:
  void SetConstant(const Napi::CallbackInfo& info)
  {
    flox_latency_distribution_set_constant(_h, info[0].As<Napi::Number>().Int64Value());
  }
  void SetUniform(const Napi::CallbackInfo& info)
  {
    flox_latency_distribution_set_uniform(_h, info[0].As<Napi::Number>().Int64Value(),
                                          info[1].As<Napi::Number>().Int64Value());
  }
  void SetLognormal(const Napi::CallbackInfo& info)
  {
    flox_latency_distribution_set_lognormal(_h,
                                            info[0].As<Napi::Number>().Int64Value(),
                                            info[1].As<Napi::Number>().DoubleValue());
  }
  void SetEmpirical(const Napi::CallbackInfo& info)
  {
    auto arr = info[0].As<Napi::Array>();
    std::vector<int64_t> samples;
    samples.reserve(arr.Length());
    for (uint32_t i = 0; i < arr.Length(); ++i)
    {
      samples.push_back(arr.Get(i).As<Napi::Number>().Int64Value());
    }
    flox_latency_distribution_set_empirical(
        _h, samples.empty() ? nullptr : samples.data(),
        static_cast<uint32_t>(samples.size()));
  }
  void SetBurstCorrelation(const Napi::CallbackInfo& info)
  {
    flox_latency_distribution_set_burst_correlation(
        _h, info[0].As<Napi::Number>().DoubleValue());
  }
  Napi::Value MedianNs(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(),
                             static_cast<double>(flox_latency_distribution_median_ns(_h)));
  }

  FloxLatencyDistributionHandle _h;
};

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
                        InstanceMethod("setSubmitAckLatency", &SimulatedExecutorWrap::SetSubmitAck),
                        InstanceMethod("setCancelAckLatency", &SimulatedExecutorWrap::SetCancelAck),
                        InstanceMethod("setReplaceAckLatency", &SimulatedExecutorWrap::SetReplaceAck),
                        InstanceMethod("setSubmitAckLatencyDistribution",
                                       &SimulatedExecutorWrap::SetSubmitAckDist),
                        InstanceMethod("setCancelAckLatencyDistribution",
                                       &SimulatedExecutorWrap::SetCancelAckDist),
                        InstanceMethod("setReplaceAckLatencyDistribution",
                                       &SimulatedExecutorWrap::SetReplaceAckDist),
                        InstanceMethod("applyLatencyProfile", &SimulatedExecutorWrap::ApplyLatencyProfile),
                        InstanceMethod("setRateLimitPolicy",
                                       &SimulatedExecutorWrap::SetRateLimitPolicy),
                        InstanceMethod("clearRateLimitPolicy",
                                       &SimulatedExecutorWrap::ClearRateLimitPolicy),
                        InstanceMethod("setSTPMode", &SimulatedExecutorWrap::SetSTPMode),
                        InstanceAccessor("fillCount", &SimulatedExecutorWrap::FillCount, nullptr)});
  }

  SimulatedExecutorWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<SimulatedExecutorWrap>(info), _h(flox_simulated_executor_create()) {}
  ~SimulatedExecutorWrap()
  {
    if (_h)
    {
      flox_simulated_executor_destroy(_h);
    }
  }
  FloxSimulatedExecutorHandle handle() const { return _h; }

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
    // Optional opts object as the 7th argument: { tif, reduceOnly, expiresAtNs }.
    if (info.Length() > 6 && info[6].IsObject())
    {
      auto opts = info[6].As<Napi::Object>();
      uint8_t tif = 0;
      bool reduceOnly = false;
      int64_t expiresAtNs = 0;
      if (opts.Has("tif"))
      {
        std::string tifStr = opts.Get("tif").As<Napi::String>().Utf8Value();
        if (tifStr == "ioc")
        {
          tif = 1;
        }
        else if (tifStr == "fok")
        {
          tif = 2;
        }
        else if (tifStr == "gtd")
        {
          tif = 3;
        }
        else if (tifStr == "post_only")
        {
          tif = 4;
        }
      }
      if (opts.Has("reduceOnly"))
      {
        reduceOnly = opts.Get("reduceOnly").As<Napi::Boolean>().Value();
      }
      if (opts.Has("expiresAtNs"))
      {
        expiresAtNs = opts.Get("expiresAtNs").As<Napi::Number>().Int64Value();
      }
      flox_simulated_executor_submit_order_ex(_h, id, s, price, qty, t, sym, tif,
                                              reduceOnly ? 1 : 0, expiresAtNs);
      return;
    }
    flox_simulated_executor_submit_order(_h, id, s, price, qty, t, sym);
  }
  void CancelOrder(const Napi::CallbackInfo& info) { flox_simulated_executor_cancel_order(_h, info[0].As<Napi::Number>().Int64Value()); }
  void CancelAll(const Napi::CallbackInfo& info) { flox_simulated_executor_cancel_all(_h, info[0].As<Napi::Number>().Uint32Value()); }
  void OnBar(const Napi::CallbackInfo& info) { flox_simulated_executor_on_bar(_h, info[0].As<Napi::Number>().Uint32Value(), info[1].As<Napi::Number>().DoubleValue()); }
  void OnTrade(const Napi::CallbackInfo& info) { flox_simulated_executor_on_trade(_h, info[0].As<Napi::Number>().Uint32Value(), info[1].As<Napi::Number>().DoubleValue(), info[2].As<Napi::Boolean>().Value() ? 1 : 0); }
  void AdvanceClock(const Napi::CallbackInfo& info) { flox_simulated_executor_advance_clock(_h, info[0].As<Napi::Number>().Int64Value()); }
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
    flox_simulated_executor_set_default_slippage(_h, m, ticks, tickSize, bps, impact);
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
    flox_simulated_executor_set_queue_model(_h, m, depth);
  }
  void SetSubmitAck(const Napi::CallbackInfo& info)
  {
    int64_t latency = info[0].As<Napi::Number>().Int64Value();
    int64_t jitter = info.Length() > 1 ? info[1].As<Napi::Number>().Int64Value() : 0;
    flox_simulated_executor_set_submit_ack_latency(_h, latency, jitter);
  }
  void SetCancelAck(const Napi::CallbackInfo& info)
  {
    int64_t latency = info[0].As<Napi::Number>().Int64Value();
    int64_t jitter = info.Length() > 1 ? info[1].As<Napi::Number>().Int64Value() : 0;
    flox_simulated_executor_set_cancel_ack_latency(_h, latency, jitter);
  }
  void SetReplaceAck(const Napi::CallbackInfo& info)
  {
    int64_t latency = info[0].As<Napi::Number>().Int64Value();
    int64_t jitter = info.Length() > 1 ? info[1].As<Napi::Number>().Int64Value() : 0;
    flox_simulated_executor_set_replace_ack_latency(_h, latency, jitter);
  }
  void ApplyLatencyProfile(const Napi::CallbackInfo& info)
  {
    std::string name = info[0].As<Napi::String>().Utf8Value();
    flox_simulated_executor_apply_latency_profile(_h, name.c_str());
  }
  void SetSubmitAckDist(const Napi::CallbackInfo& info)
  {
    auto* w = Napi::ObjectWrap<LatencyDistributionWrap>::Unwrap(
        info[0].As<Napi::Object>());
    flox_simulated_executor_set_submit_ack_latency_distribution(_h, w->handle());
  }
  void SetCancelAckDist(const Napi::CallbackInfo& info)
  {
    auto* w = Napi::ObjectWrap<LatencyDistributionWrap>::Unwrap(
        info[0].As<Napi::Object>());
    flox_simulated_executor_set_cancel_ack_latency_distribution(_h, w->handle());
  }
  void SetReplaceAckDist(const Napi::CallbackInfo& info)
  {
    auto* w = Napi::ObjectWrap<LatencyDistributionWrap>::Unwrap(
        info[0].As<Napi::Object>());
    flox_simulated_executor_set_replace_ack_latency_distribution(_h, w->handle());
  }
  void SetRateLimitPolicy(const Napi::CallbackInfo& info)
  {
    auto* w = Napi::ObjectWrap<RateLimitPolicyWrap>::Unwrap(
        info[0].As<Napi::Object>());
    flox_simulated_executor_set_rate_limit_policy(_h, w->handle());
  }
  void ClearRateLimitPolicy(const Napi::CallbackInfo&)
  {
    flox_simulated_executor_clear_rate_limit_policy(_h);
  }
  void SetSTPMode(const Napi::CallbackInfo& info)
  {
    std::string mode = info[0].As<Napi::String>().Utf8Value();
    uint8_t m = 0;
    if (mode == "cancel_newest")
    {
      m = 1;
    }
    else if (mode == "cancel_oldest")
    {
      m = 2;
    }
    else if (mode == "cancel_both")
    {
      m = 3;
    }
    else if (mode == "decrement")
    {
      m = 4;
    }
    flox_simulated_executor_set_stp_mode(_h, m);
  }
  Napi::Value FillCount(const Napi::CallbackInfo& info) { return Napi::Number::New(info.Env(), flox_simulated_executor_fill_count(_h)); }
  FloxSimulatedExecutorHandle _h;
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
  exports.Set("LatencyDistribution", LatencyDistributionWrap::Init(env));
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
