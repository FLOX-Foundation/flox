// flox_node -- Node.js native addon for Flox.

#include <napi.h>

#include "backtest.h"
#include "books.h"
#include "engine.h"
#include "indicators.h"
#include "positions.h"
#include "profiles.h"
#include "stats.h"

Napi::Object Init(Napi::Env env, Napi::Object exports)
{
  node_flox::registerEngine(env, exports);
  node_flox::registerIndicators(env, exports);
  node_flox::registerBacktest(env, exports);
  node_flox::registerBooks(env, exports);
  node_flox::registerPositions(env, exports);
  node_flox::registerProfiles(env, exports);
  node_flox::registerStats(env, exports);
  return exports;
}

NODE_API_MODULE(flox_node, Init)
