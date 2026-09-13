// node/src/bindings_common.h -- small helpers shared across the NAPI
// binding headers.
//
// `requireSameLength` used to live only in indicators.h, which most
// multi-array batch indicators there already used to close exactly this
// gap: an element count read from the FIRST array only, with every other
// array blindly indexed at that same length. aggregators.h and stats.h
// have the identical pattern (extractTrades reads `n` from `timestamps`
// alone; stat_bar_returns / stat_trade_pnl read `n` from `signal_long`
// alone) and need the same check, so this lives in its own header
// included ahead of all three in flox_node.cpp rather than duplicated or
// left order-dependent on which binding header happens to declare it.

#pragma once
#include <napi.h>

#include <initializer_list>
#include <string>

namespace node_flox
{

inline bool requireSameLength(Napi::Env env, const char* fnName,
                              std::initializer_list<size_t> lens)
{
  auto it = lens.begin();
  size_t n = *it;
  for (++it; it != lens.end(); ++it)
  {
    if (*it != n)
    {
      Napi::RangeError::New(env, std::string(fnName) + ": input arrays must have the same length")
          .ThrowAsJavaScriptException();
      return false;
    }
  }
  return true;
}

}  // namespace node_flox
