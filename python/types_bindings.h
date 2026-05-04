// python/types_bindings.h
//
// Shared Pythonic mirrors of small C-ABI structs. Used by both
// strategy_bindings.h and hook_bindings.h; broken out to avoid the
// circular include that would otherwise arise when hook trampolines
// receive PySignal / PyTradeData arguments.

#pragma once

#include "flox/capi/flox_capi.h"

#include <cstdint>
#include <string>

namespace
{

struct PyTradeData
{
  uint32_t symbol{0};
  std::string symbol_name;
  double price{0.0};
  double quantity{0.0};
  bool is_buy{false};
  std::string side;
  int64_t timestamp_ns{0};
  // For hook-side use (no symbol_name lookup).
  int64_t exchange_ts_ns{0};
};

struct PySignal
{
  uint64_t order_id{0};
  uint32_t symbol{0};
  std::string side;
  std::string order_type;
  double price{0.0};
  double quantity{0.0};
  double trigger_price{0.0};
  double trailing_offset{0.0};
  int32_t trailing_bps{0};
  double new_price{0.0};
  double new_quantity{0.0};
};

inline PySignal pySignalFromC(const FloxSignal* s)
{
  static constexpr const char* kOrderTypes[] = {
      "market", "limit", "stop_market", "stop_limit",
      "tp_market", "tp_limit", "trailing_stop",
      "cancel", "cancel_all", "modify"};
  PySignal ps{};
  ps.order_id = s->order_id;
  ps.symbol = s->symbol;
  ps.side = s->side == 0 ? "buy" : "sell";
  ps.order_type = s->order_type < 10 ? kOrderTypes[s->order_type] : "unknown";
  ps.price = s->price;
  ps.quantity = s->quantity;
  ps.trigger_price = s->trigger_price;
  ps.trailing_offset = s->trailing_offset;
  ps.trailing_bps = s->trailing_bps;
  ps.new_price = s->new_price;
  ps.new_quantity = s->new_quantity;
  return ps;
}

}  // namespace
