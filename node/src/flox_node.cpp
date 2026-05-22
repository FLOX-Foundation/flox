// flox_node -- Node.js native addon for Flox.

#include <napi.h>

#include "aggregators.h"
#include "backtest.h"
#include "bar_dispatch.h"
#include "books.h"
#include "data_ops.h"
#include "delta_book.h"
#include "engine.h"
#include "execution_algos.h"
#include "feed_clock.h"
#include "funding_schedule.h"
#include "graph.h"
#include "heatmap.h"
#include "hooks.h"
#include "indicators.h"
#include "latency.h"
#include "live_queue_position.h"
#include "order_group.h"
#include "portfolio_risk.h"
#include "positions.h"
#include "profiles.h"
#include "rate_limit.h"
#include "run_trace.h"
#include "stats.h"
#include "strategy.h"
#include "tape_aggregators.h"
#include "tape_diff.h"
#include "targets.h"

Napi::Object Init(Napi::Env env, Napi::Object exports)
{
  node_flox::registerEngine(env, exports);
  node_flox::registerIndicators(env, exports);
  node_flox::registerBacktest(env, exports);
  node_flox::registerBooks(env, exports);
  node_flox::registerPositions(env, exports);
  node_flox::registerProfiles(env, exports);
  node_flox::registerStats(env, exports);
  node_flox::registerAggregators(env, exports);
  node_flox::registerTapeAggregators(env, exports);
  node_flox::registerDataOps(env, exports);
  node_flox::registerStrategy(env, exports);
  node_flox::registerTargets(env, exports);
  node_flox::registerGraph(env, exports);
  node_flox::registerHeatmap(env, exports);
  node_flox::registerDeltaBook(env, exports);
  node_flox::registerExecutionAlgos(env, exports);
  node_flox::registerLatency(env, exports);
  node_flox::registerPortfolioRisk(env, exports);
  node_flox::registerOrderGroup(env, exports);
  node_flox::registerRateLimitPolicy(env, exports);
  node_flox::registerFundingSchedule(env, exports);
  node_flox::registerLiveQueuePositionEstimator(env, exports);
  node_flox::registerBarDispatchRecorder(env, exports);
  node_flox::registerFeedClock(env, exports);
  node_flox::registerTapeDiff(env, exports);
  node_flox::registerRunTrace(env, exports);
  flox_node::registerHooks(env, exports);
  return exports;
}

NODE_API_MODULE(flox_node, Init)
