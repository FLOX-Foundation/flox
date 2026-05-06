// node/src/grid_search.h — GridSearch type-erased grid for Node.js

#pragma once

#include <napi.h>

#include "error_translator.h"
#include "flox/capi/flox_capi.h"
#include "walk_forward.h"  // for statsToJsObj

#include <string>
#include <vector>

namespace node_flox
{

struct GridSearchCallContext
{
  Napi::Env env;
  Napi::FunctionReference factory_fn;
  std::vector<std::vector<double>> per_call_params;
};

inline int gridSearchFactoryThunk(void* user_data, uint64_t param_index,
                                  const double* params, uint32_t num_params,
                                  FloxBacktestStats* out_stats)
{
  auto* ctx = static_cast<GridSearchCallContext*>(user_data);
  Napi::HandleScope scope(ctx->env);
  auto arr = Napi::Array::New(ctx->env, num_params);
  for (uint32_t i = 0; i < num_params; ++i)
  {
    arr[i] = Napi::Number::New(ctx->env, params[i]);
  }
  auto result = ctx->factory_fn.Call({arr});
  if (!result.IsObject())
  {
    return 0;
  }
  auto obj = result.As<Napi::Object>();
  // Map snake_case keys (BacktestRunner.runCsv stats) into the C struct.
  auto getNum = [&](const char* key) -> double {
    auto v = obj.Get(key);
    return v.IsNumber() ? v.As<Napi::Number>().DoubleValue() : 0.0;
  };
  out_stats->totalTrades = static_cast<uint64_t>(getNum("totalTrades"));
  out_stats->winningTrades = static_cast<uint64_t>(getNum("winningTrades"));
  out_stats->losingTrades = static_cast<uint64_t>(getNum("losingTrades"));
  out_stats->initialCapital = getNum("initialCapital");
  out_stats->finalCapital = getNum("finalCapital");
  out_stats->totalPnl = getNum("netPnl");
  out_stats->netPnl = getNum("netPnl");
  out_stats->totalFees = getNum("totalFees");
  out_stats->returnPct = getNum("returnPct");
  out_stats->maxDrawdown = getNum("maxDrawdown");
  out_stats->maxDrawdownPct = getNum("maxDrawdownPct");
  out_stats->sharpeRatio = getNum("sharpeRatio");
  out_stats->sortinoRatio = getNum("sortinoRatio");
  out_stats->winRate = getNum("winRate");
  out_stats->profitFactor = getNum("profitFactor");
  (void)param_index;
  return 1;
}

class GridSearchNode : public Napi::ObjectWrap<GridSearchNode>
{
 public:
  static Napi::Object Init(Napi::Env env)
  {
    return DefineClass(env, "GridSearch",
                       {
                           InstanceMethod("addAxis", &GridSearchNode::addAxis),
                           InstanceMethod("setFactory", &GridSearchNode::setFactory),
                           InstanceMethod("total", &GridSearchNode::total),
                           InstanceMethod("paramsForIndex", &GridSearchNode::paramsForIndex),
                           InstanceMethod("run", &GridSearchNode::run),
                       });
  }

  GridSearchNode(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<GridSearchNode>(info)
  {
    _handle = flox_grid_search_create();
  }

  ~GridSearchNode() override
  {
    if (_handle)
    {
      flox_grid_search_destroy(_handle);
    }
  }

 private:
  FloxGridSearchHandle _handle{nullptr};
  Napi::FunctionReference _factory;

  Napi::Value addAxis(const Napi::CallbackInfo& info)
  {
    auto arr = info[0].As<Napi::Array>();
    std::vector<double> vals(arr.Length());
    for (uint32_t i = 0; i < arr.Length(); ++i)
    {
      vals[i] = arr.Get(i).As<Napi::Number>().DoubleValue();
    }
    flox_grid_search_add_axis(_handle, vals.data(),
                              static_cast<uint32_t>(vals.size()));
    return info.Env().Undefined();
  }

  Napi::Value setFactory(const Napi::CallbackInfo& info)
  {
    _factory = Napi::Persistent(info[0].As<Napi::Function>());
    return info.Env().Undefined();
  }

  Napi::Value total(const Napi::CallbackInfo& info)
  {
    return Napi::Number::New(info.Env(),
                             static_cast<double>(flox_grid_search_total(_handle)));
  }

  Napi::Value paramsForIndex(const Napi::CallbackInfo& info)
  {
    uint64_t idx = static_cast<uint64_t>(info[0].As<Napi::Number>().Int64Value());
    uint32_t total = flox_grid_search_params_for_index(_handle, idx, nullptr, 0);
    std::vector<double> out(total);
    if (total > 0)
    {
      flox_grid_search_params_for_index(_handle, idx, out.data(), total);
    }
    auto arr = Napi::Array::New(info.Env(), total);
    for (uint32_t i = 0; i < total; ++i)
    {
      arr[i] = Napi::Number::New(info.Env(), out[i]);
    }
    return arr;
  }

  Napi::Value run(const Napi::CallbackInfo& info)
  {
    return tryFlox(info.Env(), [&]() -> Napi::Value {
      auto env = info.Env();
      if (_factory.IsEmpty())
      {
        throw flox::FloxError(
            "E_RUN_001",
            "GridSearch.run called before setFactory.");
      }
      const uint64_t total = flox_grid_search_total(_handle);
      if (total == 0)
      {
        throw flox::FloxError(
            "E_RUN_002",
            "GridSearch.run called with no axes / empty axis.");
      }
      GridSearchCallContext ctx{env, Napi::FunctionReference(), {}};
      ctx.factory_fn = Napi::Persistent(_factory.Value());
      std::vector<FloxBacktestStats> stats(total);
      flox_grid_search_run(_handle, &gridSearchFactoryThunk, &ctx,
                           stats.data(), static_cast<uint32_t>(total));
      auto results = Napi::Array::New(env, total);
      for (uint64_t i = 0; i < total; ++i)
      {
        // Decode params back from index for the result tuple.
        uint32_t np = flox_grid_search_params_for_index(_handle, i, nullptr, 0);
        std::vector<double> p(np);
        if (np > 0)
        {
          flox_grid_search_params_for_index(_handle, i, p.data(), np);
        }
        auto pArr = Napi::Array::New(env, np);
        for (uint32_t j = 0; j < np; ++j)
        {
          pArr[j] = Napi::Number::New(env, p[j]);
        }
        auto o = Napi::Object::New(env);
        o.Set("index", Napi::Number::New(env, static_cast<double>(i)));
        o.Set("params", pArr);
        o.Set("stats", statsToJsObj(env, stats[i]));
        results[i] = o;
      }
      return results;
    });
  }
};

}  // namespace node_flox
