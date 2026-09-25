/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/capi/flox_capi.h"
#include "flox/capi/bridge_strategy.h"
#include "flox/engine/symbol_registry.h"
#include "flox/log/abstract_logger.h"
#include "flox/log/log_stream.h"
#include "tracker_ownership.h"

#include "flox/aggregator/bar.h"
#include "flox/aggregator/policies/tick_bar_policy.h"
#include "flox/aggregator/policies/time_bar_policy.h"
#include "flox/aggregator/policies/volume_bar_policy.h"
#include "flox/backtest/backtest_config.h"
#include "flox/backtest/backtest_result.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/backtest/grid_search.h"
#include "flox/backtest/latency_model.h"
#include "flox/backtest/simulated_clock.h"
#include "flox/backtest/simulated_executor.h"
#include "flox/backtest/walk_forward.h"
#include "flox/book/events/book_update_event.h"
#include "flox/book/nlevel_order_book.h"
#include "flox/execution/algos.h"
#include "flox/replay/abstract_event_reader.h"
#include "flox/replay/aggregator.h"
#include "flox/replay/aggregators/bin_count.h"
#include "flox/replay/aggregators/book_snapshot_bin.h"
#include "flox/replay/aggregators/event_type_stats.h"
#include "flox/replay/aggregators/ohlc_bin.h"
#include "flox/replay/aggregators/peak.h"
#include "flox/replay/aggregators/quantile.h"
#include "flox/replay/aggregators/volume_bin.h"
#include "flox/replay/binary_format_v1.h"
#include "flox/replay/delta_book.h"
#include "flox/replay/ohlcv_replay_source.h"
#include "flox/replay/tape_diff.h"
#include "flox/report/heatmap_html.h"
#include "flox/risk/portfolio_risk.h"
#include "flox/run/trace_reader.h"
#include "flox/run/trace_recorder.h"
#include "flox/stats/whites_reality_check.h"

#include "flox/indicator/adf.h"
#include "flox/indicator/adx.h"
#include "flox/indicator/atr.h"
#include "flox/indicator/autocorrelation.h"
#include "flox/indicator/bollinger.h"
#include "flox/indicator/cci.h"
#include "flox/indicator/chop.h"
#include "flox/indicator/correlation.h"
#include "flox/indicator/cvd.h"
#include "flox/indicator/dema.h"
#include "flox/indicator/ema.h"
#include "flox/indicator/indicator_pipeline.h"
#include "flox/indicator/kama.h"
#include "flox/indicator/kurtosis.h"
#include "flox/indicator/macd.h"
#include "flox/indicator/obv.h"
#include "flox/indicator/parkinson_vol.h"
#include "flox/indicator/rma.h"
#include "flox/indicator/rogers_satchell_vol.h"
#include "flox/indicator/rolling_zscore.h"
#include "flox/indicator/rsi.h"
#include "flox/indicator/shannon_entropy.h"
#include "flox/indicator/skewness.h"
#include "flox/indicator/slope.h"
#include "flox/indicator/sma.h"
#include "flox/indicator/stochastic.h"
#include "flox/indicator/streaming_graph.h"
#include "flox/indicator/vwap.h"
#include "flox/target/future_ctc_volatility.h"
#include "flox/target/future_linear_slope.h"
#include "flox/target/future_return.h"

#include "flox/aggregator/bus/bar_bus.h"
#include "flox/aggregator/custom/footprint_bar.h"
#include "flox/aggregator/custom/market_profile.h"
#include "flox/aggregator/custom/volume_profile.h"
#include "flox/aggregator/policies/heikin_ashi_bar_policy.h"
#include "flox/aggregator/policies/range_bar_policy.h"
#include "flox/aggregator/policies/renko_bar_policy.h"
#include "flox/backtest/concentrated_liquidity_curve.h"
#include "flox/backtest/constant_product_curve.h"
#include "flox/backtest/ntoken_curve.h"
#include "flox/backtest/raydium_cp_curve.h"
#include "flox/book/bus/book_update_bus.h"
#include "flox/book/bus/trade_bus.h"
#include "flox/book/composite_book_matrix.h"
#include "flox/book/events/book_update_event.h"
#include "flox/book/events/trade_event.h"
#include "flox/book/l3/l3_order_book.h"
#include "flox/connector/amm_dex_connector.h"
#include "flox/execution/abstract_execution_listener.h"
#include "flox/execution/abstract_executor.h"
#include "flox/execution/exchange_capabilities.h"
#include "flox/execution/order.h"
#include "flox/execution/order_journey_tracer.h"
#include "flox/execution/order_tracker.h"
#include "flox/position/position_group.h"
#include "flox/position/position_tracker.h"
#include "flox/replay/binary_log_recorder_hook.h"
#include "flox/replay/merged_tape_reader.h"
#include "flox/replay/ops/partitioner.h"
#include "flox/replay/ops/segment_ops.h"
#include "flox/replay/ops/validator.h"
#include "flox/replay/pool_state_tape.h"
#include "flox/replay/readers/binary_log_reader.h"
#include "flox/replay/writers/binary_log_writer.h"
#include "flox/strategy/abstract_signal_handler.h"
#include "flox/testing/bar_dispatch_recorder.h"
#include "flox/util/int/i256.h"
#include "flox/util/int/u256.h"
#include "flox/util/memory/pool.h"

#include <random>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace flox;

// ============================================================
// C ABI entry contract
// ============================================================
//
// Every exported function opens with FLOX_CAPI_ENTER (or one of its
// variants) and closes with the matching FLOX_CAPI_LEAVE. The pair does two
// things a C caller cannot do for itself. Both are written down for callers
// in the "Calling contract" section at the top of flox_capi.h.
//
// A null handle returns the function's documented zero value. A foreign
// runtime holds a handle as an opaque integer, so passing one that was never
// created, or one a finaliser already zeroed, is a one-line mistake.
// Dereferencing it is undefined behaviour; returning zero is not. Before the
// contract was written down, 446 of the 523 handle-taking functions
// dereferenced without looking while their neighbours in the same layer
// quietly returned zero, and nothing said which did what.
//
// No exception leaves the boundary. Unwinding out of a frame with C linkage
// is undefined behaviour, and the friendly outcome -- std::terminate --
// takes down the host process with nothing the caller can do about it. One
// unparsable number in a user's CSV was enough to do that through the
// shipped Node binding. The firewall turns any exception into the same zero
// return and records what happened, which flox_last_error_code and
// flox_last_error_message hand back.
//
// `return {}` value-initialises whatever the function returns: zero for the
// integer and floating-point returns, NULL for handles and strings, an
// all-zero struct for the handful of by-value struct returns. Each of those
// is the value the function already documents as its failure result, so the
// guard adds a path callers already handle rather than a new one.

namespace
{

// Mirrors the codes documented on flox_last_error_code in flox_capi.h.
constexpr int kFloxCapiOk = 0;
constexpr int kFloxCapiErrNullHandle = 1;
constexpr int kFloxCapiErrException = 2;

struct FloxCapiLastError
{
  int code{kFloxCapiOk};
  std::string message;
};

FloxCapiLastError& floxCapiLastError() noexcept
{
  static thread_local FloxCapiLastError e;
  return e;
}

// Recording an error must never itself throw out of the guard, hence the
// swallow: a bad_alloc while building the message string would otherwise
// escape the very frame the guard exists to protect.
void floxCapiNoteNullHandle(const char* fn) noexcept
{
  try
  {
    auto& e = floxCapiLastError();
    e.code = kFloxCapiErrNullHandle;
    e.message = std::string(fn != nullptr ? fn : "?") + ": null handle";
  }
  catch (...)
  {
  }
}

void floxCapiNoteNullArgument(const char* fn, const char* argument) noexcept
{
  try
  {
    auto& e = floxCapiLastError();
    e.code = kFloxCapiErrNullHandle;
    e.message = std::string(fn != nullptr ? fn : "?") + ": argument '" +
                (argument != nullptr ? argument : "?") + "' must not be NULL";
  }
  catch (...)
  {
  }
}

void floxCapiNoteException(const char* fn, const char* what) noexcept
{
  try
  {
    auto& e = floxCapiLastError();
    e.code = kFloxCapiErrException;
    e.message = std::string(fn != nullptr ? fn : "?") + ": " +
                (what != nullptr ? what : "unknown exception");
  }
  catch (...)
  {
  }
}

// Accessors that reach inside a composite -- flox_venue_stack_account and its
// siblings -- hand back a pointer the composite still owns. A consumer cannot
// tell that apart from an owned handle, because both are void*, so a binding
// that wraps every handle it receives in a finaliser deleted the stack's
// member and the stack then deleted it again. The first delete looked healthy;
// the abort landed later, inside flox_venue_stack_destroy.
//
// Registering the borrowed addresses lets _destroy recognise and ignore them.
// Entries go in when the owning composite is created and come out when it is
// destroyed, so a registered address always belongs to a live object -- the
// address-reuse trap that made the portfolio risk scratch serve a dead
// session's breach does not apply here.
class FloxBorrowedHandles
{
 public:
  static void add(const void* p)
  {
    if (p == nullptr)
    {
      return;
    }
    const std::lock_guard<std::mutex> lock(mutex());
    registry().insert(p);
  }

  static void remove(const void* p)
  {
    if (p == nullptr)
    {
      return;
    }
    const std::lock_guard<std::mutex> lock(mutex());
    registry().erase(p);
  }

  static bool contains(const void* p)
  {
    if (p == nullptr)
    {
      return false;
    }
    const std::lock_guard<std::mutex> lock(mutex());
    return registry().find(p) != registry().end();
  }

 private:
  static std::mutex& mutex()
  {
    static std::mutex m;
    return m;
  }

  static std::set<const void*>& registry()
  {
    static std::set<const void*> r;
    return r;
  }
};

}  // namespace

// Every _destroy opens with this. Destroying NULL has always been legal and
// stays legal -- a binding's finaliser runs on a half-constructed object often
// enough that making it an error would be a downgrade -- and it is not
// recorded as an error. Destroying a borrowed handle is the same no-op: the
// composite that owns the object frees it.
//
// A plain `delete` would need neither check, but several _destroy bodies
// flush or close before deleting, and those do dereference.
// clang-format off
#define FLOX_CAPI_ENTER_DESTROY(handle)                                     \
  if ((handle) == nullptr || FloxBorrowedHandles::contains(handle))         \
  {                                                                         \
    return;                                                                 \
  }                                                                         \
  try                                                                       \
  {
// clang-format on

// clang-format off
#define FLOX_CAPI_ENTER(handle)                                             \
  if ((handle) == nullptr)                                                  \
  {                                                                         \
    floxCapiNoteNullHandle(__func__);                                       \
    return {};                                                              \
  }                                                                         \
  try                                                                       \
  {

#define FLOX_CAPI_ENTER_VOID(handle)                                        \
  if ((handle) == nullptr)                                                  \
  {                                                                         \
    floxCapiNoteNullHandle(__func__);                                       \
    return;                                                                 \
  }                                                                         \
  try                                                                       \
  {

#define FLOX_CAPI_ENTER_NOHANDLE                                            \
  try                                                                       \
  {

// A required non-handle argument: a path, a name, an output buffer. The entry
// guard covers handles only, so these say so one by one. Reported through
// flox_last_error_code the same way a null handle is.
#define FLOX_CAPI_REQUIRE(argument)                                         \
  if ((argument) == nullptr)                                                \
  {                                                                         \
    floxCapiNoteNullArgument(__func__, #argument);                          \
    return {};                                                              \
  }

#define FLOX_CAPI_REQUIRE_VOID(argument)                                    \
  if ((argument) == nullptr)                                                \
  {                                                                         \
    floxCapiNoteNullArgument(__func__, #argument);                          \
    return;                                                                 \
  }

#define FLOX_CAPI_LEAVE                                                     \
  }                                                                         \
  catch (const std::exception& floxCapiEx)                                  \
  {                                                                         \
    floxCapiNoteException(__func__, floxCapiEx.what());                     \
    return {};                                                              \
  }                                                                         \
  catch (...)                                                               \
  {                                                                         \
    floxCapiNoteException(__func__, nullptr);                               \
    return {};                                                              \
  }

#define FLOX_CAPI_LEAVE_VOID                                                \
  }                                                                         \
  catch (const std::exception& floxCapiEx)                                  \
  {                                                                         \
    floxCapiNoteException(__func__, floxCapiEx.what());                     \
    return;                                                                 \
  }                                                                         \
  catch (...)                                                               \
  {                                                                         \
    floxCapiNoteException(__func__, nullptr);                               \
    return;                                                                 \
  }
// clang-format on

// ============================================================
// Internal helpers
// ============================================================

static BridgeStrategy* toStrategy(FloxStrategyHandle h)
{
  return static_cast<BridgeStrategy*>(h);
}

static SymbolRegistry* toRegistry(FloxRegistryHandle h)
{
  return static_cast<SymbolRegistry*>(h);
}

// ============================================================
// Registry
// ============================================================

FloxRegistryHandle flox_registry_create(void)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return static_cast<FloxRegistryHandle>(new SymbolRegistry());
  FLOX_CAPI_LEAVE;
}

void flox_registry_destroy(FloxRegistryHandle registry)
{
  FLOX_CAPI_ENTER_DESTROY(registry);
  delete toRegistry(registry);
  FLOX_CAPI_LEAVE_VOID;
}

uint32_t flox_registry_add_symbol(FloxRegistryHandle registry, const char* exchange,
                                  const char* name, double tick_size)
{
  FLOX_CAPI_ENTER(registry);
  FLOX_CAPI_REQUIRE(exchange);
  FLOX_CAPI_REQUIRE(name);
  auto* reg = toRegistry(registry);
  SymbolInfo info;
  info.exchange = exchange;
  info.symbol = name;
  info.tickSize = Price::fromDouble(tick_size);
  return reg->registerSymbol(info);
  FLOX_CAPI_LEAVE;
}

uint8_t flox_registry_get_symbol_id(FloxRegistryHandle registry, const char* exchange,
                                    const char* name, uint32_t* id_out)
{
  FLOX_CAPI_ENTER(registry);
  FLOX_CAPI_REQUIRE(exchange);
  FLOX_CAPI_REQUIRE(name);
  FLOX_CAPI_REQUIRE(id_out);
  auto* reg = toRegistry(registry);
  auto result = reg->getSymbolId(exchange, name);
  if (result.has_value())
  {
    *id_out = result.value();
    return 1;
  }
  return 0;
  FLOX_CAPI_LEAVE;
}

uint8_t flox_registry_get_symbol_name(FloxRegistryHandle registry, uint32_t symbol_id,
                                      char* exchange_out, size_t exchange_len, char* name_out,
                                      size_t name_len)
{
  FLOX_CAPI_ENTER(registry);
  FLOX_CAPI_REQUIRE(exchange_out);
  FLOX_CAPI_REQUIRE(name_out);
  // A buffer the caller declared as holding nothing gets nothing written.
  // `exchange_len - 1` is unsigned, so zero wrapped to SIZE_MAX: strncpy
  // copied with no bound and the terminator landed at buf[SIZE_MAX], which
  // wraps to buf[-1]. Measured 136 zeroed bytes from buf-65 to buf+70, and
  // the call still reported success.
  if (exchange_len == 0 || name_len == 0)
  {
    return 0;
  }
  auto* reg = toRegistry(registry);
  auto info = reg->getSymbolInfo(symbol_id);
  if (!info.has_value())
  {
    return 0;
  }
  std::strncpy(exchange_out, info->exchange.c_str(), exchange_len - 1);
  exchange_out[exchange_len - 1] = '\0';
  std::strncpy(name_out, info->symbol.c_str(), name_len - 1);
  name_out[name_len - 1] = '\0';
  return 1;
  FLOX_CAPI_LEAVE;
}

uint32_t flox_registry_symbol_count(FloxRegistryHandle registry)
{
  FLOX_CAPI_ENTER(registry);
  return static_cast<uint32_t>(toRegistry(registry)->size());
  FLOX_CAPI_LEAVE;
}

// ============================================================
// Strategy lifecycle
// ============================================================

// Returns NULL if the strategy cannot be constructed. BridgeStrategy's base
// throws std::invalid_argument for a SymbolId absent from the registry -- an
// easy mistake to make from a foreign runtime, where the id is just an integer
// the caller made up. Letting that propagate out of extern "C" is undefined
// behaviour, so it becomes a NULL handle the caller can test.
FloxStrategyHandle flox_strategy_create(uint32_t id, const uint32_t* symbols,
                                        uint32_t num_symbols, FloxRegistryHandle registry,
                                        FloxStrategyCallbacks callbacks)
{
  FLOX_CAPI_ENTER(registry);
  try
  {
    auto* reg = toRegistry(registry);
    if (reg == nullptr || (num_symbols > 0 && symbols == nullptr))
    {
      return nullptr;
    }
    std::vector<SymbolId> syms(symbols, symbols + num_symbols);
    auto* strat =
        new BridgeStrategy(static_cast<SubscriberId>(id), std::move(syms), *reg, callbacks);
    return static_cast<FloxStrategyHandle>(strat);
  }
  catch (const std::exception&)
  {
    return nullptr;
  }
  FLOX_CAPI_LEAVE;
}

FloxStrategyHandle flox_strategy_create_p(uint32_t id, const uint32_t* symbols,
                                          uint32_t num_symbols, FloxRegistryHandle registry,
                                          const FloxStrategyCallbacks* callbacks)
{
  FLOX_CAPI_ENTER(registry);
  FloxStrategyCallbacks cbs = callbacks ? *callbacks : FloxStrategyCallbacks{};
  return flox_strategy_create(id, symbols, num_symbols, registry, cbs);
  FLOX_CAPI_LEAVE;
}

void flox_strategy_destroy(FloxStrategyHandle strategy)
{
  FLOX_CAPI_ENTER_DESTROY(strategy);
  delete toStrategy(strategy);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_strategy_replace_callbacks(FloxStrategyHandle strategy, FloxStrategyCallbacks callbacks)
{
  FLOX_CAPI_ENTER_VOID(strategy);
  if (!strategy)
  {
    return;
  }
  toStrategy(strategy)->replaceCallbacks(callbacks);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_strategy_replace_callbacks_p(FloxStrategyHandle strategy,
                                       const FloxStrategyCallbacks* callbacks)
{
  FLOX_CAPI_ENTER_VOID(strategy);
  FloxStrategyCallbacks cbs = callbacks ? *callbacks : FloxStrategyCallbacks{};
  flox_strategy_replace_callbacks(strategy, cbs);
  FLOX_CAPI_LEAVE_VOID;
}

// ============================================================
// Signal emission
// ============================================================

uint64_t flox_emit_market_buy(FloxStrategyHandle s, uint32_t symbol, int64_t qty_raw)
{
  FLOX_CAPI_ENTER(s);
  return toStrategy(s)->publicEmitMarketBuy(symbol, Quantity::fromRaw(qty_raw));
  FLOX_CAPI_LEAVE;
}

uint64_t flox_emit_market_sell(FloxStrategyHandle s, uint32_t symbol, int64_t qty_raw)
{
  FLOX_CAPI_ENTER(s);
  return toStrategy(s)->publicEmitMarketSell(symbol, Quantity::fromRaw(qty_raw));
  FLOX_CAPI_LEAVE;
}

uint64_t flox_emit_limit_buy(FloxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                             int64_t qty_raw)
{
  FLOX_CAPI_ENTER(s);
  return toStrategy(s)->publicEmitLimitBuy(symbol, Price::fromRaw(price_raw),
                                           Quantity::fromRaw(qty_raw));
  FLOX_CAPI_LEAVE;
}

uint64_t flox_emit_limit_sell(FloxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                              int64_t qty_raw)
{
  FLOX_CAPI_ENTER(s);
  return toStrategy(s)->publicEmitLimitSell(symbol, Price::fromRaw(price_raw),
                                            Quantity::fromRaw(qty_raw));
  FLOX_CAPI_LEAVE;
}

void flox_emit_cancel(FloxStrategyHandle s, uint64_t order_id)
{
  FLOX_CAPI_ENTER_VOID(s);
  toStrategy(s)->publicEmitCancel(order_id);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_emit_cancel_all(FloxStrategyHandle s, uint32_t symbol)
{
  FLOX_CAPI_ENTER_VOID(s);
  toStrategy(s)->publicEmitCancelAll(symbol);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_emit_modify(FloxStrategyHandle s, uint64_t order_id, int64_t new_price_raw,
                      int64_t new_qty_raw)
{
  FLOX_CAPI_ENTER_VOID(s);
  toStrategy(s)->publicEmitModify(order_id, Price::fromRaw(new_price_raw),
                                  Quantity::fromRaw(new_qty_raw));
  FLOX_CAPI_LEAVE_VOID;
}

uint64_t flox_emit_stop_market(FloxStrategyHandle s, uint32_t symbol, uint8_t side,
                               int64_t trigger_raw, int64_t qty_raw)
{
  FLOX_CAPI_ENTER(s);
  return toStrategy(s)->publicEmitStopMarket(symbol, side == 0 ? Side::BUY : Side::SELL,
                                             Price::fromRaw(trigger_raw),
                                             Quantity::fromRaw(qty_raw));
  FLOX_CAPI_LEAVE;
}

uint64_t flox_emit_stop_limit(FloxStrategyHandle s, uint32_t symbol, uint8_t side,
                              int64_t trigger_raw, int64_t limit_raw, int64_t qty_raw)
{
  FLOX_CAPI_ENTER(s);
  return toStrategy(s)->publicEmitStopLimit(symbol, side == 0 ? Side::BUY : Side::SELL,
                                            Price::fromRaw(trigger_raw),
                                            Price::fromRaw(limit_raw),
                                            Quantity::fromRaw(qty_raw));
  FLOX_CAPI_LEAVE;
}

uint64_t flox_emit_take_profit_market(FloxStrategyHandle s, uint32_t symbol, uint8_t side,
                                      int64_t trigger_raw, int64_t qty_raw)
{
  FLOX_CAPI_ENTER(s);
  return toStrategy(s)->publicEmitTakeProfitMarket(symbol, side == 0 ? Side::BUY : Side::SELL,
                                                   Price::fromRaw(trigger_raw),
                                                   Quantity::fromRaw(qty_raw));
  FLOX_CAPI_LEAVE;
}

uint64_t flox_emit_trailing_stop(FloxStrategyHandle s, uint32_t symbol, uint8_t side,
                                 int64_t offset_raw, int64_t qty_raw)
{
  FLOX_CAPI_ENTER(s);
  return toStrategy(s)->publicEmitTrailingStop(symbol, side == 0 ? Side::BUY : Side::SELL,
                                               Price::fromRaw(offset_raw),
                                               Quantity::fromRaw(qty_raw));
  FLOX_CAPI_LEAVE;
}

uint64_t flox_emit_trailing_stop_percent(FloxStrategyHandle s, uint32_t symbol, uint8_t side,
                                         int32_t callback_bps, int64_t qty_raw)
{
  FLOX_CAPI_ENTER(s);
  return toStrategy(s)->publicEmitTrailingStopPercent(
      symbol, side == 0 ? Side::BUY : Side::SELL, callback_bps, Quantity::fromRaw(qty_raw));
  FLOX_CAPI_LEAVE;
}

uint64_t flox_emit_take_profit_limit(FloxStrategyHandle s, uint32_t symbol, uint8_t side,
                                     int64_t trigger_raw, int64_t limit_raw, int64_t qty_raw)
{
  FLOX_CAPI_ENTER(s);
  return toStrategy(s)->publicEmitTakeProfitLimit(
      symbol, side == 0 ? Side::BUY : Side::SELL, Price::fromRaw(trigger_raw),
      Price::fromRaw(limit_raw), Quantity::fromRaw(qty_raw));
  FLOX_CAPI_LEAVE;
}

uint64_t flox_emit_limit_buy_tif(FloxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                                 int64_t qty_raw, uint8_t time_in_force)
{
  FLOX_CAPI_ENTER(s);
  return toStrategy(s)->publicEmitLimitBuyTif(
      symbol, Price::fromRaw(price_raw), Quantity::fromRaw(qty_raw),
      static_cast<TimeInForce>(time_in_force));
  FLOX_CAPI_LEAVE;
}

uint64_t flox_emit_limit_sell_tif(FloxStrategyHandle s, uint32_t symbol, int64_t price_raw,
                                  int64_t qty_raw, uint8_t time_in_force)
{
  FLOX_CAPI_ENTER(s);
  return toStrategy(s)->publicEmitLimitSellTif(
      symbol, Price::fromRaw(price_raw), Quantity::fromRaw(qty_raw),
      static_cast<TimeInForce>(time_in_force));
  FLOX_CAPI_LEAVE;
}

uint64_t flox_emit_close_position(FloxStrategyHandle s, uint32_t symbol)
{
  FLOX_CAPI_ENTER(s);
  return toStrategy(s)->publicEmitClosePosition(symbol);
  FLOX_CAPI_LEAVE;
}

int32_t flox_get_order_status(FloxStrategyHandle s, uint64_t order_id)
{
  FLOX_CAPI_ENTER(s);
  auto status = toStrategy(s)->getOrderStatus(order_id);
  return status ? static_cast<int32_t>(*status) : -1;
  FLOX_CAPI_LEAVE;
}

// ============================================================
// Context queries
// ============================================================

int64_t flox_position_raw(FloxStrategyHandle s, uint32_t symbol)
{
  FLOX_CAPI_ENTER(s);
  return toStrategy(s)->position(symbol).raw();
  FLOX_CAPI_LEAVE;
}

int64_t flox_last_trade_price_raw(FloxStrategyHandle s, uint32_t symbol)
{
  FLOX_CAPI_ENTER(s);
  return toStrategy(s)->ctx(symbol).lastTradePrice.raw();
  FLOX_CAPI_LEAVE;
}

int64_t flox_best_bid_raw(FloxStrategyHandle s, uint32_t symbol)
{
  FLOX_CAPI_ENTER(s);
  auto bid = toStrategy(s)->ctx(symbol).book.bestBid();
  return bid ? bid->raw() : 0;
  FLOX_CAPI_LEAVE;
}

int64_t flox_best_ask_raw(FloxStrategyHandle s, uint32_t symbol)
{
  FLOX_CAPI_ENTER(s);
  auto ask = toStrategy(s)->ctx(symbol).book.bestAsk();
  return ask ? ask->raw() : 0;
  FLOX_CAPI_LEAVE;
}

int64_t flox_mid_price_raw(FloxStrategyHandle s, uint32_t symbol)
{
  FLOX_CAPI_ENTER(s);
  auto mid = toStrategy(s)->ctx(symbol).mid();
  return mid ? mid->raw() : 0;
  FLOX_CAPI_LEAVE;
}

// The three above cannot separate "no quote" from a best quote of exactly 0.0:
// both come back as a raw of 0, and a book that quotes through zero reaches
// that price. These carry the presence flag in the return value instead.
uint8_t flox_best_bid_raw_opt(FloxStrategyHandle s, uint32_t symbol, int64_t* price_out)
{
  FLOX_CAPI_ENTER(s);
  auto bid = toStrategy(s)->ctx(symbol).book.bestBid();
  if (!bid)
  {
    return 0;
  }
  if (price_out)
  {
    *price_out = bid->raw();
  }
  return 1;
  FLOX_CAPI_LEAVE;
}

uint8_t flox_best_ask_raw_opt(FloxStrategyHandle s, uint32_t symbol, int64_t* price_out)
{
  FLOX_CAPI_ENTER(s);
  auto ask = toStrategy(s)->ctx(symbol).book.bestAsk();
  if (!ask)
  {
    return 0;
  }
  if (price_out)
  {
    *price_out = ask->raw();
  }
  return 1;
  FLOX_CAPI_LEAVE;
}

uint8_t flox_mid_price_raw_opt(FloxStrategyHandle s, uint32_t symbol, int64_t* price_out)
{
  FLOX_CAPI_ENTER(s);
  auto mid = toStrategy(s)->ctx(symbol).mid();
  if (!mid)
  {
    return 0;
  }
  if (price_out)
  {
    *price_out = mid->raw();
  }
  return 1;
  FLOX_CAPI_LEAVE;
}

void flox_get_symbol_context(FloxStrategyHandle s, uint32_t symbol, FloxSymbolContext* out)
{
  FLOX_CAPI_ENTER_VOID(s);
  const auto& c = toStrategy(s)->ctx(symbol);
  out->symbol_id = c.symbolId;
  out->position_raw = c.position.raw();
  out->has_avg_entry_price = c.avgEntryPrice ? 1u : 0u;
  out->avg_entry_price_raw = c.avgEntryPrice ? c.avgEntryPrice->raw() : 0;
  out->last_trade_price_raw = c.lastTradePrice.raw();
  out->last_update_ns = c.lastUpdateNs;

  auto bid = c.book.bestBid();
  auto ask = c.book.bestAsk();
  // The flags say whether a side has a best level; the price fields cannot,
  // since a book may be quoted at exactly zero.
  out->book.has_bid = bid ? 1u : 0u;
  out->book.has_ask = ask ? 1u : 0u;
  out->book.bid_price_raw = bid ? bid->raw() : 0;
  // See BridgeStrategy::toBookSnapshot: the size at the best level comes
  // from bidAtPrice()/askAtPrice(), and 0 is reserved for an empty side.
  out->book.bid_qty_raw = bid ? c.book.bidAtPrice(*bid).raw() : 0;
  out->book.ask_price_raw = ask ? ask->raw() : 0;
  out->book.ask_qty_raw = ask ? c.book.askAtPrice(*ask).raw() : 0;
  auto mid = c.mid();
  out->book.mid_raw = mid ? mid->raw() : 0;
  auto spread = c.bookSpread();
  out->book.spread_raw = spread ? spread->raw() : 0;
  FLOX_CAPI_LEAVE_VOID;
}

// ============================================================
// Fixed-point conversion
// ============================================================

int64_t flox_price_from_double(double value)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return Price::fromDouble(value).raw();
  FLOX_CAPI_LEAVE;
}

double flox_price_to_double(int64_t raw)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return Price::fromRaw(raw).toDouble();
  FLOX_CAPI_LEAVE;
}

int64_t flox_quantity_from_double(double value)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return Quantity::fromDouble(value).raw();
  FLOX_CAPI_LEAVE;
}

double flox_quantity_to_double(int64_t raw)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return Quantity::fromRaw(raw).toDouble();
  FLOX_CAPI_LEAVE;
}

// ============================================================
// Indicators
// ============================================================

void flox_indicator_ema(const double* input, size_t len, size_t period, double* output)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  indicator::EMA ema(period);
  ema.compute(std::span<const double>(input, len), std::span<double>(output, len));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_sma(const double* input, size_t len, size_t period, double* output)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  indicator::SMA sma(period);
  sma.compute(std::span<const double>(input, len), std::span<double>(output, len));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_rsi(const double* input, size_t len, size_t period, double* output)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  indicator::RSI rsi(period);
  rsi.compute(std::span<const double>(input, len), std::span<double>(output, len));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_atr(const double* high, const double* low, const double* close, size_t len,
                        size_t period, double* output)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  indicator::ATR atr(period);
  atr.compute(std::span<const double>(high, len), std::span<const double>(low, len),
              std::span<const double>(close, len), std::span<double>(output, len));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_macd(const double* input, size_t len, size_t fast_period, size_t slow_period,
                         size_t signal_period, double* macd_out, double* signal_out,
                         double* hist_out)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  indicator::MACD macd(fast_period, slow_period, signal_period);
  macd.compute(std::span<const double>(input, len), std::span<double>(macd_out, len),
               std::span<double>(signal_out, len), std::span<double>(hist_out, len));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_bollinger(const double* input, size_t len, size_t period, double multiplier,
                              double* upper, double* middle, double* lower)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  indicator::Bollinger bb(period, multiplier);
  bb.compute(std::span<const double>(input, len), std::span<double>(upper, len),
             std::span<double>(middle, len), std::span<double>(lower, len));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_rma(const double* input, size_t len, size_t period, double* output)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  indicator::RMA rma(period);
  rma.compute(std::span<const double>(input, len), std::span<double>(output, len));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_dema(const double* input, size_t len, size_t period, double* output)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  indicator::DEMA dema(period);
  auto result = dema.compute(std::span<const double>(input, len));
  std::copy(result.begin(), result.end(), output);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_tema(const double* input, size_t len, size_t period, double* output)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  indicator::TEMA tema(period);
  auto result = tema.compute(std::span<const double>(input, len));
  std::copy(result.begin(), result.end(), output);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_kama(const double* input, size_t len, size_t period, size_t fast, size_t slow,
                         double* output)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  indicator::KAMA kama(period, fast, slow);
  auto result = kama.compute(std::span<const double>(input, len));
  std::copy(result.begin(), result.end(), output);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_slope(const double* input, size_t len, size_t length, double* output)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  indicator::Slope slope(length);
  slope.compute(std::span<const double>(input, len), std::span<double>(output, len));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_adx(const double* high, const double* low, const double* close, size_t len,
                        size_t period, double* adx_out, double* plus_di_out, double* minus_di_out)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  indicator::ADX adx(period);
  auto result = adx.compute(std::span<const double>(high, len), std::span<const double>(low, len),
                            std::span<const double>(close, len));
  std::copy(result.adx.begin(), result.adx.end(), adx_out);
  std::copy(result.plus_di.begin(), result.plus_di.end(), plus_di_out);
  std::copy(result.minus_di.begin(), result.minus_di.end(), minus_di_out);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_cci(const double* high, const double* low, const double* close, size_t len,
                        size_t period, double* output)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  indicator::CCI cci(period);
  auto result = cci.compute(std::span<const double>(high, len), std::span<const double>(low, len),
                            std::span<const double>(close, len));
  std::copy(result.begin(), result.end(), output);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_stochastic(const double* high, const double* low, const double* close,
                               size_t len, size_t k_period, size_t d_period, double* k_out,
                               double* d_out)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  indicator::Stochastic stoch(k_period, d_period);
  auto result =
      stoch.compute(std::span<const double>(high, len), std::span<const double>(low, len),
                    std::span<const double>(close, len));
  std::copy(result.k.begin(), result.k.end(), k_out);
  std::copy(result.d.begin(), result.d.end(), d_out);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_chop(const double* high, const double* low, const double* close, size_t len,
                         size_t period, double* output)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  indicator::CHOP chop(period);
  auto result = chop.compute(std::span<const double>(high, len), std::span<const double>(low, len),
                             std::span<const double>(close, len));
  std::copy(result.begin(), result.end(), output);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_obv(const double* close, const double* volume, size_t len, double* output)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  indicator::OBV obv;
  obv.compute(std::span<const double>(close, len), std::span<const double>(volume, len),
              std::span<double>(output, len));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_vwap(const double* close, const double* volume, size_t len, size_t window,
                         double* output)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  indicator::VWAP vwap(window);
  auto result =
      vwap.compute(std::span<const double>(close, len), std::span<const double>(volume, len));
  std::copy(result.begin(), result.end(), output);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_cvd(const double* open, const double* high, const double* low,
                        const double* close, const double* volume, size_t len, double* output)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  indicator::CVD cvd;
  auto result = cvd.compute(
      std::span<const double>(open, len), std::span<const double>(high, len),
      std::span<const double>(low, len), std::span<const double>(close, len),
      std::span<const double>(volume, len));
  std::copy(result.begin(), result.end(), output);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_skewness(const double* input, size_t len, size_t period, double* output)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  indicator::Skewness ind(period);
  ind.compute(std::span<const double>(input, len), std::span<double>(output, len));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_kurtosis(const double* input, size_t len, size_t period, double* output)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  indicator::Kurtosis ind(period);
  ind.compute(std::span<const double>(input, len), std::span<double>(output, len));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_parkinson_vol(const double* high, const double* low, size_t len, size_t period,
                                  double* output)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  indicator::ParkinsonVol ind(period);
  ind.compute(std::span<const double>(high, len), std::span<const double>(low, len),
              std::span<double>(output, len));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_rogers_satchell_vol(const double* open, const double* high, const double* low,
                                        const double* close, size_t len, size_t period,
                                        double* output)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  indicator::RogersSatchellVol ind(period);
  ind.compute(std::span<const double>(open, len), std::span<const double>(high, len),
              std::span<const double>(low, len), std::span<const double>(close, len),
              std::span<double>(output, len));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_rolling_zscore(const double* input, size_t len, size_t period, double* output)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  indicator::RollingZScore ind(period);
  ind.compute(std::span<const double>(input, len), std::span<double>(output, len));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_shannon_entropy(const double* input, size_t len, size_t period, size_t bins,
                                    double* output)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  indicator::ShannonEntropy ind(period, bins);
  ind.compute(std::span<const double>(input, len), std::span<double>(output, len));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_correlation(const double* x, const double* y, size_t len, size_t period,
                                double* output)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  indicator::Correlation ind(period);
  ind.compute(std::span<const double>(x, len), std::span<const double>(y, len),
              std::span<double>(output, len));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_adf(const double* input, size_t len, size_t max_lag, const char* regression,
                        double* test_stat_out, double* p_value_out, size_t* used_lag_out)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  std::string reg = regression ? regression : "c";
  auto r = indicator::adf(std::span<const double>(input, len), max_lag, reg);
  if (test_stat_out)
  {
    *test_stat_out = r.test_stat;
  }
  if (p_value_out)
  {
    *p_value_out = r.p_value;
  }
  if (used_lag_out)
  {
    *used_lag_out = r.used_lag;
  }
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_autocorrelation(const double* input, size_t len, size_t window, size_t lag,
                                    double* output)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  indicator::AutoCorrelation ind(window, lag);
  ind.compute(std::span<const double>(input, len), std::span<double>(output, len));
  FLOX_CAPI_LEAVE_VOID;
}

// ============================================================
// Targets
// ============================================================

void flox_target_future_return(const double* close, size_t len, size_t horizon, double* output)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  target::FutureReturn t(horizon);
  t.compute(std::span<const double>(close, len), std::span<double>(output, len));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_target_future_ctc_volatility(const double* close, size_t len, size_t horizon,
                                       double* output)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  target::FutureCTCVolatility t(horizon);
  t.compute(std::span<const double>(close, len), std::span<double>(output, len));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_target_future_linear_slope(const double* close, size_t len, size_t horizon,
                                     double* output)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  target::FutureLinearSlope t(horizon);
  t.compute(std::span<const double>(close, len), std::span<double>(output, len));
  FLOX_CAPI_LEAVE_VOID;
}

// ============================================================
// IndicatorGraph (batch) — handle-based wrapper.
// ============================================================

namespace
{

struct FloxGraphImpl
{
  indicator::IndicatorGraph graph;
  std::unordered_map<uint32_t, std::vector<Bar>> barStorage;
};

}  // namespace

FloxIndicatorGraphHandle flox_indicator_graph_create(void)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return new FloxGraphImpl();
  FLOX_CAPI_LEAVE;
}

void flox_indicator_graph_destroy(FloxIndicatorGraphHandle g)
{
  FLOX_CAPI_ENTER_DESTROY(g);
  delete static_cast<FloxGraphImpl*>(g);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_graph_set_bars(FloxIndicatorGraphHandle g, uint32_t symbol,
                                   const double* close, const double* high, const double* low,
                                   const double* volume, size_t len)
{
  FLOX_CAPI_ENTER_VOID(g);
  auto* impl = static_cast<FloxGraphImpl*>(g);
  std::vector<Bar> bars(len);
  for (size_t i = 0; i < len; ++i)
  {
    bars[i].open = Price::fromDouble(close[i]);
    bars[i].high = Price::fromDouble(high ? high[i] : close[i]);
    bars[i].low = Price::fromDouble(low ? low[i] : close[i]);
    bars[i].close = Price::fromDouble(close[i]);
    bars[i].volume = volume ? Volume::fromDouble(volume[i]) : Volume{};
  }
  impl->barStorage[symbol] = std::move(bars);
  impl->graph.setBars(static_cast<SymbolId>(symbol),
                      std::span<const Bar>(impl->barStorage[symbol]));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_graph_add_node(FloxIndicatorGraphHandle g, const char* name,
                                   const char* const* deps, size_t num_deps,
                                   FloxGraphNodeFn fn, void* user_data)
{
  FLOX_CAPI_ENTER_VOID(g);
  auto* impl = static_cast<FloxGraphImpl*>(g);
  std::vector<std::string> depList;
  depList.reserve(num_deps);
  for (size_t i = 0; i < num_deps; ++i)
  {
    depList.emplace_back(deps[i]);
  }
  impl->graph.addNode(name, std::move(depList),
                      [g, fn, user_data](indicator::IndicatorGraph&, SymbolId sym)
                      {
                        size_t outLen = 0;
                        const double* p = fn(user_data, g, static_cast<uint32_t>(sym), &outLen);
                        if (!p)
                        {
                          return std::vector<double>{};
                        }
                        return std::vector<double>(p, p + outLen);
                      });
  FLOX_CAPI_LEAVE_VOID;
}

const double* flox_indicator_graph_require(FloxIndicatorGraphHandle g, uint32_t symbol,
                                           const char* name, size_t* len_out)
{
  FLOX_CAPI_ENTER(g);
  auto* impl = static_cast<FloxGraphImpl*>(g);
  try
  {
    const auto& v = impl->graph.require(static_cast<SymbolId>(symbol), name);
    if (len_out)
    {
      *len_out = v.size();
    }
    return v.data();
  }
  catch (...)
  {
    if (len_out)
    {
      *len_out = 0;
    }
    return nullptr;
  }
  FLOX_CAPI_LEAVE;
}

const double* flox_indicator_graph_get(FloxIndicatorGraphHandle g, uint32_t symbol,
                                       const char* name, size_t* len_out)
{
  FLOX_CAPI_ENTER(g);
  auto* impl = static_cast<FloxGraphImpl*>(g);
  const auto* v = impl->graph.get(static_cast<SymbolId>(symbol), name);
  if (!v)
  {
    if (len_out)
    {
      *len_out = 0;
    }
    return nullptr;
  }
  if (len_out)
  {
    *len_out = v->size();
  }
  return v->data();
  FLOX_CAPI_LEAVE;
}

const double* flox_indicator_graph_close(FloxIndicatorGraphHandle g, uint32_t symbol,
                                         size_t* len_out)
{
  FLOX_CAPI_ENTER(g);
  auto* impl = static_cast<FloxGraphImpl*>(g);
  const auto& v = impl->graph.close(static_cast<SymbolId>(symbol));
  if (len_out)
  {
    *len_out = v.size();
  }
  return v.data();
  FLOX_CAPI_LEAVE;
}

const double* flox_indicator_graph_high(FloxIndicatorGraphHandle g, uint32_t symbol,
                                        size_t* len_out)
{
  FLOX_CAPI_ENTER(g);
  auto* impl = static_cast<FloxGraphImpl*>(g);
  const auto& v = impl->graph.high(static_cast<SymbolId>(symbol));
  if (len_out)
  {
    *len_out = v.size();
  }
  return v.data();
  FLOX_CAPI_LEAVE;
}

const double* flox_indicator_graph_low(FloxIndicatorGraphHandle g, uint32_t symbol,
                                       size_t* len_out)
{
  FLOX_CAPI_ENTER(g);
  auto* impl = static_cast<FloxGraphImpl*>(g);
  const auto& v = impl->graph.low(static_cast<SymbolId>(symbol));
  if (len_out)
  {
    *len_out = v.size();
  }
  return v.data();
  FLOX_CAPI_LEAVE;
}

const double* flox_indicator_graph_volume(FloxIndicatorGraphHandle g, uint32_t symbol,
                                          size_t* len_out)
{
  FLOX_CAPI_ENTER(g);
  auto* impl = static_cast<FloxGraphImpl*>(g);
  const auto& v = impl->graph.volume(static_cast<SymbolId>(symbol));
  if (len_out)
  {
    *len_out = v.size();
  }
  return v.data();
  FLOX_CAPI_LEAVE;
}

void flox_indicator_graph_invalidate(FloxIndicatorGraphHandle g, uint32_t symbol)
{
  FLOX_CAPI_ENTER_VOID(g);
  static_cast<FloxGraphImpl*>(g)->graph.invalidate(static_cast<SymbolId>(symbol));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_graph_invalidate_all(FloxIndicatorGraphHandle g)
{
  FLOX_CAPI_ENTER_VOID(g);
  static_cast<FloxGraphImpl*>(g)->graph.invalidateAll();
  FLOX_CAPI_LEAVE_VOID;
}

// ============================================================
// IndicatorGraph — streaming methods on the same handle.
// ============================================================

void flox_indicator_graph_step(FloxIndicatorGraphHandle g, uint32_t symbol, double open,
                               double high, double low, double close, double volume)
{
  FLOX_CAPI_ENTER_VOID(g);
  auto* impl = static_cast<FloxGraphImpl*>(g);
  Bar bar;
  bar.open = Price::fromDouble(open);
  bar.high = Price::fromDouble(high);
  bar.low = Price::fromDouble(low);
  bar.close = Price::fromDouble(close);
  bar.volume = Volume::fromDouble(volume);
  impl->graph.step(static_cast<SymbolId>(symbol), bar);
  FLOX_CAPI_LEAVE_VOID;
}

double flox_indicator_graph_current(FloxIndicatorGraphHandle g, uint32_t symbol, const char* name)
{
  FLOX_CAPI_ENTER(g);
  return static_cast<FloxGraphImpl*>(g)->graph.current(static_cast<SymbolId>(symbol), name);
  FLOX_CAPI_LEAVE;
}

uint32_t flox_indicator_graph_bar_count(FloxIndicatorGraphHandle g, uint32_t symbol)
{
  FLOX_CAPI_ENTER(g);
  return static_cast<uint32_t>(
      static_cast<FloxGraphImpl*>(g)->graph.barCount(static_cast<SymbolId>(symbol)));
  FLOX_CAPI_LEAVE;
}

void flox_indicator_graph_reset(FloxIndicatorGraphHandle g, uint32_t symbol)
{
  FLOX_CAPI_ENTER_VOID(g);
  static_cast<FloxGraphImpl*>(g)->graph.reset(static_cast<SymbolId>(symbol));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_indicator_graph_reset_all(FloxIndicatorGraphHandle g)
{
  FLOX_CAPI_ENTER_VOID(g);
  static_cast<FloxGraphImpl*>(g)->graph.resetAll();
  FLOX_CAPI_LEAVE_VOID;
}

// ── Deprecated streaming-graph shim — forwards to the unified API ──

FloxStreamingGraphHandle flox_streaming_graph_create(void)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return flox_indicator_graph_create();
  FLOX_CAPI_LEAVE;
}

void flox_streaming_graph_destroy(FloxStreamingGraphHandle sg)
{
  FLOX_CAPI_ENTER_DESTROY(sg);
  flox_indicator_graph_destroy(sg);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_streaming_graph_add_node(FloxStreamingGraphHandle sg, const char* name,
                                   const char* const* deps, size_t num_deps, FloxGraphNodeFn fn,
                                   void* user_data)
{
  FLOX_CAPI_ENTER_VOID(sg);
  flox_indicator_graph_add_node(sg, name, deps, num_deps, fn, user_data);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_streaming_graph_step(FloxStreamingGraphHandle sg, uint32_t symbol, double open,
                               double high, double low, double close, double volume)
{
  FLOX_CAPI_ENTER_VOID(sg);
  flox_indicator_graph_step(sg, symbol, open, high, low, close, volume);
  FLOX_CAPI_LEAVE_VOID;
}

double flox_streaming_graph_current(FloxStreamingGraphHandle sg, uint32_t symbol, const char* name)
{
  FLOX_CAPI_ENTER(sg);
  return flox_indicator_graph_current(sg, symbol, name);
  FLOX_CAPI_LEAVE;
}

uint32_t flox_streaming_graph_bar_count(FloxStreamingGraphHandle sg, uint32_t symbol)
{
  FLOX_CAPI_ENTER(sg);
  return flox_indicator_graph_bar_count(sg, symbol);
  FLOX_CAPI_LEAVE;
}

void flox_streaming_graph_reset(FloxStreamingGraphHandle sg, uint32_t symbol)
{
  FLOX_CAPI_ENTER_VOID(sg);
  flox_indicator_graph_reset(sg, symbol);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_streaming_graph_reset_all(FloxStreamingGraphHandle sg)
{
  FLOX_CAPI_ENTER_VOID(sg);
  flox_indicator_graph_reset_all(sg);
  FLOX_CAPI_LEAVE_VOID;
}

const double* flox_streaming_graph_close(FloxStreamingGraphHandle sg, uint32_t symbol,
                                         size_t* len_out)
{
  FLOX_CAPI_ENTER(sg);
  return flox_indicator_graph_close(sg, symbol, len_out);
  FLOX_CAPI_LEAVE;
}
const double* flox_streaming_graph_high(FloxStreamingGraphHandle sg, uint32_t symbol,
                                        size_t* len_out)
{
  FLOX_CAPI_ENTER(sg);
  return flox_indicator_graph_high(sg, symbol, len_out);
  FLOX_CAPI_LEAVE;
}
const double* flox_streaming_graph_low(FloxStreamingGraphHandle sg, uint32_t symbol,
                                       size_t* len_out)
{
  FLOX_CAPI_ENTER(sg);
  return flox_indicator_graph_low(sg, symbol, len_out);
  FLOX_CAPI_LEAVE;
}
const double* flox_streaming_graph_volume(FloxStreamingGraphHandle sg, uint32_t symbol,
                                          size_t* len_out)
{
  FLOX_CAPI_ENTER(sg);
  return flox_indicator_graph_volume(sg, symbol, len_out);
  FLOX_CAPI_LEAVE;
}

// ============================================================
// Order book
// ============================================================

struct FloxBookImpl
{
  NLevelOrderBook<8192> book;
  explicit FloxBookImpl(double tickSize) : book(Price::fromDouble(tickSize)) {}

  void applyUpdate(const double* bp, const double* bq, size_t bl, const double* ap,
                   const double* aq, size_t al, BookUpdateType type)
  {
    std::byte buf[32768];
    std::pmr::monotonic_buffer_resource res(buf, sizeof(buf));
    BookUpdateEvent ev(&res);
    ev.update.type = type;
    ev.update.bids.reserve(bl);
    for (size_t i = 0; i < bl; ++i)
    {
      ev.update.bids.push_back({Price::fromDouble(bp[i]), Quantity::fromDouble(bq[i])});
    }
    ev.update.asks.reserve(al);
    for (size_t i = 0; i < al; ++i)
    {
      ev.update.asks.push_back({Price::fromDouble(ap[i]), Quantity::fromDouble(aq[i])});
    }
    book.applyBookUpdate(ev);
  }
};

FloxBookHandle flox_book_create(double tick_size)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  try
  {
    return new FloxBookImpl(tick_size);
  }
  catch (...)
  {
    // A non-positive tick size is rejected by the book itself. Report it the
    // way the rest of this ABI reports a refused construction, and never let
    // the exception cross back into C.
    return nullptr;
  }
  FLOX_CAPI_LEAVE;
}

void flox_book_destroy(FloxBookHandle book)
{
  FLOX_CAPI_ENTER_DESTROY(book);
  delete static_cast<FloxBookImpl*>(book);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_book_apply_snapshot(FloxBookHandle h, const double* bp, const double* bq, size_t bl,
                              const double* ap, const double* aq, size_t al)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxBookImpl*>(h)->applyUpdate(bp, bq, bl, ap, aq, al, BookUpdateType::SNAPSHOT);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_book_apply_delta(FloxBookHandle h, const double* bp, const double* bq, size_t bl,
                           const double* ap, const double* aq, size_t al)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxBookImpl*>(h)->applyUpdate(bp, bq, bl, ap, aq, al, BookUpdateType::DELTA);
  FLOX_CAPI_LEAVE_VOID;
}

uint8_t flox_book_best_bid(FloxBookHandle h, double* price_out)
{
  FLOX_CAPI_ENTER(h);
  auto bid = static_cast<FloxBookImpl*>(h)->book.bestBid();
  if (bid)
  {
    *price_out = bid->toDouble();
    return 1;
  }
  return 0;
  FLOX_CAPI_LEAVE;
}

uint8_t flox_book_best_ask(FloxBookHandle h, double* price_out)
{
  FLOX_CAPI_ENTER(h);
  auto ask = static_cast<FloxBookImpl*>(h)->book.bestAsk();
  if (ask)
  {
    *price_out = ask->toDouble();
    return 1;
  }
  return 0;
  FLOX_CAPI_LEAVE;
}

uint8_t flox_book_mid(FloxBookHandle h, double* price_out)
{
  FLOX_CAPI_ENTER(h);
  auto& book = static_cast<FloxBookImpl*>(h)->book;
  auto mid = book.mid();
  if (mid)
  {
    *price_out = mid->toDouble();
    return 1;
  }
  return 0;
  FLOX_CAPI_LEAVE;
}

uint8_t flox_book_spread(FloxBookHandle h, double* spread_out)
{
  FLOX_CAPI_ENTER(h);
  auto& book = static_cast<FloxBookImpl*>(h)->book;
  auto sp = book.spread();
  if (sp)
  {
    *spread_out = sp->toDouble();
    return 1;
  }
  return 0;
  FLOX_CAPI_LEAVE;
}

double flox_book_bid_at_price(FloxBookHandle h, double price)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<FloxBookImpl*>(h)->book.bidAtPrice(Price::fromDouble(price)).toDouble();
  FLOX_CAPI_LEAVE;
}

double flox_book_ask_at_price(FloxBookHandle h, double price)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<FloxBookImpl*>(h)->book.askAtPrice(Price::fromDouble(price)).toDouble();
  FLOX_CAPI_LEAVE;
}

uint8_t flox_book_is_crossed(FloxBookHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<FloxBookImpl*>(h)->book.isCrossed() ? 1 : 0;
  FLOX_CAPI_LEAVE;
}

void flox_book_clear(FloxBookHandle h)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxBookImpl*>(h)->book.clear();
  FLOX_CAPI_LEAVE_VOID;
}

// ============================================================
// Simulated executor
// ============================================================

// A FloxSimulatedExecutorHandle points at this wrapper, never at the
// SimulatedExecutor itself: every flox_simulated_executor_* function reaches
// `executor` through it, and the engine object sits at a non-zero offset
// inside. Handing out the engine object instead read every field sixteen
// bytes off -- fill counts came back as billions, advancing the clock
// overwrote the executor's own header, and submitting an order crashed.
//
// The members are references so a wrapper can either own the pair or borrow
// one that a VenueStack owns, without every call site having to know which.
struct FloxSimulatedExecutorImpl
{
 private:
  // Declared first so they are constructed before the references bind.
  std::unique_ptr<SimulatedClock> _ownedClock;
  std::unique_ptr<SimulatedExecutor> _ownedExecutor;

 public:
  SimulatedClock& clock;
  SimulatedExecutor& executor;

  FloxSimulatedExecutorImpl()
      : _ownedClock(std::make_unique<SimulatedClock>()),
        _ownedExecutor(std::make_unique<SimulatedExecutor>(*_ownedClock)),
        clock(*_ownedClock),
        executor(*_ownedExecutor)
  {
  }

  FloxSimulatedExecutorImpl(SimulatedClock& borrowedClock, SimulatedExecutor& borrowedExecutor)
      : clock(borrowedClock), executor(borrowedExecutor)
  {
  }
};

FloxSimulatedExecutorHandle flox_simulated_executor_create(void)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return new FloxSimulatedExecutorImpl();
  FLOX_CAPI_LEAVE;
}

void flox_simulated_executor_destroy(FloxSimulatedExecutorHandle executor)
{
  FLOX_CAPI_ENTER_DESTROY(executor);
  delete static_cast<FloxSimulatedExecutorImpl*>(executor);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_submit_order(FloxSimulatedExecutorHandle h, uint64_t id, uint8_t side, double price,
                                          double quantity, uint8_t order_type, uint32_t symbol)
{
  FLOX_CAPI_ENTER_VOID(h);
  auto* impl = static_cast<FloxSimulatedExecutorImpl*>(h);
  Order order{};
  order.id = id;
  order.side = side == 0 ? Side::BUY : Side::SELL;
  order.price = Price::fromDouble(price);
  order.quantity = Quantity::fromDouble(quantity);
  order.type = static_cast<OrderType>(order_type);
  order.symbol = symbol;
  impl->executor.submitOrder(order);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_submit_order_ex(FloxSimulatedExecutorHandle h, uint64_t id,
                                             uint8_t side, double price, double quantity,
                                             uint8_t order_type, uint32_t symbol,
                                             uint8_t tif, uint8_t reduce_only,
                                             int64_t expires_at_ns)
{
  FLOX_CAPI_ENTER_VOID(h);
  auto* impl = static_cast<FloxSimulatedExecutorImpl*>(h);
  Order order{};
  order.id = id;
  order.side = side == 0 ? Side::BUY : Side::SELL;
  order.price = Price::fromDouble(price);
  order.quantity = Quantity::fromDouble(quantity);
  order.type = static_cast<OrderType>(order_type);
  order.symbol = symbol;
  order.timeInForce = static_cast<TimeInForce>(tif);
  order.flags.reduceOnly = reduce_only ? 1 : 0;
  if (expires_at_ns > 0)
  {
    order.expiresAfter = TimePoint(std::chrono::nanoseconds(expires_at_ns));
  }
  impl->executor.submitOrder(order);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_submit_bracket(FloxSimulatedExecutorHandle h, uint64_t bracket_id,
                                            uint32_t symbol, uint8_t entry_side,
                                            uint8_t entry_type, double entry_price,
                                            double quantity, uint8_t tp_side, uint8_t tp_type,
                                            double tp_price, uint8_t stop_side,
                                            uint8_t stop_type, double stop_trigger_price)
{
  FLOX_CAPI_ENTER_VOID(h);
  BracketOrder b;
  b.bracketId = bracket_id;
  b.symbol = symbol;
  b.entry.side = entry_side == 0 ? Side::BUY : Side::SELL;
  b.entry.type = static_cast<OrderType>(entry_type);
  b.entry.price = Price::fromDouble(entry_price);
  b.entry.quantity = Quantity::fromDouble(quantity);
  b.takeProfit.side = tp_side == 0 ? Side::BUY : Side::SELL;
  b.takeProfit.type = static_cast<OrderType>(tp_type);
  b.takeProfit.price = Price::fromDouble(tp_price);
  b.takeProfit.quantity = Quantity::fromDouble(quantity);
  b.stop.side = stop_side == 0 ? Side::BUY : Side::SELL;
  b.stop.type = static_cast<OrderType>(stop_type);
  b.stop.triggerPrice = Price::fromDouble(stop_trigger_price);
  b.stop.quantity = Quantity::fromDouble(quantity);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.submitBracket(b);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_cancel_bracket(FloxSimulatedExecutorHandle h, uint64_t bracket_id)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.cancelBracket(bracket_id);
  FLOX_CAPI_LEAVE_VOID;
}

uint8_t flox_simulated_executor_bracket_state(FloxSimulatedExecutorHandle h, uint64_t bracket_id)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint8_t>(
      static_cast<FloxSimulatedExecutorImpl*>(h)->executor.bracketStatus(bracket_id).state);
  FLOX_CAPI_LEAVE;
}

void flox_simulated_executor_set_bracket_child_arm_mode(FloxSimulatedExecutorHandle h,
                                                        uint8_t mode)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setBracketChildArmMode(
      mode == 1 ? SimulatedExecutor::BracketArmMode::OnPartialFill
                : SimulatedExecutor::BracketArmMode::OnFullFill);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_submit_iceberg(FloxSimulatedExecutorHandle h, uint64_t id,
                                            uint8_t side, double price,
                                            double total_quantity, double visible_quantity,
                                            uint32_t symbol)
{
  FLOX_CAPI_ENTER_VOID(h);
  Order order{};
  order.id = id;
  order.side = side == 0 ? Side::BUY : Side::SELL;
  order.price = Price::fromDouble(price);
  order.quantity = Quantity::fromDouble(total_quantity);
  order.visibleQuantity = Quantity::fromDouble(visible_quantity);
  order.type = OrderType::ICEBERG;
  order.symbol = symbol;
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.submitOrder(order);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_set_iceberg_refresh_latency(FloxSimulatedExecutorHandle h,
                                                         int64_t latency_ns)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setIcebergRefreshLatency(latency_ns);
  FLOX_CAPI_LEAVE_VOID;
}

int64_t flox_simulated_executor_iceberg_hidden_remaining_raw(FloxSimulatedExecutorHandle h,
                                                             uint64_t id)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<FloxSimulatedExecutorImpl*>(h)->executor.icebergHiddenRemainingRaw(id);
  FLOX_CAPI_LEAVE;
}

void flox_simulated_executor_set_iceberg_size_randomisation_pct(
    FloxSimulatedExecutorHandle h, double pct)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setIcebergSizeRandomisationPct(pct);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_set_iceberg_priority_mode(FloxSimulatedExecutorHandle h,
                                                       uint8_t mode)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setIcebergPriorityMode(
      mode == 1 ? SimulatedExecutor::IcebergPriorityMode::Retain
                : SimulatedExecutor::IcebergPriorityMode::Back);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_set_iceberg_jitter_seed(FloxSimulatedExecutorHandle h,
                                                     uint64_t seed)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setIcebergJitterSeed(seed);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_cancel_order(FloxSimulatedExecutorHandle h, uint64_t order_id)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.cancelOrder(order_id);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_cancel_all(FloxSimulatedExecutorHandle h, uint32_t symbol)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.cancelAllOrders(symbol);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_on_bar(FloxSimulatedExecutorHandle h, uint32_t symbol, double close_price)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.onBar(symbol, Price::fromDouble(close_price));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_on_bar_ohlc(FloxSimulatedExecutorHandle h, uint32_t symbol,
                                         double open_price, double high_price,
                                         double low_price, double close_price)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.onBar(
      symbol, Price::fromDouble(open_price), Price::fromDouble(high_price),
      Price::fromDouble(low_price), Price::fromDouble(close_price));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_begin_bar_callback_window(FloxSimulatedExecutorHandle h)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.beginBarCallbackWindow();
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_end_bar_callback_window(FloxSimulatedExecutorHandle h)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.endBarCallbackWindow();
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_reset(FloxSimulatedExecutorHandle h)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.reset();
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_on_trade(FloxSimulatedExecutorHandle h, uint32_t symbol, double price, uint8_t is_buy)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.onTrade(symbol, Price::fromDouble(price),
                                                               is_buy != 0);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_advance_clock(FloxSimulatedExecutorHandle h, int64_t timestamp_ns)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->clock.advanceTo(UnixNanos::fromRaw(timestamp_ns));
  FLOX_CAPI_LEAVE_VOID;
}

uint32_t flox_simulated_executor_fill_count(FloxSimulatedExecutorHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint32_t>(static_cast<FloxSimulatedExecutorImpl*>(h)->executor.fills().size());
  FLOX_CAPI_LEAVE;
}

// ============================================================
// Bar aggregation
// ============================================================

static FloxBar toFloxBar(const Bar& bar)
{
  return {bar.startTime.time_since_epoch().count(),
          bar.endTime.time_since_epoch().count(),
          bar.open.raw(),
          bar.high.raw(),
          bar.low.raw(),
          bar.close.raw(),
          bar.volume.raw(),
          bar.buyVolume.raw(),
          static_cast<uint32_t>(bar.tradeCount.raw()),
          static_cast<uint8_t>(bar.reason)};
}

// The batch copy of the close path. It has to agree bar for bar with
// BarAggregator::onTrade and MultiTimeframeAggregator::processPolicy, so the
// two policy customization points -- a late trade the policy recognizes, and
// a policy that owns its own close -- are branched on here in the same order
// and with the same meaning.
template <typename Policy>
static uint32_t doAggregateC(Policy& policy, const int64_t* ts, const double* px,
                             const double* qty, const uint8_t* ib, size_t n, FloxBar* bars_out,
                             uint32_t max_bars)
{
  uint32_t count = 0;
  Bar currentBar;
  bool initialized = false;

  // Reports the true total even past max_bars so a caller that sized its
  // buffer to the trade count can tell the result was truncated and retry.
  const auto writeBar = [&](const Bar& bar)
  {
    if (count < max_bars)
    {
      bars_out[count] = toFloxBar(bar);
    }
    count++;
  };

  for (size_t i = 0; i < n; ++i)
  {
    TradeEvent trade;
    trade.trade.price = Price::fromDouble(px[i]);
    trade.trade.quantity = Quantity::fromDouble(qty[i]);
    trade.trade.isBuy = (ib[i] != 0);
    trade.trade.exchangeTsNs = UnixNanos::fromRaw(ts[i]);
    trade.trade.symbol = 1;
    trade.trade.instrument = InstrumentType::Spot;

    if (!initialized)
    {
      policy.initBar(trade, currentBar);
      initialized = true;
      continue;
    }

    if constexpr (DetectsLateTrades<Policy>)
    {
      if (policy.isLate(trade, currentBar))
      {
        continue;
      }
    }

    if (policy.shouldClose(trade, currentBar))
    {
      if constexpr (ClosesAndReopens<Policy>)
      {
        policy.closeAndReopen(trade, currentBar, writeBar);
      }
      else
      {
        writeBar(currentBar);
        policy.initBar(trade, currentBar);
      }
      continue;
    }
    policy.update(trade, currentBar);
  }
  return count;
}

uint32_t flox_aggregate_time_bars(const int64_t* ts, const double* px, const double* qty,
                                  const uint8_t* ib, size_t len, double interval_seconds,
                                  FloxBar* bars_out, uint32_t max_bars)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  TimeBarPolicy policy(std::chrono::nanoseconds(
      static_cast<int64_t>(interval_seconds * 1'000'000'000.0)));
  return doAggregateC(policy, ts, px, qty, ib, len, bars_out, max_bars);
  FLOX_CAPI_LEAVE;
}

uint32_t flox_aggregate_tick_bars(const int64_t* ts, const double* px, const double* qty,
                                  const uint8_t* ib, size_t len, uint32_t tick_count,
                                  FloxBar* bars_out, uint32_t max_bars)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  TickBarPolicy policy(tick_count);
  return doAggregateC(policy, ts, px, qty, ib, len, bars_out, max_bars);
  FLOX_CAPI_LEAVE;
}

uint32_t flox_aggregate_volume_bars(const int64_t* ts, const double* px, const double* qty,
                                    const uint8_t* ib, size_t len, double volume_threshold,
                                    FloxBar* bars_out, uint32_t max_bars)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  auto policy = VolumeBarPolicy::fromDouble(volume_threshold);
  return doAggregateC(policy, ts, px, qty, ib, len, bars_out, max_bars);
  FLOX_CAPI_LEAVE;
}

// ============================================================
// Multi-timeframe alignment helpers
// ============================================================

static void writeFloxBar(const Bar& bar, FloxBar* out)
{
  *out = toFloxBar(bar);
}

uint8_t flox_strategy_last_closed_bar(FloxStrategyHandle s, uint32_t symbol,
                                      uint8_t bar_type, uint64_t param, FloxBar* out)
{
  FLOX_CAPI_ENTER(s);
  auto bar = toStrategy(s)->lastClosedBar(symbol, static_cast<BarType>(bar_type), param);
  if (!bar || !out)
  {
    return 0;
  }
  writeFloxBar(*bar, out);
  return 1;
  FLOX_CAPI_LEAVE;
}

uint32_t flox_strategy_last_n_closed_bars(FloxStrategyHandle s, uint32_t symbol,
                                          uint8_t bar_type, uint64_t param,
                                          FloxBar* bars_out, uint32_t max_bars)
{
  FLOX_CAPI_ENTER(s);
  if (!bars_out || max_bars == 0)
  {
    return 0;
  }
  auto bars = toStrategy(s)->lastNClosedBars(symbol, static_cast<BarType>(bar_type),
                                             param, static_cast<size_t>(max_bars));
  uint32_t count = static_cast<uint32_t>(std::min<size_t>(bars.size(), max_bars));
  for (uint32_t i = 0; i < count; ++i)
  {
    writeFloxBar(bars[i], &bars_out[i]);
  }
  return count;
  FLOX_CAPI_LEAVE;
}

uint32_t flox_strategy_get_bar_ring_capacity(FloxStrategyHandle s)
{
  FLOX_CAPI_ENTER(s);
  return static_cast<uint32_t>(toStrategy(s)->barRingCapacity());
  FLOX_CAPI_LEAVE;
}

void flox_strategy_set_bar_ring_capacity(FloxStrategyHandle s, uint32_t capacity)
{
  FLOX_CAPI_ENTER_VOID(s);
  toStrategy(s)->setBarRingCapacity(static_cast<size_t>(capacity));
  FLOX_CAPI_LEAVE_VOID;
}

// ============================================================
// Multi-leg order group
// ============================================================

#include "flox/execution/order_group.h"

static OrderGroup* toOrderGroup(FloxOrderGroupHandle h) { return static_cast<OrderGroup*>(h); }

FloxOrderGroupHandle flox_order_group_create(uint64_t parent_signal_id, uint8_t policy)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return new OrderGroup(parent_signal_id, static_cast<OrderGroupPolicy>(policy));
  FLOX_CAPI_LEAVE;
}

void flox_order_group_destroy(FloxOrderGroupHandle h)
{
  FLOX_CAPI_ENTER_DESTROY(h);
  delete toOrderGroup(h);
  FLOX_CAPI_LEAVE_VOID;
}

uint32_t flox_order_group_add_market_leg(FloxOrderGroupHandle h, uint32_t symbol,
                                         uint8_t side, int64_t qty_raw)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint32_t>(
      toOrderGroup(h)->addMarketLeg(symbol, side, Quantity::fromRaw(qty_raw)));
  FLOX_CAPI_LEAVE;
}

uint32_t flox_order_group_add_limit_leg(FloxOrderGroupHandle h, uint32_t symbol,
                                        uint8_t side, int64_t price_raw,
                                        int64_t qty_raw)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toOrderGroup(h)->addLimitLeg(
      symbol, side, Price::fromRaw(price_raw), Quantity::fromRaw(qty_raw)));
  FLOX_CAPI_LEAVE;
}

uint32_t flox_order_group_leg_count(FloxOrderGroupHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toOrderGroup(h)->legCount());
  FLOX_CAPI_LEAVE;
}

uint8_t flox_order_group_leg_state(FloxOrderGroupHandle h, uint32_t leg_index)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint8_t>(toOrderGroup(h)->leg(leg_index).state);
  FLOX_CAPI_LEAVE;
}

int64_t flox_order_group_leg_filled_raw(FloxOrderGroupHandle h, uint32_t leg_index)
{
  FLOX_CAPI_ENTER(h);
  return toOrderGroup(h)->leg(leg_index).filledQty.raw();
  FLOX_CAPI_LEAVE;
}

uint64_t flox_order_group_leg_order_id(FloxOrderGroupHandle h, uint32_t leg_index)
{
  FLOX_CAPI_ENTER(h);
  return toOrderGroup(h)->leg(leg_index).orderId;
  FLOX_CAPI_LEAVE;
}

void flox_order_group_record_submit(FloxOrderGroupHandle h, uint32_t leg_index,
                                    uint64_t order_id)
{
  FLOX_CAPI_ENTER_VOID(h);
  toOrderGroup(h)->recordSubmit(leg_index, order_id);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_order_group_record_fill(FloxOrderGroupHandle h, uint32_t leg_index,
                                  int64_t cumulative_qty_raw)
{
  FLOX_CAPI_ENTER_VOID(h);
  toOrderGroup(h)->recordFill(leg_index, Quantity::fromRaw(cumulative_qty_raw));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_order_group_record_cancel(FloxOrderGroupHandle h, uint32_t leg_index)
{
  FLOX_CAPI_ENTER_VOID(h);
  toOrderGroup(h)->recordCancel(leg_index);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_order_group_record_failure(FloxOrderGroupHandle h, uint32_t leg_index)
{
  FLOX_CAPI_ENTER_VOID(h);
  toOrderGroup(h)->recordFailure(leg_index);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_order_group_record_replace_accepted(FloxOrderGroupHandle h, uint32_t leg_index,
                                              uint64_t new_order_id)
{
  FLOX_CAPI_ENTER_VOID(h);
  toOrderGroup(h)->recordReplaceAccepted(leg_index, new_order_id);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_order_group_record_replace_rejected(FloxOrderGroupHandle h, uint32_t leg_index)
{
  FLOX_CAPI_ENTER_VOID(h);
  toOrderGroup(h)->recordReplaceRejected(leg_index);
  FLOX_CAPI_LEAVE_VOID;
}

uint32_t flox_order_group_find_leg_by_order_id(FloxOrderGroupHandle h, uint64_t order_id)
{
  FLOX_CAPI_ENTER(h);
  auto idx = toOrderGroup(h)->findLegByOrderId(order_id);
  if (idx.has_value())
  {
    return static_cast<uint32_t>(*idx);
  }
  return UINT32_MAX;
  FLOX_CAPI_LEAVE;
}

uint8_t flox_order_group_state(FloxOrderGroupHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint8_t>(toOrderGroup(h)->state());
  FLOX_CAPI_LEAVE;
}

void flox_order_group_mark_action_dispatched(FloxOrderGroupHandle h, uint32_t leg_index,
                                             uint8_t kind)
{
  FLOX_CAPI_ENTER_VOID(h);
  toOrderGroup(h)->markActionDispatched(leg_index,
                                        static_cast<OrderGroupAction::Kind>(kind));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_order_group_set_risk_limits(FloxOrderGroupHandle h, int64_t max_gross_notional_raw,
                                      double max_concentration_pct, int64_t max_leg_qty_raw)
{
  FLOX_CAPI_ENTER_VOID(h);
  GroupRiskLimits limits;
  limits.maxGrossNotional = Quantity::fromRaw(max_gross_notional_raw);
  limits.maxConcentrationPct = max_concentration_pct;
  limits.maxLegQty = Quantity::fromRaw(max_leg_qty_raw);
  toOrderGroup(h)->setRiskLimits(limits);
  FLOX_CAPI_LEAVE_VOID;
}

namespace
{
inline void copy_truncated(const std::string& src, char* out, size_t capacity)
{
  if (!out || capacity == 0)
  {
    return;
  }
  size_t n = std::min(src.size(), capacity - 1);
  std::memcpy(out, src.data(), n);
  out[n] = '\0';
}
}  // namespace

uint8_t flox_order_group_precheck_submission(FloxOrderGroupHandle h, double equity,
                                             const int64_t* market_ref_prices_raw,
                                             uint32_t market_ref_prices_len, char* rule_out,
                                             size_t rule_capacity, char* detail_out,
                                             size_t detail_capacity)
{
  FLOX_CAPI_ENTER(h);
  std::vector<Price> prices;
  prices.reserve(market_ref_prices_len);
  for (uint32_t i = 0; i < market_ref_prices_len; ++i)
  {
    prices.push_back(Price::fromRaw(market_ref_prices_raw[i]));
  }
  auto breach = toOrderGroup(h)->precheckSubmission(equity, prices);
  if (!breach.denied)
  {
    if (rule_out && rule_capacity > 0)
    {
      rule_out[0] = '\0';
    }
    if (detail_out && detail_capacity > 0)
    {
      detail_out[0] = '\0';
    }
    return 0;
  }
  copy_truncated(breach.rule, rule_out, rule_capacity);
  copy_truncated(breach.detail, detail_out, detail_capacity);
  return 1;
  FLOX_CAPI_LEAVE;
}

void flox_order_group_set_pair_latency_budget_ns(FloxOrderGroupHandle h, int64_t budget_ns)
{
  FLOX_CAPI_ENTER_VOID(h);
  toOrderGroup(h)->setPairLatencyBudgetNs(budget_ns);
  FLOX_CAPI_LEAVE_VOID;
}

uint8_t flox_order_group_pair_latency_decision(FloxOrderGroupHandle h,
                                               int64_t leader_submit_ts_ns,
                                               int64_t leader_ack_ts_ns,
                                               uint8_t ack_received)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint8_t>(toOrderGroup(h)->pairLatencyDecision(
      leader_submit_ts_ns, leader_ack_ts_ns, ack_received != 0));
  FLOX_CAPI_LEAVE;
}

uint32_t flox_order_group_recommended_actions(FloxOrderGroupHandle h,
                                              int64_t* actions_out,
                                              uint32_t max_actions)
{
  FLOX_CAPI_ENTER(h);
  if (!actions_out || max_actions == 0)
  {
    return 0;
  }
  auto actions = toOrderGroup(h)->recommendedActions();
  uint32_t n = static_cast<uint32_t>(std::min<size_t>(actions.size(), max_actions));
  for (uint32_t i = 0; i < n; ++i)
  {
    const auto& a = actions[i];
    int64_t* slot = actions_out + i * 5;
    slot[0] = static_cast<int64_t>(a.kind);
    slot[1] = static_cast<int64_t>(a.legIndex);
    if (a.kind == OrderGroupAction::Kind::CancelLeg)
    {
      slot[2] = static_cast<int64_t>(a.orderId);
      slot[3] = 0;
      slot[4] = 0;
    }
    else
    {
      slot[2] = static_cast<int64_t>(a.symbol);
      slot[3] = static_cast<int64_t>(a.side);
      slot[4] = a.qty.raw();
    }
  }
  return n;
  FLOX_CAPI_LEAVE;
}

// ============================================================
// Multi-feed clock
// ============================================================

#include "flox/feed/multi_feed_clock.h"

namespace
{

struct FeedClockState
{
  MultiFeedClock clock;
  FeedClockSnapshot last;

  FeedClockState(std::vector<SymbolId> symbols, FeedClockPolicy policy, int64_t timeoutMs,
                 SymbolId leader, int64_t budgetMs)
      : clock(std::move(symbols), policy, timeoutMs, leader, budgetMs)
  {
  }
};

}  // namespace

static FeedClockState* toFeedClock(FloxFeedClockHandle h)
{
  return static_cast<FeedClockState*>(h);
}

FloxFeedClockHandle flox_feed_clock_create(const uint32_t* symbols, uint32_t symbol_count,
                                           uint8_t policy, int64_t timeout_ms,
                                           uint32_t leader_symbol,
                                           int64_t staleness_budget_ms)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  std::vector<SymbolId> sv;
  sv.reserve(symbol_count);
  for (uint32_t i = 0; i < symbol_count; ++i)
  {
    sv.push_back(symbols[i]);
  }
  return new FeedClockState(std::move(sv), static_cast<FeedClockPolicy>(policy), timeout_ms,
                            static_cast<SymbolId>(leader_symbol), staleness_budget_ms);
  FLOX_CAPI_LEAVE;
}

void flox_feed_clock_destroy(FloxFeedClockHandle h)
{
  FLOX_CAPI_ENTER_DESTROY(h);
  delete toFeedClock(h);
  FLOX_CAPI_LEAVE_VOID;
}

uint32_t flox_feed_clock_symbol_count(FloxFeedClockHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toFeedClock(h)->clock.symbolCount());
  FLOX_CAPI_LEAVE;
}

uint32_t flox_feed_clock_symbol_at(FloxFeedClockHandle h, uint32_t index)
{
  FLOX_CAPI_ENTER(h);
  auto& last = toFeedClock(h)->last;
  if (last.symbols.empty())
  {
    // Snapshot not yet populated; tick once to materialize symbols.
    // Use ts=0 + an out-of-band symbol so the tick records nothing.
    toFeedClock(h)->last = toFeedClock(h)->clock.tick(0, 0);
  }
  if (index >= toFeedClock(h)->last.symbols.size())
  {
    return 0;
  }
  return toFeedClock(h)->last.symbols[index];
  FLOX_CAPI_LEAVE;
}

uint8_t flox_feed_clock_tick(FloxFeedClockHandle h, int64_t ts_ns, uint32_t symbol)
{
  FLOX_CAPI_ENTER(h);
  auto* st = toFeedClock(h);
  st->last = st->clock.tick(ts_ns, static_cast<SymbolId>(symbol));
  return st->last.fired ? 1 : 0;
  FLOX_CAPI_LEAVE;
}

uint8_t flox_feed_clock_last_fired(FloxFeedClockHandle h)
{
  FLOX_CAPI_ENTER(h);
  return toFeedClock(h)->last.fired ? 1 : 0;
  FLOX_CAPI_LEAVE;
}

uint32_t flox_feed_clock_last_triggered_by(FloxFeedClockHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toFeedClock(h)->last.triggeredBy);
  FLOX_CAPI_LEAVE;
}

int64_t flox_feed_clock_last_seen_at(FloxFeedClockHandle h, uint32_t index)
{
  FLOX_CAPI_ENTER(h);
  auto& last = toFeedClock(h)->last;
  if (index >= last.lastTsNs.size())
  {
    return 0;
  }
  return last.lastTsNs[index];
  FLOX_CAPI_LEAVE;
}

int64_t flox_feed_clock_staleness_at(FloxFeedClockHandle h, uint32_t index)
{
  FLOX_CAPI_ENTER(h);
  auto& last = toFeedClock(h)->last;
  if (index >= last.stalenessNs.size())
  {
    return 0;
  }
  return last.stalenessNs[index];
  FLOX_CAPI_LEAVE;
}

void flox_feed_clock_reset(FloxFeedClockHandle h)
{
  FLOX_CAPI_ENTER_VOID(h);
  toFeedClock(h)->clock.reset();
  toFeedClock(h)->last = FeedClockSnapshot{};
  FLOX_CAPI_LEAVE_VOID;
}

// ============================================================
// Position tracking
// ============================================================

FloxPositionTrackerHandle flox_position_tracker_create(uint8_t cost_basis)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  auto method = static_cast<CostBasisMethod>(cost_basis);
  return new PositionTracker(0, method);
  FLOX_CAPI_LEAVE;
}

void flox_position_tracker_destroy(FloxPositionTrackerHandle tracker)
{
  FLOX_CAPI_ENTER_DESTROY(tracker);
  delete static_cast<PositionTracker*>(tracker);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_position_tracker_on_fill(FloxPositionTrackerHandle h, uint32_t symbol, uint8_t side,
                                   double price, double quantity)
{
  FLOX_CAPI_ENTER_VOID(h);
  auto* tracker = static_cast<PositionTracker*>(h);
  Order order{};
  order.symbol = symbol;
  order.side = side == 0 ? Side::BUY : Side::SELL;
  order.price = Price::fromDouble(price);
  order.quantity = Quantity::fromDouble(quantity);
  order.id = 0;
  tracker->onOrderFilled(order);
  FLOX_CAPI_LEAVE_VOID;
}

double flox_position_tracker_position(FloxPositionTrackerHandle h, uint32_t symbol)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<PositionTracker*>(h)->getPosition(symbol).toDouble();
  FLOX_CAPI_LEAVE;
}

double flox_position_tracker_avg_entry(FloxPositionTrackerHandle h, uint32_t symbol)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<PositionTracker*>(h)->getAvgEntryPrice(symbol).toDouble();
  FLOX_CAPI_LEAVE;
}

double flox_position_tracker_realized_pnl(FloxPositionTrackerHandle h, uint32_t symbol)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<PositionTracker*>(h)->getRealizedPnl(symbol).toDouble();
  FLOX_CAPI_LEAVE;
}

double flox_position_tracker_total_pnl(FloxPositionTrackerHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<PositionTracker*>(h)->getTotalRealizedPnl().toDouble();
  FLOX_CAPI_LEAVE;
}

// ============================================================
// Volume profile
// ============================================================

FloxVolumeProfileHandle flox_volume_profile_create(double tick_size)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  auto* vp = new VolumeProfile<>();
  vp->setTickSize(Price::fromDouble(tick_size));
  return vp;
  FLOX_CAPI_LEAVE;
}

void flox_volume_profile_destroy(FloxVolumeProfileHandle profile)
{
  FLOX_CAPI_ENTER_DESTROY(profile);
  delete static_cast<VolumeProfile<>*>(profile);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_volume_profile_add_trade(FloxVolumeProfileHandle h, double price, double quantity,
                                   uint8_t is_buy)
{
  FLOX_CAPI_ENTER_VOID(h);
  TradeEvent te;
  te.trade.price = Price::fromDouble(price);
  te.trade.quantity = Quantity::fromDouble(quantity);
  te.trade.isBuy = (is_buy != 0);
  static_cast<VolumeProfile<>*>(h)->addTrade(te);
  FLOX_CAPI_LEAVE_VOID;
}

double flox_volume_profile_poc(FloxVolumeProfileHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<VolumeProfile<>*>(h)->poc().toDouble();
  FLOX_CAPI_LEAVE;
}

double flox_volume_profile_vah(FloxVolumeProfileHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<VolumeProfile<>*>(h)->valueAreaHigh().toDouble();
  FLOX_CAPI_LEAVE;
}

double flox_volume_profile_val(FloxVolumeProfileHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<VolumeProfile<>*>(h)->valueAreaLow().toDouble();
  FLOX_CAPI_LEAVE;
}

double flox_volume_profile_total_volume(FloxVolumeProfileHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<VolumeProfile<>*>(h)->totalVolume().toDouble();
  FLOX_CAPI_LEAVE;
}

double flox_volume_profile_total_delta(FloxVolumeProfileHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<VolumeProfile<>*>(h)->totalDelta().toDouble();
  FLOX_CAPI_LEAVE;
}

uint32_t flox_volume_profile_num_levels(FloxVolumeProfileHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint32_t>(static_cast<VolumeProfile<>*>(h)->numLevels());
  FLOX_CAPI_LEAVE;
}

void flox_volume_profile_clear(FloxVolumeProfileHandle h)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<VolumeProfile<>*>(h)->clear();
  FLOX_CAPI_LEAVE_VOID;
}

// ============================================================
// Footprint bar
// ============================================================

FloxFootprintHandle flox_footprint_create(double tick_size)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  auto* fp = new FootprintBar<>();
  fp->setTickSize(Price::fromDouble(tick_size));
  return fp;
  FLOX_CAPI_LEAVE;
}

void flox_footprint_destroy(FloxFootprintHandle footprint)
{
  FLOX_CAPI_ENTER_DESTROY(footprint);
  delete static_cast<FootprintBar<>*>(footprint);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_footprint_add_trade(FloxFootprintHandle h, double price, double quantity,
                              uint8_t is_buy)
{
  FLOX_CAPI_ENTER_VOID(h);
  TradeEvent te;
  te.trade.price = Price::fromDouble(price);
  te.trade.quantity = Quantity::fromDouble(quantity);
  te.trade.isBuy = (is_buy != 0);
  static_cast<FootprintBar<>*>(h)->addTrade(te);
  FLOX_CAPI_LEAVE_VOID;
}

double flox_footprint_total_delta(FloxFootprintHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<FootprintBar<>*>(h)->totalDelta().toDouble();
  FLOX_CAPI_LEAVE;
}

double flox_footprint_total_volume(FloxFootprintHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<FootprintBar<>*>(h)->totalVolume().toDouble();
  FLOX_CAPI_LEAVE;
}

uint32_t flox_footprint_num_levels(FloxFootprintHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint32_t>(static_cast<FootprintBar<>*>(h)->numLevels());
  FLOX_CAPI_LEAVE;
}

void flox_footprint_clear(FloxFootprintHandle h)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FootprintBar<>*>(h)->clear();
  FLOX_CAPI_LEAVE_VOID;
}

// ============================================================
// Statistics
// ============================================================

double flox_stat_correlation(const double* x, const double* y, size_t len)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  double sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0, sumY2 = 0;
  for (size_t i = 0; i < len; i++)
  {
    sumX += x[i];
    sumY += y[i];
    sumXY += x[i] * y[i];
    sumX2 += x[i] * x[i];
    sumY2 += y[i] * y[i];
  }
  double n = static_cast<double>(len);
  double num = n * sumXY - sumX * sumY;
  double den = std::sqrt((n * sumX2 - sumX * sumX) * (n * sumY2 - sumY * sumY));
  return den != 0 ? num / den : 0;
  FLOX_CAPI_LEAVE;
}

double flox_stat_profit_factor(const double* pnl, size_t len)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  double gross_profit = 0, gross_loss = 0;
  for (size_t i = 0; i < len; i++)
  {
    if (pnl[i] > 0)
    {
      gross_profit += pnl[i];
    }
    else
    {
      gross_loss -= pnl[i];
    }
  }
  return gross_loss > 0 ? gross_profit / gross_loss : (gross_profit > 0 ? 1e9 : 0);
  FLOX_CAPI_LEAVE;
}

double flox_stat_win_rate(const double* pnl, size_t len)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  if (len == 0)
  {
    return 0;
  }
  size_t wins = 0;
  for (size_t i = 0; i < len; i++)
  {
    if (pnl[i] > 0)
    {
      wins++;
    }
  }
  return static_cast<double>(wins) / static_cast<double>(len);
  FLOX_CAPI_LEAVE;
}

// ============================================================
// Order journey tracer
// ============================================================

namespace
{
flox::OrderJourneyTracer* asTracer(FloxOrderJourneyTracerHandle h)
{
  return static_cast<flox::OrderJourneyTracer*>(h);
}

FloxOrderTraceRow toCapiRow(const flox::OrderTraceRecord& r)
{
  FloxOrderTraceRow row{};
  row.order_id = r.orderId;
  row.seq = r.seq;
  row.status = r.status;
  row.is_maker = r.isMaker;
  row.ts_ns = r.tsNs;
  row.fill_qty_raw = r.fillQtyRaw;
  row.fill_price_raw = r.fillPriceRaw;
  row.queue_ahead_raw = r.queueAheadRaw;
  row.queue_total_raw = r.queueTotalRaw;
  row.submitted_at_ns = r.timestamps.submittedAtNs.raw();
  row.accepted_at_ns = r.timestamps.acceptedAtNs.raw();
  row.first_fill_at_ns = r.timestamps.firstFillAtNs.raw();
  row.last_fill_at_ns = r.timestamps.lastFillAtNs.raw();
  row.canceled_at_ns = r.timestamps.canceledAtNs.raw();
  row.rejected_at_ns = r.timestamps.rejectedAtNs.raw();
  row.triggered_at_ns = r.timestamps.triggeredAtNs.raw();
  row.expired_at_ns = r.timestamps.expiredAtNs.raw();
  return row;
}
}  // namespace

FloxOrderJourneyTracerHandle flox_order_journey_tracer_create(
    uint64_t max_orders, uint64_t max_records_per_order, double sample_rate,
    uint64_t sample_salt)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  flox::OrderJourneyTracer::Config cfg{};
  cfg.maxOrders = max_orders;
  cfg.maxRecordsPerOrder = max_records_per_order;
  cfg.sampleRate = sample_rate;
  cfg.sampleSalt = sample_salt;
  return new flox::OrderJourneyTracer(cfg);
  FLOX_CAPI_LEAVE;
}

void flox_order_journey_tracer_destroy(FloxOrderJourneyTracerHandle h)
{
  FLOX_CAPI_ENTER_DESTROY(h);
  delete asTracer(h);
  FLOX_CAPI_LEAVE_VOID;
}

uint64_t flox_order_journey_tracer_order_count(FloxOrderJourneyTracerHandle h)
{
  FLOX_CAPI_ENTER(h);
  return asTracer(h) ? asTracer(h)->orderCount() : 0u;
  FLOX_CAPI_LEAVE;
}

uint64_t flox_order_journey_tracer_record_count(FloxOrderJourneyTracerHandle h)
{
  FLOX_CAPI_ENTER(h);
  return asTracer(h) ? asTracer(h)->recordCount() : 0u;
  FLOX_CAPI_LEAVE;
}

double flox_order_journey_tracer_median_ack_latency_ns(FloxOrderJourneyTracerHandle h)
{
  FLOX_CAPI_ENTER(h);
  return asTracer(h) ? asTracer(h)->medianAckLatencyNs() : 0.0;
  FLOX_CAPI_LEAVE;
}

double flox_order_journey_tracer_median_time_to_first_fill_ns(
    FloxOrderJourneyTracerHandle h)
{
  FLOX_CAPI_ENTER(h);
  return asTracer(h) ? asTracer(h)->medianTimeToFirstFillNs() : 0.0;
  FLOX_CAPI_LEAVE;
}

double flox_order_journey_tracer_maker_fill_ratio(FloxOrderJourneyTracerHandle h)
{
  FLOX_CAPI_ENTER(h);
  return asTracer(h) ? asTracer(h)->makerFillRatio() : 0.0;
  FLOX_CAPI_LEAVE;
}

double flox_order_journey_tracer_cancel_race_loss_rate(FloxOrderJourneyTracerHandle h)
{
  FLOX_CAPI_ENTER(h);
  return asTracer(h) ? asTracer(h)->cancelRaceLossRate() : 0.0;
  FLOX_CAPI_LEAVE;
}

uint64_t flox_order_journey_tracer_result(FloxOrderJourneyTracerHandle h,
                                          FloxOrderTraceRow* out, uint64_t max_rows)
{
  FLOX_CAPI_ENTER(h);
  auto* tracer = asTracer(h);
  if (!tracer)
  {
    return 0u;
  }
  auto rows = tracer->result();
  if (out == nullptr || max_rows == 0)
  {
    return rows.size();
  }
  const uint64_t n = std::min<uint64_t>(rows.size(), max_rows);
  for (uint64_t i = 0; i < n; ++i)
  {
    out[i] = toCapiRow(rows[i]);
  }
  return n;
  FLOX_CAPI_LEAVE;
}

uint64_t flox_order_journey_tracer_journey(FloxOrderJourneyTracerHandle h,
                                           uint64_t order_id, FloxOrderTraceRow* out,
                                           uint64_t max_rows)
{
  FLOX_CAPI_ENTER(h);
  auto* tracer = asTracer(h);
  if (!tracer)
  {
    return 0u;
  }
  auto rows = tracer->journey(static_cast<flox::OrderId>(order_id));
  if (out == nullptr || max_rows == 0)
  {
    return rows.size();
  }
  const uint64_t n = std::min<uint64_t>(rows.size(), max_rows);
  for (uint64_t i = 0; i < n; ++i)
  {
    out[i] = toCapiRow(rows[i]);
  }
  return n;
  FLOX_CAPI_LEAVE;
}

void flox_order_journey_tracer_clear(FloxOrderJourneyTracerHandle h)
{
  FLOX_CAPI_ENTER_VOID(h);
  if (auto* tracer = asTracer(h))
  {
    tracer->clear();
  }
  FLOX_CAPI_LEAVE_VOID;
}

// ============================================================
// Order tracker
// ============================================================

FloxOrderTrackerHandle flox_order_tracker_create(void)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return new OrderTracker();
  FLOX_CAPI_LEAVE;
}

void flox_order_tracker_destroy(FloxOrderTrackerHandle tracker)
{
  FLOX_CAPI_ENTER_DESTROY(tracker);
  delete static_cast<OrderTracker*>(tracker);
  FLOX_CAPI_LEAVE_VOID;
}

uint8_t flox_order_tracker_on_submitted(FloxOrderTrackerHandle h, uint64_t order_id,
                                        uint32_t symbol, uint8_t side, double price, double qty)
{
  FLOX_CAPI_ENTER(h);
  Order order{};
  order.id = order_id;
  order.symbol = symbol;
  order.side = side == 0 ? Side::BUY : Side::SELL;
  order.price = Price::fromDouble(price);
  order.quantity = Quantity::fromDouble(qty);
  return static_cast<OrderTracker*>(h)->onSubmitted(order, "", "") ? 1 : 0;
  FLOX_CAPI_LEAVE;
}

uint8_t flox_order_tracker_on_filled(FloxOrderTrackerHandle h, uint64_t order_id, double fill_qty)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<OrderTracker*>(h)->onFilled(order_id, Quantity::fromDouble(fill_qty)) ? 1
                                                                                           : 0;
  FLOX_CAPI_LEAVE;
}

uint8_t flox_order_tracker_on_canceled(FloxOrderTrackerHandle h, uint64_t order_id)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<OrderTracker*>(h)->onCanceled(order_id) ? 1 : 0;
  FLOX_CAPI_LEAVE;
}

uint8_t flox_order_tracker_is_active(FloxOrderTrackerHandle h, uint64_t order_id)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<OrderTracker*>(h)->isActive(order_id) ? 1 : 0;
  FLOX_CAPI_LEAVE;
}

uint32_t flox_order_tracker_active_count(FloxOrderTrackerHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint32_t>(static_cast<OrderTracker*>(h)->activeOrderCount());
  FLOX_CAPI_LEAVE;
}

uint32_t flox_order_tracker_total_count(FloxOrderTrackerHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint32_t>(static_cast<OrderTracker*>(h)->totalOrderCount());
  FLOX_CAPI_LEAVE;
}

void flox_order_tracker_prune(FloxOrderTrackerHandle h)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<OrderTracker*>(h)->pruneTerminal();
  FLOX_CAPI_LEAVE_VOID;
}

// ============================================================
// Position group tracker
// ============================================================

FloxPositionGroupHandle flox_position_group_create(void)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return new PositionGroupTracker();
  FLOX_CAPI_LEAVE;
}

void flox_position_group_destroy(FloxPositionGroupHandle h)
{
  FLOX_CAPI_ENTER_DESTROY(h);
  delete static_cast<PositionGroupTracker*>(h);
  FLOX_CAPI_LEAVE_VOID;
}

uint64_t flox_position_group_open(FloxPositionGroupHandle h, uint64_t order_id, uint32_t symbol,
                                  uint8_t side, double price, double qty)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<PositionGroupTracker*>(h)->openPosition(
      order_id, symbol, side == 0 ? Side::BUY : Side::SELL, Price::fromDouble(price),
      Quantity::fromDouble(qty));
  FLOX_CAPI_LEAVE;
}

void flox_position_group_close(FloxPositionGroupHandle h, uint64_t position_id, double exit_price)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<PositionGroupTracker*>(h)->closePosition(position_id,
                                                       Price::fromDouble(exit_price));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_position_group_partial_close(FloxPositionGroupHandle h, uint64_t position_id, double qty,
                                       double exit_price)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<PositionGroupTracker*>(h)->partialClose(position_id, Quantity::fromDouble(qty),
                                                      Price::fromDouble(exit_price));
  FLOX_CAPI_LEAVE_VOID;
}

double flox_position_group_net_position(FloxPositionGroupHandle h, uint32_t symbol)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<PositionGroupTracker*>(h)->netPosition(symbol).toDouble();
  FLOX_CAPI_LEAVE;
}

double flox_position_group_realized_pnl(FloxPositionGroupHandle h, uint32_t symbol)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<PositionGroupTracker*>(h)->realizedPnl(symbol).toDouble();
  FLOX_CAPI_LEAVE;
}

double flox_position_group_total_pnl(FloxPositionGroupHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<PositionGroupTracker*>(h)->totalRealizedPnl().toDouble();
  FLOX_CAPI_LEAVE;
}

uint32_t flox_position_group_open_count(FloxPositionGroupHandle h, uint32_t symbol)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint32_t>(
      static_cast<PositionGroupTracker*>(h)->openPositionCount(symbol));
  FLOX_CAPI_LEAVE;
}

void flox_position_group_prune(FloxPositionGroupHandle h)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<PositionGroupTracker*>(h)->pruneClosedPositions();
  FLOX_CAPI_LEAVE_VOID;
}

// ============================================================
// Market profile
// ============================================================

FloxMarketProfileHandle flox_market_profile_create(double tick_size, uint32_t period_minutes,
                                                   int64_t session_start_ns)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  auto* mp = new MarketProfile<>();
  mp->setTickSize(Price::fromDouble(tick_size));
  mp->setPeriodDuration(std::chrono::minutes(period_minutes));
  mp->setSessionStart(static_cast<uint64_t>(session_start_ns));
  return mp;
  FLOX_CAPI_LEAVE;
}

void flox_market_profile_destroy(FloxMarketProfileHandle h)
{
  FLOX_CAPI_ENTER_DESTROY(h);
  delete static_cast<MarketProfile<>*>(h);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_market_profile_add_trade(FloxMarketProfileHandle h, int64_t timestamp_ns, double price,
                                   double qty, uint8_t is_buy)
{
  FLOX_CAPI_ENTER_VOID(h);
  TradeEvent te;
  te.trade.price = Price::fromDouble(price);
  te.trade.quantity = Quantity::fromDouble(qty);
  te.trade.isBuy = (is_buy != 0);
  te.trade.exchangeTsNs = UnixNanos::fromRaw(timestamp_ns);
  static_cast<MarketProfile<>*>(h)->addTrade(te);
  FLOX_CAPI_LEAVE_VOID;
}

double flox_market_profile_poc(FloxMarketProfileHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<MarketProfile<>*>(h)->poc().toDouble();
  FLOX_CAPI_LEAVE;
}

double flox_market_profile_vah(FloxMarketProfileHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<MarketProfile<>*>(h)->valueAreaHigh().toDouble();
  FLOX_CAPI_LEAVE;
}

double flox_market_profile_val(FloxMarketProfileHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<MarketProfile<>*>(h)->valueAreaLow().toDouble();
  FLOX_CAPI_LEAVE;
}

double flox_market_profile_ib_high(FloxMarketProfileHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<MarketProfile<>*>(h)->initialBalanceHigh().toDouble();
  FLOX_CAPI_LEAVE;
}

double flox_market_profile_ib_low(FloxMarketProfileHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<MarketProfile<>*>(h)->initialBalanceLow().toDouble();
  FLOX_CAPI_LEAVE;
}

uint8_t flox_market_profile_is_poor_high(FloxMarketProfileHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<MarketProfile<>*>(h)->isPoorHigh() ? 1 : 0;
  FLOX_CAPI_LEAVE;
}

uint8_t flox_market_profile_is_poor_low(FloxMarketProfileHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<MarketProfile<>*>(h)->isPoorLow() ? 1 : 0;
  FLOX_CAPI_LEAVE;
}

uint32_t flox_market_profile_num_levels(FloxMarketProfileHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint32_t>(static_cast<MarketProfile<>*>(h)->numLevels());
  FLOX_CAPI_LEAVE;
}

void flox_market_profile_clear(FloxMarketProfileHandle h)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<MarketProfile<>*>(h)->clear();
  FLOX_CAPI_LEAVE_VOID;
}

// ============================================================
// Composite book
// ============================================================

FloxCompositeBookHandle flox_composite_book_create(void)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return new CompositeBookMatrix<>();
  FLOX_CAPI_LEAVE;
}

void flox_composite_book_destroy(FloxCompositeBookHandle h)
{
  FLOX_CAPI_ENTER_DESTROY(h);
  delete static_cast<CompositeBookMatrix<>*>(h);
  FLOX_CAPI_LEAVE_VOID;
}

uint8_t flox_composite_book_best_bid(FloxCompositeBookHandle h, uint32_t symbol, double* price_out,
                                     double* qty_out)
{
  FLOX_CAPI_ENTER(h);
  auto q = static_cast<CompositeBookMatrix<>*>(h)->bestBid(symbol);
  if (q.valid)
  {
    *price_out = Price::fromRaw(q.priceRaw).toDouble();
    *qty_out = Quantity::fromRaw(q.qtyRaw).toDouble();
    return 1;
  }
  return 0;
  FLOX_CAPI_LEAVE;
}

uint8_t flox_composite_book_best_ask(FloxCompositeBookHandle h, uint32_t symbol, double* price_out,
                                     double* qty_out)
{
  FLOX_CAPI_ENTER(h);
  auto q = static_cast<CompositeBookMatrix<>*>(h)->bestAsk(symbol);
  if (q.valid)
  {
    *price_out = Price::fromRaw(q.priceRaw).toDouble();
    *qty_out = Quantity::fromRaw(q.qtyRaw).toDouble();
    return 1;
  }
  return 0;
  FLOX_CAPI_LEAVE;
}

uint8_t flox_composite_book_has_arb(FloxCompositeBookHandle h, uint32_t symbol)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<CompositeBookMatrix<>*>(h)->hasArbitrageOpportunity(symbol) ? 1 : 0;
  FLOX_CAPI_LEAVE;
}

void flox_composite_book_mark_stale(FloxCompositeBookHandle h, uint32_t exchange, uint32_t symbol)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<CompositeBookMatrix<>*>(h)->markStale(exchange, symbol);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_composite_book_check_staleness(FloxCompositeBookHandle h, int64_t now_ns,
                                         int64_t threshold_ns)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<CompositeBookMatrix<>*>(h)->checkStaleness(now_ns, threshold_ns);
  FLOX_CAPI_LEAVE_VOID;
}

namespace
{
// Shared by flox_composite_book_apply_snapshot/_delta below -- same shape as
// FloxBookImpl::applyUpdate above, but addressed at one exchange/symbol slot
// of a multi-venue CompositeBookMatrix rather than at a single-venue book.
void applyCompositeBookUpdate(CompositeBookMatrix<>* matrix, uint32_t exchange, uint32_t symbol,
                              const double* bp, const double* bq, size_t bl, const double* ap,
                              const double* aq, size_t al, int64_t recv_ns, BookUpdateType type)
{
  std::byte buf[32768];
  std::pmr::monotonic_buffer_resource res(buf, sizeof(buf));
  BookUpdateEvent ev(&res);
  ev.update.symbol = symbol;
  ev.update.type = type;
  ev.sourceExchange = static_cast<ExchangeId>(exchange);
  ev.recvNs = MonoNanos::fromRaw(static_cast<uint64_t>(recv_ns));
  ev.update.bids.reserve(bl);
  for (size_t i = 0; i < bl; ++i)
  {
    ev.update.bids.push_back({Price::fromDouble(bp[i]), Quantity::fromDouble(bq[i])});
  }
  ev.update.asks.reserve(al);
  for (size_t i = 0; i < al; ++i)
  {
    ev.update.asks.push_back({Price::fromDouble(ap[i]), Quantity::fromDouble(aq[i])});
  }
  matrix->onBookUpdate(ev);
}
}  // namespace

void flox_composite_book_apply_snapshot(FloxCompositeBookHandle h, uint32_t exchange, uint32_t symbol,
                                        const double* bid_prices, const double* bid_qtys,
                                        size_t bid_len, const double* ask_prices,
                                        const double* ask_qtys, size_t ask_len, int64_t recv_ns)
{
  FLOX_CAPI_ENTER_VOID(h);
  applyCompositeBookUpdate(static_cast<CompositeBookMatrix<>*>(h), exchange, symbol, bid_prices,
                           bid_qtys, bid_len, ask_prices, ask_qtys, ask_len, recv_ns,
                           BookUpdateType::SNAPSHOT);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_composite_book_apply_delta(FloxCompositeBookHandle h, uint32_t exchange, uint32_t symbol,
                                     const double* bid_prices, const double* bid_qtys, size_t bid_len,
                                     const double* ask_prices, const double* ask_qtys, size_t ask_len,
                                     int64_t recv_ns)
{
  FLOX_CAPI_ENTER_VOID(h);
  applyCompositeBookUpdate(static_cast<CompositeBookMatrix<>*>(h), exchange, symbol, bid_prices,
                           bid_qtys, bid_len, ask_prices, ask_qtys, ask_len, recv_ns,
                           BookUpdateType::DELTA);
  FLOX_CAPI_LEAVE_VOID;
}

// ============================================================
// Executor fill access
// ============================================================

uint32_t flox_simulated_executor_get_fills(FloxSimulatedExecutorHandle h, FloxFill* fills_out, uint32_t max_fills)
{
  FLOX_CAPI_ENTER(h);
  auto& fills = static_cast<FloxSimulatedExecutorImpl*>(h)->executor.fills();
  uint32_t count = static_cast<uint32_t>(std::min(fills.size(), static_cast<size_t>(max_fills)));
  for (uint32_t i = 0; i < count; i++)
  {
    fills_out[i].order_id = fills[i].orderId;
    fills_out[i].symbol = fills[i].symbol;
    fills_out[i].side = fills[i].side == Side::BUY ? 0 : 1;
    fills_out[i].price_raw = fills[i].price.raw();
    fills_out[i].quantity_raw = fills[i].quantity.raw();
    fills_out[i].timestamp_ns = fills[i].timestampNs.raw();
  }
  return count;
  FLOX_CAPI_LEAVE;
}

// ============================================================
// Additional bar aggregation
// ============================================================

uint32_t flox_aggregate_range_bars(const int64_t* ts, const double* px, const double* qty,
                                   const uint8_t* ib, size_t len, double range_size,
                                   FloxBar* bars_out, uint32_t max_bars)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  auto policy = RangeBarPolicy::fromDouble(range_size);
  return doAggregateC(policy, ts, px, qty, ib, len, bars_out, max_bars);
  FLOX_CAPI_LEAVE;
}

uint32_t flox_aggregate_renko_bars(const int64_t* ts, const double* px, const double* qty,
                                   const uint8_t* ib, size_t len, double brick_size,
                                   FloxBar* bars_out, uint32_t max_bars)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  auto policy = RenkoBarPolicy::fromDouble(brick_size);
  return doAggregateC(policy, ts, px, qty, ib, len, bars_out, max_bars);
  FLOX_CAPI_LEAVE;
}

// ============================================================
// L3 Order book
// ============================================================

FloxL3BookHandle flox_l3_book_create(void)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return new L3OrderBook<>();
  FLOX_CAPI_LEAVE;
}

void flox_l3_book_destroy(FloxL3BookHandle h)
{
  FLOX_CAPI_ENTER_DESTROY(h);
  delete static_cast<L3OrderBook<>*>(h);
  FLOX_CAPI_LEAVE_VOID;
}

int32_t flox_l3_book_add_order(FloxL3BookHandle h, uint64_t order_id, double price, double quantity,
                               uint8_t side)
{
  FLOX_CAPI_ENTER(h);
  auto status = static_cast<L3OrderBook<>*>(h)->addOrder(
      order_id, Price::fromDouble(price), Quantity::fromDouble(quantity),
      side == 0 ? Side::BUY : Side::SELL);
  return static_cast<int32_t>(status);
  FLOX_CAPI_LEAVE;
}

int32_t flox_l3_book_remove_order(FloxL3BookHandle h, uint64_t order_id)
{
  FLOX_CAPI_ENTER(h);
  auto status = static_cast<L3OrderBook<>*>(h)->removeOrder(order_id);
  return static_cast<int32_t>(status);
  FLOX_CAPI_LEAVE;
}

int32_t flox_l3_book_modify_order(FloxL3BookHandle h, uint64_t order_id, double new_qty)
{
  FLOX_CAPI_ENTER(h);
  auto status =
      static_cast<L3OrderBook<>*>(h)->modifyOrder(order_id, Quantity::fromDouble(new_qty));
  return static_cast<int32_t>(status);
  FLOX_CAPI_LEAVE;
}

uint8_t flox_l3_book_best_bid(FloxL3BookHandle h, double* price_out)
{
  FLOX_CAPI_ENTER(h);
  auto bid = static_cast<L3OrderBook<>*>(h)->bestBid();
  if (bid)
  {
    *price_out = bid->toDouble();
    return 1;
  }
  return 0;
  FLOX_CAPI_LEAVE;
}

uint8_t flox_l3_book_best_ask(FloxL3BookHandle h, double* price_out)
{
  FLOX_CAPI_ENTER(h);
  auto ask = static_cast<L3OrderBook<>*>(h)->bestAsk();
  if (ask)
  {
    *price_out = ask->toDouble();
    return 1;
  }
  return 0;
  FLOX_CAPI_LEAVE;
}

double flox_l3_book_bid_at_price(FloxL3BookHandle h, double price)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<L3OrderBook<>*>(h)->bidAtPrice(Price::fromDouble(price)).toDouble();
  FLOX_CAPI_LEAVE;
}

double flox_l3_book_ask_at_price(FloxL3BookHandle h, double price)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<L3OrderBook<>*>(h)->askAtPrice(Price::fromDouble(price)).toDouble();
  FLOX_CAPI_LEAVE;
}

// ============================================================
// Data writer
// ============================================================

FloxDataWriterHandle flox_data_writer_create(const char* output_dir, uint64_t max_segment_mb,
                                             uint8_t exchange_id)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  FLOX_CAPI_REQUIRE(output_dir);
  replay::WriterConfig cfg;
  cfg.output_dir = output_dir;
  cfg.max_segment_bytes = max_segment_mb * 1024 * 1024;
  cfg.exchange_id = exchange_id;
  return new replay::BinaryLogWriter(cfg);
  FLOX_CAPI_LEAVE;
}

void flox_data_writer_destroy(FloxDataWriterHandle h)
{
  FLOX_CAPI_ENTER_DESTROY(h);
  auto* w = static_cast<replay::BinaryLogWriter*>(h);
  w->close();
  delete w;
  FLOX_CAPI_LEAVE_VOID;
}

uint8_t flox_data_writer_write_trade(FloxDataWriterHandle h, int64_t exchange_ts_ns,
                                     int64_t recv_ts_ns, double price, double qty,
                                     uint64_t trade_id, uint32_t symbol_id, uint8_t side)
{
  FLOX_CAPI_ENTER(h);
  replay::TradeRecord tr{};
  tr.exchange_ts_ns = exchange_ts_ns;
  tr.recv_ts_ns = recv_ts_ns;
  tr.price_raw = Price::fromDouble(price).raw();
  tr.qty_raw = Quantity::fromDouble(qty).raw();
  tr.trade_id = trade_id;
  tr.symbol_id = symbol_id;
  tr.side = side;
  return static_cast<replay::BinaryLogWriter*>(h)->writeTrade(tr) ? 1 : 0;
  FLOX_CAPI_LEAVE;
}

uint8_t flox_data_writer_write_book(FloxDataWriterHandle h,
                                    int64_t exchange_ts_ns,
                                    int64_t recv_ts_ns,
                                    int64_t seq,
                                    uint32_t symbol_id,
                                    uint8_t is_snapshot,
                                    const FloxBookLevel* bids, uint32_t n_bids,
                                    const FloxBookLevel* asks, uint32_t n_asks)
{
  FLOX_CAPI_ENTER(h);
  replay::BookRecordHeader header{};
  header.exchange_ts_ns = exchange_ts_ns;
  header.recv_ts_ns = recv_ts_ns;
  header.seq = seq;
  header.symbol_id = symbol_id;
  header.bid_count = static_cast<uint16_t>(n_bids);
  header.ask_count = static_cast<uint16_t>(n_asks);
  header.type = is_snapshot ? 0 : 1;

  // FloxBookLevel and replay::BookLevel are layout-compatible
  // (int64 price_raw, int64 qty_raw). Cast is intentional.
  auto* bid_levels = reinterpret_cast<const replay::BookLevel*>(bids);
  auto* ask_levels = reinterpret_cast<const replay::BookLevel*>(asks);
  std::span<const replay::BookLevel> bid_span(bid_levels, n_bids);
  std::span<const replay::BookLevel> ask_span(ask_levels, n_asks);

  return static_cast<replay::BinaryLogWriter*>(h)->writeBook(header, bid_span, ask_span) ? 1 : 0;
  FLOX_CAPI_LEAVE;
}

uint64_t flox_data_writer_write_books(FloxDataWriterHandle h,
                                      const FloxBookUpdateHeader* headers,
                                      uint64_t n_events,
                                      const FloxLevel* levels,
                                      uint64_t /*total_levels*/)
{
  FLOX_CAPI_ENTER(h);
  auto* writer = static_cast<replay::BinaryLogWriter*>(h);
  if (!writer || !headers)
  {
    return 0;
  }

  // FloxLevel has an extra `side` byte we ignore on write — copy into
  // contiguous replay::BookLevel buffers per event.
  std::vector<replay::BookLevel> scratch;
  uint64_t written = 0;
  for (uint64_t i = 0; i < n_events; ++i)
  {
    const auto& fh = headers[i];
    const uint64_t total = static_cast<uint64_t>(fh.bid_count) + fh.ask_count;
    scratch.clear();
    scratch.reserve(total);
    for (uint64_t k = 0; k < total; ++k)
    {
      const auto& lv = levels[fh.level_offset + k];
      scratch.push_back({lv.price_raw, lv.qty_raw});
    }

    replay::BookRecordHeader rh{};
    rh.exchange_ts_ns = fh.exchange_ts_ns;
    rh.recv_ts_ns = fh.recv_ts_ns;
    rh.seq = fh.seq;
    rh.symbol_id = fh.symbol_id;
    rh.bid_count = fh.bid_count;
    rh.ask_count = fh.ask_count;
    rh.type = (fh.event_type == 2) ? 0 : 1;

    std::span<const replay::BookLevel> bid_span(scratch.data(), fh.bid_count);
    std::span<const replay::BookLevel> ask_span(scratch.data() + fh.bid_count, fh.ask_count);
    if (writer->writeBook(rh, bid_span, ask_span))
    {
      ++written;
    }
  }
  return written;
  FLOX_CAPI_LEAVE;
}

void flox_data_writer_flush(FloxDataWriterHandle h)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<replay::BinaryLogWriter*>(h)->flush();
  FLOX_CAPI_LEAVE_VOID;
}

void flox_data_writer_close(FloxDataWriterHandle h)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<replay::BinaryLogWriter*>(h)->close();
  FLOX_CAPI_LEAVE_VOID;
}

// ============================================================
// Data reader
// ============================================================

FloxDataReaderHandle flox_data_reader_create(const char* data_dir)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  FLOX_CAPI_REQUIRE(data_dir);
  replay::ReaderConfig cfg;
  cfg.data_dir = data_dir;
  return new replay::BinaryLogReader(cfg);
  FLOX_CAPI_LEAVE;
}

void flox_data_reader_destroy(FloxDataReaderHandle h)
{
  FLOX_CAPI_ENTER_DESTROY(h);
  delete static_cast<replay::BinaryLogReader*>(h);
  FLOX_CAPI_LEAVE_VOID;
}

uint64_t flox_data_reader_count(FloxDataReaderHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<replay::BinaryLogReader*>(h)->count();
  FLOX_CAPI_LEAVE;
}

// ============================================================
// Order book level access
// ============================================================

uint32_t flox_book_get_bids(FloxBookHandle h, double* prices_out, double* qtys_out,
                            uint32_t max_levels)
{
  FLOX_CAPI_ENTER(h);
  auto levels = static_cast<FloxBookImpl*>(h)->book.getBidLevels(max_levels);
  uint32_t count = static_cast<uint32_t>(levels.size());
  for (uint32_t i = 0; i < count; i++)
  {
    prices_out[i] = levels[i].price.toDouble();
    qtys_out[i] = levels[i].quantity.toDouble();
  }
  return count;
  FLOX_CAPI_LEAVE;
}

uint32_t flox_book_get_asks(FloxBookHandle h, double* prices_out, double* qtys_out,
                            uint32_t max_levels)
{
  FLOX_CAPI_ENTER(h);
  auto levels = static_cast<FloxBookImpl*>(h)->book.getAskLevels(max_levels);
  uint32_t count = static_cast<uint32_t>(levels.size());
  for (uint32_t i = 0; i < count; i++)
  {
    prices_out[i] = levels[i].price.toDouble();
    qtys_out[i] = levels[i].quantity.toDouble();
  }
  return count;
  FLOX_CAPI_LEAVE;
}

// ============================================================
// Heikin-Ashi bar aggregation
// ============================================================

uint32_t flox_aggregate_heikin_ashi_bars(const int64_t* ts, const double* px, const double* qty,
                                         const uint8_t* ib, size_t len, double interval_seconds,
                                         FloxBar* bars_out, uint32_t max_bars)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  HeikinAshiBarPolicy policy(
      std::chrono::nanoseconds(static_cast<int64_t>(interval_seconds * 1e9)));
  return doAggregateC(policy, ts, px, qty, ib, len, bars_out, max_bars);
  FLOX_CAPI_LEAVE;
}

// ============================================================
// Additional stats
// ============================================================

double flox_stat_permutation_test(const double* group1, size_t len1, const double* group2,
                                  size_t len2, uint32_t num_permutations)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  auto mean = [](const double* d, size_t n)
  {
    double s = 0;
    for (size_t i = 0; i < n; i++)
    {
      s += d[i];
    }
    return n > 0 ? s / static_cast<double>(n) : 0.0;
  };

  double observed = std::abs(mean(group1, len1) - mean(group2, len2));

  std::vector<double> combined(len1 + len2);
  std::copy(group1, group1 + len1, combined.begin());
  std::copy(group2, group2 + len2, combined.begin() + len1);

  std::mt19937 rng(42);
  uint32_t count = 0;
  for (uint32_t i = 0; i < num_permutations; i++)
  {
    std::shuffle(combined.begin(), combined.end(), rng);
    double diff = std::abs(mean(combined.data(), len1) - mean(combined.data() + len1, len2));
    if (diff >= observed)
    {
      count++;
    }
  }
  return static_cast<double>(count) / static_cast<double>(num_permutations);
  FLOX_CAPI_LEAVE;
}

void flox_stat_bootstrap_ci(const double* data, size_t len, double confidence,
                            uint32_t num_samples, double* lower_out, double* median_out,
                            double* upper_out)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  std::mt19937 rng(42);
  std::uniform_int_distribution<size_t> dist(0, len - 1);

  std::vector<double> means(num_samples);
  for (uint32_t s = 0; s < num_samples; s++)
  {
    double sum = 0;
    for (size_t i = 0; i < len; i++)
    {
      sum += data[dist(rng)];
    }
    means[s] = sum / static_cast<double>(len);
  }
  std::sort(means.begin(), means.end());

  double alpha = (1.0 - confidence) / 2.0;
  size_t lo = static_cast<size_t>(alpha * num_samples);
  size_t hi = static_cast<size_t>((1.0 - alpha) * num_samples);
  size_t mid = num_samples / 2;

  *lower_out = means[lo];
  *median_out = means[mid];
  *upper_out = means[std::min(hi, static_cast<size_t>(num_samples - 1))];
  FLOX_CAPI_LEAVE_VOID;
}

void flox_stat_whites_reality_check(const double* returns, size_t num_strategies,
                                    size_t num_periods, uint32_t num_bootstrap,
                                    double avg_block_size, double* p_value_out,
                                    double* best_stat_out, int32_t* best_index_out)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  auto result = flox::stats::whitesRealityCheck(
      returns, num_strategies, num_periods, num_bootstrap, avg_block_size);
  if (p_value_out)
  {
    *p_value_out = result.pValue;
  }
  if (best_stat_out)
  {
    *best_stat_out = result.bestStat;
  }
  if (best_index_out)
  {
    *best_index_out = result.bestIndex;
  }
  FLOX_CAPI_LEAVE_VOID;
}

// ============================================================
// Segment operations
// ============================================================

uint8_t flox_segment_validate(const char* path)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  FLOX_CAPI_REQUIRE(path);
  replay::SegmentValidator validator;
  auto result = validator.validate(path);
  return result.valid ? 1 : 0;
  FLOX_CAPI_LEAVE;
}

uint8_t flox_segment_merge(const char* input_dir, const char* output_path)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  FLOX_CAPI_REQUIRE(input_dir);
  FLOX_CAPI_REQUIRE(output_path);
  replay::MergeConfig cfg;
  cfg.output_dir = output_path;
  auto result = replay::SegmentOps::mergeDirectory(input_dir, cfg);
  return result.segments_merged > 0 ? 1 : 0;
  FLOX_CAPI_LEAVE;
}

// ============================================================
// Backtest: slippage, queue, result, metrics, equity curve
// ============================================================

namespace
{
SlippageProfile makeSlippageProfile(int32_t model, int32_t ticks, double tick_size,
                                    double bps, double impact_coeff)
{
  SlippageProfile prof;
  prof.model = static_cast<SlippageModel>(model);
  prof.ticks = ticks;
  prof.tickSize = (tick_size > 0.0) ? Price::fromDouble(tick_size) : Price{};
  prof.bps = bps;
  prof.impactCoeff = impact_coeff;
  return prof;
}
}  // namespace

void flox_simulated_executor_set_default_slippage(FloxSimulatedExecutorHandle h, int32_t model, int32_t ticks,
                                                  double tick_size, double bps, double impact_coeff)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setDefaultSlippage(
      makeSlippageProfile(model, ticks, tick_size, bps, impact_coeff));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_set_symbol_slippage(FloxSimulatedExecutorHandle h, uint32_t symbol, int32_t model,
                                                 int32_t ticks, double tick_size, double bps,
                                                 double impact_coeff)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setSymbolSlippage(
      symbol, makeSlippageProfile(model, ticks, tick_size, bps, impact_coeff));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_set_queue_model(FloxSimulatedExecutorHandle h, int32_t model, uint32_t depth)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setQueueModel(
      static_cast<QueueModel>(model), depth);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_set_queue_fifo_top_n(FloxSimulatedExecutorHandle h, uint32_t top_n)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setQueueFifoTopN(top_n);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_set_top_priority_share(FloxSimulatedExecutorHandle h,
                                                    double share)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setTopPriorityShare(share);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_set_lmm_orders(FloxSimulatedExecutorHandle h,
                                            const uint64_t* ids, uint32_t n_ids)
{
  FLOX_CAPI_ENTER_VOID(h);
  std::vector<OrderId> v;
  v.reserve(n_ids);
  for (uint32_t i = 0; i < n_ids; ++i)
  {
    v.push_back(static_cast<OrderId>(ids ? ids[i] : 0));
  }
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setLmmOrders(v);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_set_lmm_bonus_multiplier(FloxSimulatedExecutorHandle h,
                                                      double multiplier)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setLmmBonusMultiplier(multiplier);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_set_order_priority_multiplier(FloxSimulatedExecutorHandle h,
                                                           uint64_t order_id,
                                                           double multiplier)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setOrderPriorityMultiplier(
      static_cast<OrderId>(order_id), multiplier);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_set_submit_ack_latency(FloxSimulatedExecutorHandle h,
                                                    int64_t latency_ns, int64_t jitter_ns)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setSubmitAckLatency(latency_ns, jitter_ns);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_set_cancel_ack_latency(FloxSimulatedExecutorHandle h,
                                                    int64_t latency_ns, int64_t jitter_ns)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setCancelAckLatency(latency_ns, jitter_ns);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_set_replace_ack_latency(FloxSimulatedExecutorHandle h,
                                                     int64_t latency_ns, int64_t jitter_ns)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setReplaceAckLatency(latency_ns, jitter_ns);
  FLOX_CAPI_LEAVE_VOID;
}

int flox_simulated_executor_apply_latency_profile(FloxSimulatedExecutorHandle h,
                                                  const char* profile_name)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<FloxSimulatedExecutorImpl*>(h)->executor.applyLatencyProfile(
             profile_name)
             ? 1
             : 0;
  FLOX_CAPI_LEAVE;
}

void flox_simulated_executor_set_stp_mode(FloxSimulatedExecutorHandle h, uint8_t mode)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setSTPMode(
      static_cast<STPMode>(mode));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_set_fok_mode(FloxSimulatedExecutorHandle h, uint8_t mode)
{
  FLOX_CAPI_ENTER_VOID(h);
  const SimulatedExecutor::FokMode m =
      (mode == 1) ? SimulatedExecutor::FokMode::SinglePrice
                  : SimulatedExecutor::FokMode::AnyPrice;
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setFokMode(m);
  FLOX_CAPI_LEAVE_VOID;
}

uint8_t flox_simulated_executor_fok_mode(FloxSimulatedExecutorHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint8_t>(
      static_cast<FloxSimulatedExecutorImpl*>(h)->executor.fokMode());
  FLOX_CAPI_LEAVE;
}

void flox_simulated_executor_set_stp_group_membership(FloxSimulatedExecutorHandle h,
                                                      uint64_t account_id,
                                                      uint64_t group_id)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setSTPGroupMembership(
      account_id, group_id);
  FLOX_CAPI_LEAVE_VOID;
}

uint64_t flox_simulated_executor_stp_group_for(FloxSimulatedExecutorHandle h,
                                               uint64_t account_id)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<FloxSimulatedExecutorImpl*>(h)->executor.stpGroupFor(account_id);
  FLOX_CAPI_LEAVE;
}

// Latency distribution handle bridging.

namespace
{
inline LatencyDistribution* toDist(FloxLatencyDistributionHandle h)
{
  return static_cast<LatencyDistribution*>(h);
}
}  // namespace

FloxLatencyDistributionHandle flox_latency_distribution_create(void)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return new LatencyDistribution();
  FLOX_CAPI_LEAVE;
}

void flox_latency_distribution_destroy(FloxLatencyDistributionHandle h)
{
  FLOX_CAPI_ENTER_DESTROY(h);
  delete toDist(h);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_latency_distribution_set_constant(FloxLatencyDistributionHandle h, int64_t ns)
{
  FLOX_CAPI_ENTER_VOID(h);
  const double rho = toDist(h)->burstCorrelation();
  *toDist(h) = LatencyDistribution::constant(ns);
  toDist(h)->setBurstCorrelation(rho);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_latency_distribution_set_uniform(FloxLatencyDistributionHandle h, int64_t lo_ns,
                                           int64_t hi_ns)
{
  FLOX_CAPI_ENTER_VOID(h);
  const double rho = toDist(h)->burstCorrelation();
  *toDist(h) = LatencyDistribution::uniform(lo_ns, hi_ns);
  toDist(h)->setBurstCorrelation(rho);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_latency_distribution_set_lognormal(FloxLatencyDistributionHandle h,
                                             int64_t median_ns, double sigma)
{
  FLOX_CAPI_ENTER_VOID(h);
  const double rho = toDist(h)->burstCorrelation();
  *toDist(h) = LatencyDistribution::lognormal(median_ns, sigma);
  toDist(h)->setBurstCorrelation(rho);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_latency_distribution_set_empirical(FloxLatencyDistributionHandle h,
                                             const int64_t* samples_ns, uint32_t n_samples)
{
  FLOX_CAPI_ENTER_VOID(h);
  const double rho = toDist(h)->burstCorrelation();
  std::vector<int64_t> samples;
  samples.reserve(n_samples);
  for (uint32_t i = 0; i < n_samples; ++i)
  {
    samples.push_back(samples_ns ? samples_ns[i] : 0);
  }
  *toDist(h) = LatencyDistribution::empirical(samples);
  toDist(h)->setBurstCorrelation(rho);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_latency_distribution_set_burst_correlation(FloxLatencyDistributionHandle h, double rho)
{
  FLOX_CAPI_ENTER_VOID(h);
  toDist(h)->setBurstCorrelation(rho);
  FLOX_CAPI_LEAVE_VOID;
}

int64_t flox_latency_distribution_median_ns(FloxLatencyDistributionHandle h)
{
  FLOX_CAPI_ENTER(h);
  return toDist(h)->medianNs();
  FLOX_CAPI_LEAVE;
}

void flox_simulated_executor_set_submit_ack_latency_distribution(
    FloxSimulatedExecutorHandle h, FloxLatencyDistributionHandle dist)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setSubmitAckLatencyDistribution(
      *toDist(dist));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_set_cancel_ack_latency_distribution(
    FloxSimulatedExecutorHandle h, FloxLatencyDistributionHandle dist)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setCancelAckLatencyDistribution(
      *toDist(dist));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_set_replace_ack_latency_distribution(
    FloxSimulatedExecutorHandle h, FloxLatencyDistributionHandle dist)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.setReplaceAckLatencyDistribution(
      *toDist(dist));
  FLOX_CAPI_LEAVE_VOID;
}

// ====== Rate-limit policy bridging ======

#include "flox/backtest/rate_limit_policy.h"
#include "flox/backtest/venue_availability.h"

namespace
{
inline flox::RateLimitPolicy* toRateLimit(FloxRateLimitPolicyHandle h)
{
  return static_cast<flox::RateLimitPolicy*>(h);
}
}  // namespace

extern "C" FloxRateLimitPolicyHandle flox_rate_limit_policy_create(void)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return new flox::RateLimitPolicy();
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_rate_limit_policy_destroy(FloxRateLimitPolicyHandle h)
{
  FLOX_CAPI_ENTER_DESTROY(h);
  delete toRateLimit(h);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_rate_limit_policy_add_bucket(FloxRateLimitPolicyHandle h,
                                                  const char* name, int64_t window_ns,
                                                  uint32_t capacity, uint32_t submit_w,
                                                  uint32_t cancel_w, uint32_t replace_w)
{
  FLOX_CAPI_ENTER_VOID(h);
  toRateLimit(h)->addBucket(name ? std::string(name) : std::string("bucket"), window_ns,
                            capacity, submit_w, cancel_w, replace_w);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_rate_limit_policy_add_bucket_family(
    FloxRateLimitPolicyHandle h, const char* name, int64_t window_ns,
    uint32_t capacity, uint32_t submit_w, uint32_t cancel_w, uint32_t replace_w,
    uint8_t family, uint32_t query_w)
{
  FLOX_CAPI_ENTER_VOID(h);
  RateLimitPolicy::EndpointFamily f = RateLimitPolicy::EndpointFamily::Trading;
  switch (family)
  {
    case 1:
      f = RateLimitPolicy::EndpointFamily::MarketData;
      break;
    case 2:
      f = RateLimitPolicy::EndpointFamily::Account;
      break;
    default:
      f = RateLimitPolicy::EndpointFamily::Trading;
      break;
  }
  toRateLimit(h)->addBucket(name ? std::string(name) : std::string("bucket"), window_ns,
                            capacity, submit_w, cancel_w, replace_w, f, query_w);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_rate_limit_policy_set_ban(FloxRateLimitPolicyHandle h,
                                               uint32_t after_consecutive_rejects,
                                               int64_t ban_duration_ns)
{
  FLOX_CAPI_ENTER_VOID(h);
  toRateLimit(h)->setBan(after_consecutive_rejects, ban_duration_ns);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" int flox_rate_limit_policy_load_profile(FloxRateLimitPolicyHandle h,
                                                   const char* name)
{
  FLOX_CAPI_ENTER(h);
  if (!h || !name)
  {
    return 0;
  }
  std::string n = name;
  if (n == "binance_um_futures")
  {
    *toRateLimit(h) = flox::RateLimitPolicy::binance_um_futures();
  }
  else if (n == "bybit_linear")
  {
    *toRateLimit(h) = flox::RateLimitPolicy::bybit_linear();
  }
  else if (n == "okx_swap")
  {
    *toRateLimit(h) = flox::RateLimitPolicy::okx_swap();
  }
  else if (n == "deribit")
  {
    *toRateLimit(h) = flox::RateLimitPolicy::deribit();
  }
  else
  {
    return 0;
  }
  return 1;
  FLOX_CAPI_LEAVE;
}

extern "C" int64_t flox_rate_limit_policy_ban_until_ns(FloxRateLimitPolicyHandle h)
{
  FLOX_CAPI_ENTER(h);
  return toRateLimit(h)->banUntilNs();
  FLOX_CAPI_LEAVE;
}

extern "C" uint32_t flox_rate_limit_policy_consecutive_rejects(FloxRateLimitPolicyHandle h)
{
  FLOX_CAPI_ENTER(h);
  return toRateLimit(h)->consecutiveRejects();
  FLOX_CAPI_LEAVE;
}

extern "C" uint32_t flox_rate_limit_policy_bucket_state(FloxRateLimitPolicyHandle h,
                                                        int64_t now_ns, int64_t* out_buf,
                                                        uint32_t max_buckets)
{
  FLOX_CAPI_ENTER(h);
  auto states = toRateLimit(h)->bucketStates(now_ns);
  if (out_buf == nullptr || max_buckets == 0)
  {
    return static_cast<uint32_t>(states.size());
  }
  uint32_t n = std::min<uint32_t>(max_buckets, static_cast<uint32_t>(states.size()));
  for (uint32_t i = 0; i < n; ++i)
  {
    out_buf[i * 4 + 0] = states[i].windowNs;
    out_buf[i * 4 + 1] = static_cast<int64_t>(states[i].used);
    out_buf[i * 4 + 2] = static_cast<int64_t>(states[i].capacity);
    out_buf[i * 4 + 3] = 0;
  }
  return n;
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_simulated_executor_set_rate_limit_policy(
    FloxSimulatedExecutorHandle exec_h, FloxRateLimitPolicyHandle policy_h)
{
  FLOX_CAPI_ENTER_VOID(exec_h);
  static_cast<FloxSimulatedExecutorImpl*>(exec_h)->executor.setRateLimitPolicy(
      *toRateLimit(policy_h));
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_simulated_executor_clear_rate_limit_policy(
    FloxSimulatedExecutorHandle exec_h)
{
  FLOX_CAPI_ENTER_VOID(exec_h);
  static_cast<FloxSimulatedExecutorImpl*>(exec_h)->executor.clearRateLimitPolicy();
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" FloxVenueAvailabilityHandle flox_venue_availability_create(void)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return new VenueAvailability();
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_venue_availability_destroy(FloxVenueAvailabilityHandle h)
{
  FLOX_CAPI_ENTER_DESTROY(h);
  delete static_cast<VenueAvailability*>(h);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_venue_availability_schedule_outage(FloxVenueAvailabilityHandle h,
                                                        int64_t start_ns, int64_t duration_ns,
                                                        uint8_t policy, int64_t gtc_ttl_ns)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<VenueAvailability*>(h)->scheduleOutage(start_ns, duration_ns,
                                                     static_cast<OnOutage>(policy),
                                                     gtc_ttl_ns);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_venue_availability_auto_random_outages(FloxVenueAvailabilityHandle h,
                                                            double per_day,
                                                            int64_t mean_duration_ns,
                                                            uint8_t policy, uint64_t seed)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<VenueAvailability*>(h)->autoRandomOutages(per_day, mean_duration_ns,
                                                        static_cast<OnOutage>(policy),
                                                        seed);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" uint8_t flox_venue_availability_is_up(FloxVenueAvailabilityHandle h, int64_t now_ns)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<VenueAvailability*>(h)->isUp(now_ns) ? 1 : 0;
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_venue_availability_schedule_outage_ex(
    FloxVenueAvailabilityHandle h, int64_t start_ns, int64_t duration_ns,
    uint8_t outage_type, uint8_t policy, int64_t gtc_ttl_ns,
    double degradation_latency_multiplier, double wrong_side_recovery_bps)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<VenueAvailability*>(h)->scheduleOutageEx(
      start_ns, duration_ns, static_cast<OutageType>(outage_type),
      static_cast<OnOutage>(policy), gtc_ttl_ns,
      degradation_latency_multiplier, wrong_side_recovery_bps);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" uint8_t flox_venue_availability_submits_allowed(FloxVenueAvailabilityHandle h,
                                                           int64_t now_ns)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<VenueAvailability*>(h)->submitsAllowed(now_ns) ? 1 : 0;
  FLOX_CAPI_LEAVE;
}

extern "C" uint8_t flox_venue_availability_cancels_allowed(FloxVenueAvailabilityHandle h,
                                                           int64_t now_ns)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<VenueAvailability*>(h)->cancelsAllowed(now_ns) ? 1 : 0;
  FLOX_CAPI_LEAVE;
}

extern "C" uint8_t flox_venue_availability_book_updates_allowed(
    FloxVenueAvailabilityHandle h, int64_t now_ns)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<VenueAvailability*>(h)->bookUpdatesAllowed(now_ns) ? 1 : 0;
  FLOX_CAPI_LEAVE;
}

extern "C" uint8_t flox_venue_availability_trades_allowed(FloxVenueAvailabilityHandle h,
                                                          int64_t now_ns)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<VenueAvailability*>(h)->tradesAllowed(now_ns) ? 1 : 0;
  FLOX_CAPI_LEAVE;
}

extern "C" double flox_venue_availability_latency_multiplier(
    FloxVenueAvailabilityHandle h, int64_t now_ns)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<VenueAvailability*>(h)->latencyMultiplier(now_ns);
  FLOX_CAPI_LEAVE;
}

extern "C" double flox_venue_availability_consume_wrong_side_recovery_bps(
    FloxVenueAvailabilityHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<VenueAvailability*>(h)->consumeWrongSideRecoveryBps();
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_simulated_executor_set_venue_availability(
    FloxSimulatedExecutorHandle exec_h, FloxVenueAvailabilityHandle va_h)
{
  FLOX_CAPI_ENTER_VOID(exec_h);
  static_cast<FloxSimulatedExecutorImpl*>(exec_h)->executor.setVenueAvailability(
      static_cast<VenueAvailability*>(va_h));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_on_trade_qty(FloxSimulatedExecutorHandle h, uint32_t symbol, double price,
                                          double quantity, uint8_t is_buy)
{
  FLOX_CAPI_ENTER_VOID(h);
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.onTrade(
      symbol, Price::fromDouble(price), Quantity::fromDouble(quantity), is_buy != 0);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_on_best_levels(FloxSimulatedExecutorHandle h, uint32_t symbol, double bid_price,
                                            double bid_qty, double ask_price, double ask_qty)
{
  FLOX_CAPI_ENTER_VOID(h);
  std::pmr::monotonic_buffer_resource pool(512);
  std::pmr::vector<BookLevel> bids(&pool);
  std::pmr::vector<BookLevel> asks(&pool);
  bids.emplace_back(Price::fromDouble(bid_price), Quantity::fromDouble(bid_qty));
  asks.emplace_back(Price::fromDouble(ask_price), Quantity::fromDouble(ask_qty));
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.onBookUpdate(symbol, bids, asks);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_simulated_executor_on_book_snapshot(FloxSimulatedExecutorHandle h, uint32_t symbol,
                                              const double* bid_prices, const double* bid_qtys,
                                              uint32_t n_bids, const double* ask_prices,
                                              const double* ask_qtys, uint32_t n_asks)
{
  FLOX_CAPI_ENTER_VOID(h);
  std::pmr::monotonic_buffer_resource pool(1024);
  std::pmr::vector<BookLevel> bids(&pool);
  std::pmr::vector<BookLevel> asks(&pool);
  bids.reserve(n_bids);
  asks.reserve(n_asks);
  for (uint32_t i = 0; i < n_bids; ++i)
  {
    bids.emplace_back(Price::fromDouble(bid_prices[i]), Quantity::fromDouble(bid_qtys[i]));
  }
  for (uint32_t i = 0; i < n_asks; ++i)
  {
    asks.emplace_back(Price::fromDouble(ask_prices[i]), Quantity::fromDouble(ask_qtys[i]));
  }
  static_cast<FloxSimulatedExecutorImpl*>(h)->executor.onBookUpdate(symbol, bids, asks);
  FLOX_CAPI_LEAVE_VOID;
}

struct FloxBacktestResultImpl
{
  BacktestConfig config;
  std::unique_ptr<BacktestResult> result;
};

FloxBacktestResultHandle flox_backtest_result_create(double initial_capital, double fee_rate,
                                                     uint8_t use_percentage_fee,
                                                     double fixed_fee_per_trade,
                                                     double risk_free_rate,
                                                     double annualization_factor)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  auto* impl = new FloxBacktestResultImpl();
  impl->config.initialCapital = initial_capital;
  impl->config.feeRate = fee_rate;
  impl->config.usePercentageFee = use_percentage_fee != 0;
  impl->config.fixedFeePerTrade = fixed_fee_per_trade;
  impl->config.riskFreeRate = risk_free_rate;
  impl->config.metricsAnnualizationFactor =
      (annualization_factor > 0.0) ? annualization_factor : 252.0;
  impl->result = std::make_unique<BacktestResult>(impl->config);
  return impl;
  FLOX_CAPI_LEAVE;
}

void flox_backtest_result_destroy(FloxBacktestResultHandle h)
{
  FLOX_CAPI_ENTER_DESTROY(h);
  delete static_cast<FloxBacktestResultImpl*>(h);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_backtest_result_record_fill(FloxBacktestResultHandle h, uint64_t order_id,
                                      uint32_t symbol, uint8_t side, double price,
                                      double quantity, int64_t timestamp_ns)
{
  FLOX_CAPI_ENTER_VOID(h);
  Fill fill{};
  fill.orderId = order_id;
  fill.symbol = symbol;
  fill.side = (side == 0) ? Side::BUY : Side::SELL;
  fill.price = Price::fromDouble(price);
  fill.quantity = Quantity::fromDouble(quantity);
  fill.timestampNs = UnixNanos::fromRaw(timestamp_ns);
  static_cast<FloxBacktestResultImpl*>(h)->result->recordFill(fill);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_backtest_result_ingest_executor(FloxBacktestResultHandle h, FloxSimulatedExecutorHandle eh)
{
  FLOX_CAPI_ENTER_VOID(h);
  auto* impl = static_cast<FloxBacktestResultImpl*>(h);
  for (const auto& fill : static_cast<FloxSimulatedExecutorImpl*>(eh)->executor.fills())
  {
    impl->result->recordFill(fill);
  }
  FLOX_CAPI_LEAVE_VOID;
}

void flox_backtest_result_stats(FloxBacktestResultHandle h, FloxBacktestStats* out)
{
  FLOX_CAPI_ENTER_VOID(h);
  if (!out)
  {
    return;
  }
  auto stats = static_cast<FloxBacktestResultImpl*>(h)->result->computeStats();
  out->totalTrades = stats.totalTrades;
  out->winningTrades = stats.winningTrades;
  out->losingTrades = stats.losingTrades;
  out->maxConsecutiveWins = stats.maxConsecutiveWins;
  out->maxConsecutiveLosses = stats.maxConsecutiveLosses;
  out->initialCapital = stats.initialCapital;
  out->finalCapital = stats.finalCapital;
  out->totalPnl = stats.totalPnl;
  out->totalFees = stats.totalFees;
  out->netPnl = stats.netPnl;
  out->grossProfit = stats.grossProfit;
  out->grossLoss = stats.grossLoss;
  out->maxDrawdown = stats.maxDrawdown;
  out->maxDrawdownPct = stats.maxDrawdownPct;
  out->winRate = stats.winRate;
  out->profitFactor = stats.profitFactor;
  out->avgWin = stats.avgWin;
  out->avgLoss = stats.avgLoss;
  out->avgWinLossRatio = stats.avgWinLossRatio;
  out->avgTradeDurationNs = stats.avgTradeDurationNs;
  out->medianTradeDurationNs = stats.medianTradeDurationNs;
  out->maxTradeDurationNs = stats.maxTradeDurationNs;
  out->sharpeRatio = stats.sharpeRatio;
  out->sortinoRatio = stats.sortinoRatio;
  out->calmarRatio = stats.calmarRatio;
  out->timeWeightedReturn = stats.timeWeightedReturn;
  out->returnPct = stats.returnPct;
  out->startTimeNs = stats.startTimeNs.raw();
  out->endTimeNs = stats.endTimeNs.raw();
  FLOX_CAPI_LEAVE_VOID;
}

uint32_t flox_backtest_result_equity_curve(FloxBacktestResultHandle h, FloxEquityPoint* out,
                                           uint32_t max_points)
{
  FLOX_CAPI_ENTER(h);
  const auto& curve = static_cast<FloxBacktestResultImpl*>(h)->result->equityCurve();
  const uint32_t total = static_cast<uint32_t>(curve.size());
  if (!out)
  {
    return total;
  }
  const uint32_t n = (total < max_points) ? total : max_points;
  for (uint32_t i = 0; i < n; ++i)
  {
    out[i].timestamp_ns = curve[i].timestampNs.raw();
    out[i].equity = curve[i].equity;
    out[i].drawdown_pct = curve[i].drawdownPct;
  }
  return n;
  FLOX_CAPI_LEAVE;
}

uint8_t flox_backtest_result_write_equity_curve_csv(FloxBacktestResultHandle h, const char* path)
{
  FLOX_CAPI_ENTER(h);
  if (!path)
  {
    return 0;
  }
  return static_cast<FloxBacktestResultImpl*>(h)->result->writeEquityCurveCsv(path) ? 1 : 0;
  FLOX_CAPI_LEAVE;
}

uint32_t flox_backtest_result_trades(FloxBacktestResultHandle h, FloxBacktestTrade* out,
                                     uint32_t max_trades)
{
  FLOX_CAPI_ENTER(h);
  const auto& trades = static_cast<FloxBacktestResultImpl*>(h)->result->trades();
  const uint32_t total = static_cast<uint32_t>(trades.size());
  if (!out)
  {
    return total;
  }
  const uint32_t n = (total < max_trades) ? total : max_trades;
  for (uint32_t i = 0; i < n; ++i)
  {
    const auto& t = trades[i];
    out[i].symbol = static_cast<uint32_t>(t.symbol);
    out[i].side = static_cast<uint8_t>(t.side);
    out[i].entry_price = t.entryPrice.toDouble();
    out[i].exit_price = t.exitPrice.toDouble();
    out[i].quantity = t.quantity.toDouble();
    out[i].entry_time_ns = t.entryTimeNs.raw();
    out[i].exit_time_ns = t.exitTimeNs.raw();
    out[i].pnl = t.pnl.toDouble();
    out[i].fee = t.fee.toDouble();
  }
  return n;
  FLOX_CAPI_LEAVE;
}

// ============================================================
// Segment operations (full API)
// ============================================================

static replay::CompressionType toCompression(uint8_t c)
{
  return c == 1 ? replay::CompressionType::LZ4 : replay::CompressionType::None;
}

FloxMergeResult flox_segment_merge_full(const char* input_paths, size_t num_paths,
                                        const char* output_dir, const char* output_name,
                                        uint8_t sort)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  FLOX_CAPI_REQUIRE(input_paths);
  FLOX_CAPI_REQUIRE(output_dir);
  std::vector<std::filesystem::path> paths;
  const char* p = input_paths;
  for (size_t i = 0; i < num_paths; ++i)
  {
    paths.emplace_back(p);
    p += std::strlen(p) + 1;
  }
  replay::MergeConfig cfg;
  cfg.output_dir = output_dir;
  cfg.output_name = output_name ? output_name : "merged";
  cfg.sort_by_timestamp = sort != 0;
  auto r = replay::SegmentOps::merge(paths, cfg);
  return {r.success ? (uint8_t)1 : (uint8_t)0, r.segments_merged, r.events_written,
          r.bytes_written};
  FLOX_CAPI_LEAVE;
}

FloxMergeResult flox_segment_merge_dir(const char* input_dir, const char* output_dir)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  FLOX_CAPI_REQUIRE(input_dir);
  FLOX_CAPI_REQUIRE(output_dir);
  auto r = replay::quickMerge(input_dir, output_dir);
  return {r.success ? (uint8_t)1 : (uint8_t)0, r.segments_merged, r.events_written,
          r.bytes_written};
  FLOX_CAPI_LEAVE;
}

FloxSplitResult flox_segment_split(const char* input_path, const char* output_dir, uint8_t mode,
                                   int64_t time_interval_ns, uint64_t events_per_file)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  FLOX_CAPI_REQUIRE(input_path);
  FLOX_CAPI_REQUIRE(output_dir);
  replay::SplitConfig cfg;
  cfg.output_dir = output_dir;
  cfg.mode = static_cast<replay::SplitMode>(mode);
  cfg.time_interval_ns = time_interval_ns;
  cfg.events_per_file = events_per_file;
  auto r = replay::SegmentOps::split(input_path, cfg);
  return {r.success ? (uint8_t)1 : (uint8_t)0, r.segments_created, r.events_written};
  FLOX_CAPI_LEAVE;
}

FloxExportResult flox_segment_export(const char* input_path, const char* output_path,
                                     uint8_t format, int64_t from_ns, int64_t to_ns,
                                     const uint32_t* symbols, uint32_t num_symbols)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  FLOX_CAPI_REQUIRE(input_path);
  FLOX_CAPI_REQUIRE(output_path);
  replay::ExportConfig cfg;
  cfg.output_path = output_path;
  cfg.format = static_cast<replay::ExportFormat>(format);
  if (from_ns > 0)
  {
    cfg.from_ts = from_ns;
  }
  if (to_ns > 0)
  {
    cfg.to_ts = to_ns;
  }
  if (symbols && num_symbols > 0)
  {
    cfg.symbols.insert(symbols, symbols + num_symbols);
  }
  auto r = replay::SegmentOps::exportData(input_path, cfg);
  return {r.success ? (uint8_t)1 : (uint8_t)0, r.events_exported, r.bytes_written};
  FLOX_CAPI_LEAVE;
}

uint8_t flox_segment_recompress(const char* input_path, const char* output_path,
                                uint8_t compression)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  FLOX_CAPI_REQUIRE(input_path);
  FLOX_CAPI_REQUIRE(output_path);
  return replay::SegmentOps::recompress(input_path, output_path, toCompression(compression)) ? 1
                                                                                             : 0;
  FLOX_CAPI_LEAVE;
}

uint64_t flox_segment_extract_symbols(const char* input_path, const char* output_path,
                                      const uint32_t* symbols, uint32_t num_symbols)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  FLOX_CAPI_REQUIRE(input_path);
  FLOX_CAPI_REQUIRE(output_path);
  std::set<uint32_t> symSet(symbols, symbols + num_symbols);
  replay::WriterConfig wc;
  auto out = std::filesystem::path(output_path);
  wc.output_dir = out.parent_path();
  wc.output_filename = out.filename().string();
  return replay::SegmentOps::extractSymbols(input_path, output_path, symSet, wc);
  FLOX_CAPI_LEAVE;
}

uint64_t flox_segment_extract_time_range(const char* input_path, const char* output_path,
                                         int64_t from_ns, int64_t to_ns)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  FLOX_CAPI_REQUIRE(input_path);
  FLOX_CAPI_REQUIRE(output_path);
  replay::WriterConfig wc;
  auto out = std::filesystem::path(output_path);
  wc.output_dir = out.parent_path();
  wc.output_filename = out.filename().string();
  return replay::SegmentOps::extractTimeRange(input_path, output_path, from_ns, to_ns, wc);
  FLOX_CAPI_LEAVE;
}

// ============================================================
// Validation (full API)
// ============================================================

FloxSegmentValidation flox_segment_validate_full(const char* path, uint8_t verify_crc,
                                                 uint8_t verify_timestamps)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  FLOX_CAPI_REQUIRE(path);
  replay::ValidatorConfig cfg;
  cfg.verify_crc = verify_crc != 0;
  cfg.verify_timestamps = verify_timestamps != 0;
  replay::SegmentValidator validator(cfg);
  auto r = validator.validate(path);
  return {r.valid ? (uint8_t)1 : (uint8_t)0,
          r.header_valid ? (uint8_t)1 : (uint8_t)0,
          r.reported_event_count,
          r.actual_event_count,
          r.has_index ? (uint8_t)1 : (uint8_t)0,
          r.index_valid ? (uint8_t)1 : (uint8_t)0,
          r.trades_found,
          r.book_updates_found,
          r.crc_errors,
          r.timestamp_anomalies};
  FLOX_CAPI_LEAVE;
}

FloxDatasetValidation flox_dataset_validate(const char* data_dir)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  FLOX_CAPI_REQUIRE(data_dir);
  replay::DatasetValidator validator;
  auto r = validator.validate(data_dir);
  return {r.valid ? (uint8_t)1 : (uint8_t)0, r.total_segments, r.valid_segments,
          r.corrupted_segments, r.total_events, r.total_bytes,
          r.first_timestamp, r.last_timestamp};
  FLOX_CAPI_LEAVE;
}

// ============================================================
// DataReader (full API)
// ============================================================

FloxDataReaderHandle flox_data_reader_create_filtered(const char* data_dir, int64_t from_ns,
                                                      int64_t to_ns, const uint32_t* symbols,
                                                      uint32_t num_symbols)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  FLOX_CAPI_REQUIRE(data_dir);
  replay::ReaderConfig cfg;
  cfg.data_dir = data_dir;
  if (from_ns > 0)
  {
    cfg.from_ns = from_ns;
  }
  if (to_ns > 0)
  {
    cfg.to_ns = to_ns;
  }
  if (symbols && num_symbols > 0)
  {
    cfg.symbols.insert(symbols, symbols + num_symbols);
  }
  return new replay::BinaryLogReader(cfg);
  FLOX_CAPI_LEAVE;
}

FloxDataReaderHandle flox_data_reader_create_ordered(const char* data_dir, int64_t from_ns,
                                                     int64_t to_ns, const uint32_t* symbols,
                                                     uint32_t num_symbols,
                                                     int64_t reorder_window_ns,
                                                     int32_t strict_ordering)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  FLOX_CAPI_REQUIRE(data_dir);
  replay::ReaderConfig cfg;
  cfg.data_dir = data_dir;
  if (from_ns > 0)
  {
    cfg.from_ns = from_ns;
  }
  if (to_ns > 0)
  {
    cfg.to_ns = to_ns;
  }
  if (symbols && num_symbols > 0)
  {
    cfg.symbols.insert(symbols, symbols + num_symbols);
  }
  if (reorder_window_ns > 0)
  {
    cfg.reorder_window_ns = reorder_window_ns;
  }
  cfg.strict_ordering = strict_ordering != 0;
  return new replay::BinaryLogReader(cfg);
  FLOX_CAPI_LEAVE;
}

FloxDatasetSummary flox_data_reader_summary(FloxDataReaderHandle h)
{
  FLOX_CAPI_ENTER(h);
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  auto s = reader->summary();
  return {s.first_event_ns, s.last_event_ns, s.total_events, s.segment_count, s.total_bytes,
          s.durationSeconds()};
  FLOX_CAPI_LEAVE;
}

FloxReaderStats flox_data_reader_stats(FloxDataReaderHandle h)
{
  FLOX_CAPI_ENTER(h);
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  auto s = reader->stats();
  return {s.files_read, s.events_read, s.trades_read, s.book_updates_read,
          s.bytes_read, s.crc_errors, s.late_dropped, s.unknown_frames_skipped};
  FLOX_CAPI_LEAVE;
}

uint64_t flox_data_reader_read_trades(FloxDataReaderHandle h, FloxTradeRecord* trades_out,
                                      uint64_t max_trades)
{
  FLOX_CAPI_ENTER(h);
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  uint64_t count = 0;
  reader->forEach(
      [&](const replay::ReplayEvent& ev) -> bool
      {
        if (ev.type == replay::EventType::Trade)
        {
          if (trades_out && count < max_trades)
          {
            trades_out[count] = {ev.trade.exchange_ts_ns, ev.trade.recv_ts_ns, ev.trade.price_raw,
                                 ev.trade.qty_raw, ev.trade.trade_id, ev.trade.symbol_id,
                                 ev.trade.side};
          }
          ++count;
        }
        return !trades_out || count < max_trades;
      });
  return count;
  FLOX_CAPI_LEAVE;
}

// Layout invariants — language bindings (Codon, QuickJS) parse these structs
// from raw byte buffers and depend on exact offsets/sizes.
static_assert(sizeof(FloxBBO) == 64, "FloxBBO must be 64 bytes");
static_assert(sizeof(FloxBookUpdateHeader) == 48, "FloxBookUpdateHeader must be 48 bytes");
static_assert(sizeof(FloxLevel) == 24, "FloxLevel must be 24 bytes");

uint64_t flox_data_reader_read_bbo(FloxDataReaderHandle h, FloxBBO* bbos_out,
                                   uint64_t max_events)
{
  FLOX_CAPI_ENTER(h);
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  uint64_t count = 0;
  reader->forEach(
      [&](const replay::ReplayEvent& ev) -> bool
      {
        if (ev.type != replay::EventType::BookSnapshot &&
            ev.type != replay::EventType::BookDelta)
        {
          return true;
        }

        if (bbos_out && count < max_events)
        {
          FloxBBO b{};
          b.exchange_ts_ns = ev.book_header.exchange_ts_ns;
          b.recv_ts_ns = ev.book_header.recv_ts_ns;
          b.seq = ev.book_header.seq;
          b.symbol_id = ev.book_header.symbol_id;
          b.event_type = ev.book_header.type;
          if (!ev.bids.empty())
          {
            b.bid_price_raw = ev.bids[0].price_raw;
            b.bid_qty_raw = ev.bids[0].qty_raw;
          }
          if (!ev.asks.empty())
          {
            b.ask_price_raw = ev.asks[0].price_raw;
            b.ask_qty_raw = ev.asks[0].qty_raw;
          }
          bbos_out[count] = b;
        }
        ++count;
        return !bbos_out || count < max_events;
      });
  return count;
  FLOX_CAPI_LEAVE;
}

uint64_t flox_data_reader_count_book_updates(FloxDataReaderHandle h, uint64_t* total_levels_out)
{
  FLOX_CAPI_ENTER(h);
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  uint64_t events = 0;
  uint64_t levels = 0;
  reader->forEach(
      [&](const replay::ReplayEvent& ev) -> bool
      {
        if (ev.type == replay::EventType::BookSnapshot ||
            ev.type == replay::EventType::BookDelta)
        {
          ++events;
          levels += ev.bids.size() + ev.asks.size();
        }
        return true;
      });
  if (total_levels_out)
  {
    *total_levels_out = levels;
  }
  return events;
  FLOX_CAPI_LEAVE;
}

uint64_t flox_data_reader_read_book_updates(FloxDataReaderHandle h,
                                            FloxBookUpdateHeader* headers_out,
                                            uint64_t max_events,
                                            FloxLevel* levels_out,
                                            uint64_t max_levels)
{
  FLOX_CAPI_ENTER(h);
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  uint64_t events = 0;
  uint64_t levels_written = 0;
  reader->forEach(
      [&](const replay::ReplayEvent& ev) -> bool
      {
        if (ev.type != replay::EventType::BookSnapshot &&
            ev.type != replay::EventType::BookDelta)
        {
          return true;
        }

        const uint64_t bid_n = ev.bids.size();
        const uint64_t ask_n = ev.asks.size();
        const uint64_t total = bid_n + ask_n;

        if (headers_out && events < max_events && levels_written + total <= max_levels)
        {
          FloxBookUpdateHeader hdr{};
          hdr.exchange_ts_ns = ev.book_header.exchange_ts_ns;
          hdr.recv_ts_ns = ev.book_header.recv_ts_ns;
          hdr.seq = ev.book_header.seq;
          hdr.level_offset = levels_written;
          hdr.symbol_id = ev.book_header.symbol_id;
          hdr.bid_count = static_cast<uint16_t>(bid_n);
          hdr.ask_count = static_cast<uint16_t>(ask_n);
          hdr.event_type = ev.book_header.type;
          headers_out[events] = hdr;

          for (uint64_t i = 0; i < bid_n; ++i)
          {
            levels_out[levels_written + i] = {ev.bids[i].price_raw, ev.bids[i].qty_raw, 0};
          }
          for (uint64_t i = 0; i < ask_n; ++i)
          {
            levels_out[levels_written + bid_n + i] = {ev.asks[i].price_raw, ev.asks[i].qty_raw, 1};
          }

          levels_written += total;
          ++events;
          return true;
        }

        return false;
      });
  return events;
  FLOX_CAPI_LEAVE;
}

// ── _from variants ─────────────────────────────────────────────
// Mid-stream seek: start iterating from start_ts_ns instead of segment
// beginning. Behaviour is otherwise identical to the matching non-_from
// reader.

uint64_t flox_data_reader_read_trades_from(FloxDataReaderHandle h, int64_t start_ts_ns,
                                           FloxTradeRecord* trades_out, uint64_t max_trades)
{
  FLOX_CAPI_ENTER(h);
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  uint64_t count = 0;
  reader->forEachFrom(start_ts_ns,
                      [&](const replay::ReplayEvent& ev) -> bool
                      {
                        if (ev.type == replay::EventType::Trade)
                        {
                          if (trades_out && count < max_trades)
                          {
                            trades_out[count] = {ev.trade.exchange_ts_ns, ev.trade.recv_ts_ns,
                                                 ev.trade.price_raw, ev.trade.qty_raw,
                                                 ev.trade.trade_id, ev.trade.symbol_id,
                                                 ev.trade.side};
                          }
                          ++count;
                        }
                        return !trades_out || count < max_trades;
                      });
  return count;
  FLOX_CAPI_LEAVE;
}

uint64_t flox_data_reader_read_bbo_from(FloxDataReaderHandle h, int64_t start_ts_ns,
                                        FloxBBO* bbos_out, uint64_t max_events)
{
  FLOX_CAPI_ENTER(h);
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  uint64_t count = 0;
  reader->forEachFrom(start_ts_ns,
                      [&](const replay::ReplayEvent& ev) -> bool
                      {
                        if (ev.type != replay::EventType::BookSnapshot &&
                            ev.type != replay::EventType::BookDelta)
                        {
                          return true;
                        }

                        if (bbos_out && count < max_events)
                        {
                          FloxBBO b{};
                          b.exchange_ts_ns = ev.book_header.exchange_ts_ns;
                          b.recv_ts_ns = ev.book_header.recv_ts_ns;
                          b.seq = ev.book_header.seq;
                          b.symbol_id = ev.book_header.symbol_id;
                          b.event_type = ev.book_header.type;
                          if (!ev.bids.empty())
                          {
                            b.bid_price_raw = ev.bids[0].price_raw;
                            b.bid_qty_raw = ev.bids[0].qty_raw;
                          }
                          if (!ev.asks.empty())
                          {
                            b.ask_price_raw = ev.asks[0].price_raw;
                            b.ask_qty_raw = ev.asks[0].qty_raw;
                          }
                          bbos_out[count] = b;
                        }
                        ++count;
                        return !bbos_out || count < max_events;
                      });
  return count;
  FLOX_CAPI_LEAVE;
}

uint64_t flox_data_reader_count_book_updates_from(FloxDataReaderHandle h, int64_t start_ts_ns,
                                                  uint64_t* total_levels_out)
{
  FLOX_CAPI_ENTER(h);
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  uint64_t events = 0;
  uint64_t levels = 0;
  reader->forEachFrom(start_ts_ns,
                      [&](const replay::ReplayEvent& ev) -> bool
                      {
                        if (ev.type == replay::EventType::BookSnapshot ||
                            ev.type == replay::EventType::BookDelta)
                        {
                          ++events;
                          levels += ev.bids.size() + ev.asks.size();
                        }
                        return true;
                      });
  if (total_levels_out)
  {
    *total_levels_out = levels;
  }
  return events;
  FLOX_CAPI_LEAVE;
}

uint64_t flox_data_reader_read_book_updates_from(FloxDataReaderHandle h, int64_t start_ts_ns,
                                                 FloxBookUpdateHeader* headers_out,
                                                 uint64_t max_events, FloxLevel* levels_out,
                                                 uint64_t max_levels)
{
  FLOX_CAPI_ENTER(h);
  auto* reader = static_cast<replay::BinaryLogReader*>(h);
  uint64_t events = 0;
  uint64_t levels_written = 0;
  reader->forEachFrom(start_ts_ns,
                      [&](const replay::ReplayEvent& ev) -> bool
                      {
                        if (ev.type != replay::EventType::BookSnapshot &&
                            ev.type != replay::EventType::BookDelta)
                        {
                          return true;
                        }

                        const uint64_t bid_n = ev.bids.size();
                        const uint64_t ask_n = ev.asks.size();
                        const uint64_t total = bid_n + ask_n;

                        if (headers_out && events < max_events &&
                            levels_written + total <= max_levels)
                        {
                          FloxBookUpdateHeader hdr{};
                          hdr.exchange_ts_ns = ev.book_header.exchange_ts_ns;
                          hdr.recv_ts_ns = ev.book_header.recv_ts_ns;
                          hdr.seq = ev.book_header.seq;
                          hdr.level_offset = levels_written;
                          hdr.symbol_id = ev.book_header.symbol_id;
                          hdr.bid_count = static_cast<uint16_t>(bid_n);
                          hdr.ask_count = static_cast<uint16_t>(ask_n);
                          hdr.event_type = ev.book_header.type;
                          headers_out[events] = hdr;

                          for (uint64_t i = 0; i < bid_n; ++i)
                          {
                            levels_out[levels_written + i] = {ev.bids[i].price_raw,
                                                              ev.bids[i].qty_raw, 0};
                          }
                          for (uint64_t i = 0; i < ask_n; ++i)
                          {
                            levels_out[levels_written + bid_n + i] = {
                                ev.asks[i].price_raw, ev.asks[i].qty_raw, 1};
                          }

                          levels_written += total;
                          ++events;
                          return true;
                        }

                        return false;
                      });
  return events;
  FLOX_CAPI_LEAVE;
}

// ============================================================
// DataWriter (extras)
// ============================================================

FloxWriterStats flox_data_writer_stats(FloxDataWriterHandle h)
{
  FLOX_CAPI_ENTER(h);
  auto* w = static_cast<replay::BinaryLogWriter*>(h);
  auto s = w->stats();
  return {s.bytes_written, s.events_written, s.segments_created, s.trades_written};
  FLOX_CAPI_LEAVE;
}

// ============================================================
// MergedTapeReader — N-tape merged consumption
// ============================================================
namespace capi_impl
{
struct FloxMergedTapeReaderImpl
{
  std::unique_ptr<flox::replay::MergedTapeReader> reader;
  // Cached result of readTrades / readBooks so two-phase count→read
  // doesn't re-merge. Cleared on first read of each kind.
  std::optional<std::vector<flox::replay::MergedTradeRow>> cached_trades;
  std::optional<std::pair<std::vector<flox::replay::MergedBookRow>,
                          std::vector<flox::replay::BookLevel>>>
      cached_books;

  // Owning strings for symbol_table and per-tape paths so the borrowed
  // const char* pointers we hand out stay valid for the reader's lifetime.
  std::vector<std::string> sym_exchanges;
  std::vector<std::string> sym_names;
  std::vector<std::string> tape_paths;
};
}  // namespace capi_impl

FloxMergedTapeReaderHandle
flox_merged_tape_reader_create(const char* const* paths, uint32_t n_paths,
                               int64_t from_ns, int64_t to_ns,
                               const uint32_t* symbol_filter,
                               uint32_t n_filter)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  if (!paths || n_paths == 0)
  {
    return nullptr;
  }
  flox::replay::MergedTapeReaderConfig cfg{};
  cfg.tape_dirs.reserve(n_paths);
  for (uint32_t i = 0; i < n_paths; ++i)
  {
    cfg.tape_dirs.emplace_back(paths[i] ? paths[i] : "");
  }
  if (from_ns >= 0)
  {
    cfg.from_ns = from_ns;
  }
  if (to_ns >= 0)
  {
    cfg.to_ns = to_ns;
  }
  if (symbol_filter && n_filter > 0)
  {
    cfg.symbol_filter.assign(symbol_filter, symbol_filter + n_filter);
  }

  try
  {
    auto impl = std::make_unique<capi_impl::FloxMergedTapeReaderImpl>();
    impl->reader =
        std::make_unique<flox::replay::MergedTapeReader>(std::move(cfg));
    // Cache symbol / path strings now so const char* pointers stay stable.
    for (const auto& s : impl->reader->symbols())
    {
      impl->sym_exchanges.push_back(s.exchange);
      impl->sym_names.push_back(s.name);
    }
    for (const auto& t : impl->reader->perTapeStats())
    {
      impl->tape_paths.push_back(t.path.string());
    }
    return static_cast<FloxMergedTapeReaderHandle>(impl.release());
  }
  catch (...)
  {
    // Construction may throw on bad input or overlapping book streams;
    // surface as a NULL handle, leaving caller to decide.
    return nullptr;
  }
  FLOX_CAPI_LEAVE;
}

void flox_merged_tape_reader_destroy(FloxMergedTapeReaderHandle h)
{
  FLOX_CAPI_ENTER_DESTROY(h);
  delete static_cast<capi_impl::FloxMergedTapeReaderImpl*>(h);
  FLOX_CAPI_LEAVE_VOID;
}

uint32_t flox_merged_tape_reader_symbol_count(FloxMergedTapeReaderHandle h)
{
  FLOX_CAPI_ENTER(h);
  if (!h)
  {
    return 0;
  }
  auto* impl = static_cast<capi_impl::FloxMergedTapeReaderImpl*>(h);
  return static_cast<uint32_t>(impl->reader->symbols().size());
  FLOX_CAPI_LEAVE;
}

uint32_t flox_merged_tape_reader_get_symbols(FloxMergedTapeReaderHandle h,
                                             FloxMergedSymbol* out,
                                             uint32_t max)
{
  FLOX_CAPI_ENTER(h);
  if (!h)
  {
    return 0;
  }
  auto* impl = static_cast<capi_impl::FloxMergedTapeReaderImpl*>(h);
  const auto& syms = impl->reader->symbols();
  uint32_t n = std::min(static_cast<uint32_t>(syms.size()), max);
  if (!out)
  {
    return static_cast<uint32_t>(syms.size());
  }
  for (uint32_t i = 0; i < n; ++i)
  {
    out[i].global_id = syms[i].global_id;
    out[i].price_precision = syms[i].price_precision;
    out[i].qty_precision = syms[i].qty_precision;
    out[i]._pad[0] = out[i]._pad[1] = 0;
    out[i].exchange = impl->sym_exchanges[i].c_str();
    out[i].name = impl->sym_names[i].c_str();
  }
  return n;
  FLOX_CAPI_LEAVE;
}

uint32_t flox_merged_tape_reader_tape_count(FloxMergedTapeReaderHandle h)
{
  FLOX_CAPI_ENTER(h);
  if (!h)
  {
    return 0;
  }
  return static_cast<uint32_t>(
      static_cast<capi_impl::FloxMergedTapeReaderImpl*>(h)
          ->reader->perTapeStats()
          .size());
  FLOX_CAPI_LEAVE;
}

uint32_t flox_merged_tape_reader_get_tape_stats(FloxMergedTapeReaderHandle h,
                                                FloxMergedTapeStats* out,
                                                uint32_t max)
{
  FLOX_CAPI_ENTER(h);
  if (!h)
  {
    return 0;
  }
  auto* impl = static_cast<capi_impl::FloxMergedTapeReaderImpl*>(h);
  const auto& stats = impl->reader->perTapeStats();
  uint32_t n = std::min(static_cast<uint32_t>(stats.size()), max);
  if (!out)
  {
    return static_cast<uint32_t>(stats.size());
  }
  for (uint32_t i = 0; i < n; ++i)
  {
    out[i].first_event_ns = stats[i].first_event_ns;
    out[i].last_event_ns = stats[i].last_event_ns;
    out[i].trades = stats[i].trades;
    out[i].books = stats[i].books;
    out[i].path = impl->tape_paths[i].c_str();
  }
  return n;
  FLOX_CAPI_LEAVE;
}

void flox_merged_tape_reader_time_range(FloxMergedTapeReaderHandle h,
                                        int64_t* min_first_ns_out,
                                        int64_t* max_last_ns_out)
{
  FLOX_CAPI_ENTER_VOID(h);
  if (!h)
  {
    if (min_first_ns_out)
    {
      *min_first_ns_out = 0;
    }
    if (max_last_ns_out)
    {
      *max_last_ns_out = 0;
    }
    return;
  }
  auto* impl = static_cast<capi_impl::FloxMergedTapeReaderImpl*>(h);
  auto [a, b] = impl->reader->timeRange();
  if (min_first_ns_out)
  {
    *min_first_ns_out = a;
  }
  if (max_last_ns_out)
  {
    *max_last_ns_out = b;
  }
  FLOX_CAPI_LEAVE_VOID;
}

uint64_t flox_merged_tape_reader_count_trades(FloxMergedTapeReaderHandle h)
{
  FLOX_CAPI_ENTER(h);
  if (!h)
  {
    return 0;
  }
  auto* impl = static_cast<capi_impl::FloxMergedTapeReaderImpl*>(h);
  if (!impl->cached_trades)
  {
    impl->cached_trades = impl->reader->readTrades();
  }
  return impl->cached_trades->size();
  FLOX_CAPI_LEAVE;
}

uint64_t flox_merged_tape_reader_read_trades(FloxMergedTapeReaderHandle h,
                                             FloxTradeRecord* trades_out,
                                             uint64_t max_trades)
{
  FLOX_CAPI_ENTER(h);
  if (!h)
  {
    return 0;
  }
  auto* impl = static_cast<capi_impl::FloxMergedTapeReaderImpl*>(h);
  if (!impl->cached_trades)
  {
    impl->cached_trades = impl->reader->readTrades();
  }
  const auto& src = *impl->cached_trades;
  uint64_t n = std::min<uint64_t>(src.size(), max_trades);
  if (!trades_out)
  {
    return src.size();
  }
  for (uint64_t i = 0; i < n; ++i)
  {
    trades_out[i].exchange_ts_ns = src[i].exchange_ts_ns;
    trades_out[i].recv_ts_ns = src[i].recv_ts_ns;
    trades_out[i].price_raw = src[i].price_raw;
    trades_out[i].qty_raw = src[i].qty_raw;
    trades_out[i].trade_id = src[i].trade_id;
    trades_out[i].symbol_id = src[i].global_symbol_id;
    trades_out[i].side = src[i].side;
  }
  return n;
  FLOX_CAPI_LEAVE;
}

uint64_t flox_merged_tape_reader_count_books(FloxMergedTapeReaderHandle h,
                                             uint64_t* total_levels_out)
{
  FLOX_CAPI_ENTER(h);
  if (!h)
  {
    if (total_levels_out)
    {
      *total_levels_out = 0;
    }
    return 0;
  }
  auto* impl = static_cast<capi_impl::FloxMergedTapeReaderImpl*>(h);
  if (!impl->cached_books)
  {
    impl->cached_books = impl->reader->readBooks();
  }
  if (total_levels_out)
  {
    *total_levels_out = impl->cached_books->second.size();
  }
  return impl->cached_books->first.size();
  FLOX_CAPI_LEAVE;
}

uint64_t flox_merged_tape_reader_read_books(FloxMergedTapeReaderHandle h,
                                            FloxBookUpdateHeader* headers_out,
                                            uint64_t max_events,
                                            FloxLevel* levels_out,
                                            uint64_t max_levels)
{
  FLOX_CAPI_ENTER(h);
  if (!h)
  {
    return 0;
  }
  auto* impl = static_cast<capi_impl::FloxMergedTapeReaderImpl*>(h);
  if (!impl->cached_books)
  {
    impl->cached_books = impl->reader->readBooks();
  }
  const auto& [rows, levels] = *impl->cached_books;
  uint64_t n_ev = std::min<uint64_t>(rows.size(), max_events);
  uint64_t n_lv = std::min<uint64_t>(levels.size(), max_levels);

  if (headers_out)
  {
    for (uint64_t i = 0; i < n_ev; ++i)
    {
      headers_out[i].exchange_ts_ns = rows[i].exchange_ts_ns;
      headers_out[i].recv_ts_ns = rows[i].recv_ts_ns;
      headers_out[i].seq = rows[i].seq;
      headers_out[i].level_offset = rows[i].level_offset;
      headers_out[i].symbol_id = rows[i].global_symbol_id;
      headers_out[i].bid_count = rows[i].bid_count;
      headers_out[i].ask_count = rows[i].ask_count;
      headers_out[i].event_type = rows[i].event_type;
    }
  }
  if (levels_out)
  {
    // Reader stores levels flat (bids then asks per event). The side
    // byte is reconstructed here using the corresponding header's
    // bid_count split.
    uint64_t k = 0;
    for (uint64_t i = 0; i < rows.size() && k < n_lv; ++i)
    {
      const auto& r = rows[i];
      for (uint16_t b = 0; b < r.bid_count && k < n_lv; ++b, ++k)
      {
        levels_out[k].price_raw = levels[k].price_raw;
        levels_out[k].qty_raw = levels[k].qty_raw;
        levels_out[k].side = 0;
      }
      for (uint16_t a = 0; a < r.ask_count && k < n_lv; ++a, ++k)
      {
        levels_out[k].price_raw = levels[k].price_raw;
        levels_out[k].qty_raw = levels[k].qty_raw;
        levels_out[k].side = 1;
      }
    }
  }
  return n_ev;
  FLOX_CAPI_LEAVE;
}

// ============================================================
// Partitioner
// ============================================================

FloxPartitionerHandle flox_partitioner_create(const char* data_dir)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return new replay::Partitioner(std::filesystem::path(data_dir));
  FLOX_CAPI_LEAVE;
}

void flox_partitioner_destroy(FloxPartitionerHandle h)
{
  FLOX_CAPI_ENTER_DESTROY(h);
  delete static_cast<replay::Partitioner*>(h);
  FLOX_CAPI_LEAVE_VOID;
}

static uint32_t copyPartitions(const std::vector<replay::Partition>& parts,
                               FloxPartition* out, uint32_t max)
{
  uint32_t n = static_cast<uint32_t>(parts.size());
  if (!out)
  {
    return n;
  }
  uint32_t count = std::min(n, max);
  for (uint32_t i = 0; i < count; ++i)
  {
    out[i] = {parts[i].partition_id, parts[i].from_ns, parts[i].to_ns,
              parts[i].warmup_from_ns, parts[i].estimated_events, parts[i].estimated_bytes};
  }
  return count;
}

uint32_t flox_partitioner_by_time(FloxPartitionerHandle h, uint32_t num_partitions,
                                  int64_t warmup_ns, FloxPartition* out, uint32_t max)
{
  FLOX_CAPI_ENTER(h);
  return copyPartitions(
      static_cast<replay::Partitioner*>(h)->partitionByTime(num_partitions, warmup_ns), out, max);
  FLOX_CAPI_LEAVE;
}

uint32_t flox_partitioner_by_duration(FloxPartitionerHandle h, int64_t duration_ns,
                                      int64_t warmup_ns, FloxPartition* out, uint32_t max)
{
  FLOX_CAPI_ENTER(h);
  return copyPartitions(
      static_cast<replay::Partitioner*>(h)->partitionByDuration(duration_ns, warmup_ns), out, max);
  FLOX_CAPI_LEAVE;
}

uint32_t flox_partitioner_by_calendar(FloxPartitionerHandle h, uint8_t unit, int64_t warmup_ns,
                                      FloxPartition* out, uint32_t max)
{
  FLOX_CAPI_ENTER(h);
  return copyPartitions(
      static_cast<replay::Partitioner*>(h)->partitionByCalendar(
          static_cast<replay::Partitioner::CalendarUnit>(unit), warmup_ns),
      out, max);
  FLOX_CAPI_LEAVE;
}

uint32_t flox_partitioner_by_symbol(FloxPartitionerHandle h, uint32_t num_partitions,
                                    FloxPartition* out, uint32_t max)
{
  FLOX_CAPI_ENTER(h);
  return copyPartitions(
      static_cast<replay::Partitioner*>(h)->partitionBySymbol(num_partitions), out, max);
  FLOX_CAPI_LEAVE;
}

uint32_t flox_partitioner_per_symbol(FloxPartitionerHandle h, FloxPartition* out, uint32_t max)
{
  FLOX_CAPI_ENTER(h);
  return copyPartitions(static_cast<replay::Partitioner*>(h)->partitionPerSymbol(), out, max);
  FLOX_CAPI_LEAVE;
}

uint32_t flox_partitioner_by_event_count(FloxPartitionerHandle h, uint32_t num_partitions,
                                         FloxPartition* out, uint32_t max)
{
  FLOX_CAPI_ENTER(h);
  return copyPartitions(
      static_cast<replay::Partitioner*>(h)->partitionByEventCount(num_partitions), out, max);
  FLOX_CAPI_LEAVE;
}

// ============================================================
// Pointer-out wrappers for struct-returning functions.
// ============================================================

void flox_data_reader_summary_p(FloxDataReaderHandle h, void* out)
{
  FLOX_CAPI_ENTER_VOID(h);
  auto s = flox_data_reader_summary(h);
  memcpy(out, &s, sizeof(s));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_data_reader_stats_p(FloxDataReaderHandle h, void* out)
{
  FLOX_CAPI_ENTER_VOID(h);
  auto s = flox_data_reader_stats(h);
  memcpy(out, &s, sizeof(s));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_data_writer_stats_p(FloxDataWriterHandle h, void* out)
{
  FLOX_CAPI_ENTER_VOID(h);
  auto s = flox_data_writer_stats(h);
  memcpy(out, &s, sizeof(s));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_segment_merge_full_p(const char* input_paths, size_t num_paths,
                               const char* output_dir, const char* output_name,
                               uint8_t sort, void* out)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  auto s = flox_segment_merge_full(input_paths, num_paths, output_dir, output_name, sort);
  memcpy(out, &s, sizeof(s));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_segment_merge_dir_p(const char* input_dir, const char* output_dir, void* out)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  auto s = flox_segment_merge_dir(input_dir, output_dir);
  memcpy(out, &s, sizeof(s));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_segment_split_p(const char* input_path, const char* output_dir, uint8_t mode,
                          int64_t time_interval_ns, uint64_t events_per_file, void* out)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  auto s = flox_segment_split(input_path, output_dir, mode, time_interval_ns, events_per_file);
  memcpy(out, &s, sizeof(s));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_segment_export_p(const char* input_path, const char* output_path, uint8_t format,
                           int64_t from_ns, int64_t to_ns,
                           const uint32_t* symbols, uint32_t num_symbols, void* out)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  auto s = flox_segment_export(input_path, output_path, format, from_ns, to_ns,
                               symbols, num_symbols);
  memcpy(out, &s, sizeof(s));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_segment_validate_full_p(const char* path, uint8_t verify_crc,
                                  uint8_t verify_timestamps, void* out)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  auto s = flox_segment_validate_full(path, verify_crc, verify_timestamps);
  memcpy(out, &s, sizeof(s));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_dataset_validate_p(const char* data_dir, void* out)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  auto s = flox_dataset_validate(data_dir);
  memcpy(out, &s, sizeof(s));
  FLOX_CAPI_LEAVE_VOID;
}

// ============================================================
// Shared internals: RunnerSignalHandler, FloxRunnerImpl, FloxLiveEngineImpl
// ============================================================

namespace capi_impl
{

// FloxRiskManagerImpl — non-owning callback bundle. Outlives any runner /
// engine it's attached to (caller manages destruction). Thread-safe to
// invoke from multiple consumer threads concurrently as long as the user-
// supplied callback is itself thread-safe.
struct FloxRiskManagerImpl
{
  FloxRiskManagerCallbacks cb;
};

// Same shape, different intent. See header docs for the evaluation order.
struct FloxKillSwitchImpl
{
  FloxKillSwitchCallbacks cb;
};

struct FloxOrderValidatorImpl
{
  FloxOrderValidatorCallbacks cb;
};

// Post-emission observers. Fire after the user on_signal callback.
// Never block; return type is void.
struct FloxPnLTrackerImpl
{
  FloxPnLTrackerCallbacks cb;
};

struct FloxStorageSinkImpl
{
  FloxStorageSinkCallbacks cb;
};

struct FloxMarketDataRecorderImpl
{
  FloxMarketDataRecorderCallbacks cb;
};

struct FloxReplaySourceImpl
{
  FloxReplaySourceCallbacks cb;
};

// Pack a flox::Order into the ABI-stable FloxOrder for binding callbacks.
inline FloxOrder packOrder(const flox::Order& o) noexcept
{
  FloxOrder fo{};
  fo.id = o.id;
  fo.client_order_id = o.clientOrderId;
  fo.symbol = o.symbol;
  fo.strategy_id = o.strategyId;
  fo.order_tag = o.orderTag;
  fo.side = (o.side == flox::Side::BUY) ? 0u : 1u;
  fo.type = static_cast<uint8_t>(o.type);
  fo.time_in_force = static_cast<uint8_t>(o.timeInForce);
  // Pack ExecutionFlags bit-by-bit for ABI stability.
  uint8_t flags = 0;
  flags |= (o.flags.reduceOnly ? 0x01 : 0);
  flags |= (o.flags.closePosition ? 0x02 : 0);
  flags |= (o.flags.postOnly ? 0x04 : 0);
  flags |= static_cast<uint8_t>((o.flags.holdSide & 0x03) << 3);
  fo.flags = flags;
  fo.price_raw = o.price.raw();
  fo.quantity_raw = o.quantity.raw();
  fo.filled_quantity_raw = o.filledQuantity.raw();
  fo.trigger_price_raw = o.triggerPrice.raw();
  fo.trailing_offset_raw = o.trailingOffset.raw();
  fo.created_at_ns = o.createdAt.time_since_epoch().count();
  fo.exchange_ts_ns = o.exchangeTimestamp.has_value()
                          ? o.exchangeTimestamp->time_since_epoch().count()
                          : 0;
  return fo;
}

struct FloxExecutionListenerImpl
{
  FloxExecutionListenerCallbacks cb;
};

// Adapter that exposes a binding's FloxExecutionListenerCallbacks as a
// flox::IOrderExecutionListener — pluggable into BacktestRunner.
class CapiExecutionListener : public flox::IOrderExecutionListener
{
 public:
  CapiExecutionListener(flox::SubscriberId id, FloxExecutionListenerImpl* impl)
      : flox::IOrderExecutionListener(id), _impl(impl)
  {
  }

  void onOrderSubmitted(const flox::Order& o) override
  {
    if (_impl && _impl->cb.on_submitted)
    {
      auto fo = packOrder(o);
      _impl->cb.on_submitted(_impl->cb.user_data, &fo);
    }
  }
  void onOrderAccepted(const flox::Order& o) override
  {
    if (_impl && _impl->cb.on_accepted)
    {
      auto fo = packOrder(o);
      _impl->cb.on_accepted(_impl->cb.user_data, &fo);
    }
  }
  void onOrderPartiallyFilled(const flox::Order& o, flox::Quantity q) override
  {
    if (_impl && _impl->cb.on_partially_filled)
    {
      auto fo = packOrder(o);
      _impl->cb.on_partially_filled(_impl->cb.user_data, &fo, q.raw());
    }
  }
  void onOrderFilled(const flox::Order& o) override
  {
    if (_impl && _impl->cb.on_filled)
    {
      auto fo = packOrder(o);
      _impl->cb.on_filled(_impl->cb.user_data, &fo);
    }
  }
  void onOrderPendingCancel(const flox::Order& o) override
  {
    if (_impl && _impl->cb.on_pending_cancel)
    {
      auto fo = packOrder(o);
      _impl->cb.on_pending_cancel(_impl->cb.user_data, &fo);
    }
  }
  void onOrderCanceled(const flox::Order& o) override
  {
    if (_impl && _impl->cb.on_canceled)
    {
      auto fo = packOrder(o);
      _impl->cb.on_canceled(_impl->cb.user_data, &fo);
    }
  }
  void onOrderExpired(const flox::Order& o) override
  {
    if (_impl && _impl->cb.on_expired)
    {
      auto fo = packOrder(o);
      _impl->cb.on_expired(_impl->cb.user_data, &fo);
    }
  }
  void onOrderRejected(const flox::Order& o, const std::string& reason) override
  {
    if (_impl && _impl->cb.on_rejected)
    {
      auto fo = packOrder(o);
      _impl->cb.on_rejected(_impl->cb.user_data, &fo, reason.c_str());
    }
  }
  void onOrderReplaced(const flox::Order& oldOrder, const flox::Order& newOrder) override
  {
    if (_impl && _impl->cb.on_replaced)
    {
      auto fo_old = packOrder(oldOrder);
      auto fo_new = packOrder(newOrder);
      _impl->cb.on_replaced(_impl->cb.user_data, &fo_old, &fo_new);
    }
  }
  void onOrderPendingTrigger(const flox::Order& o) override
  {
    if (_impl && _impl->cb.on_pending_trigger)
    {
      auto fo = packOrder(o);
      _impl->cb.on_pending_trigger(_impl->cb.user_data, &fo);
    }
  }
  void onOrderTriggered(const flox::Order& o) override
  {
    if (_impl && _impl->cb.on_triggered)
    {
      auto fo = packOrder(o);
      _impl->cb.on_triggered(_impl->cb.user_data, &fo);
    }
  }
  void onTrailingStopUpdated(const flox::Order& o, flox::Price newTrigger) override
  {
    if (_impl && _impl->cb.on_trailing_stop_updated)
    {
      auto fo = packOrder(o);
      _impl->cb.on_trailing_stop_updated(_impl->cb.user_data, &fo, newTrigger.raw());
    }
  }
  void onOrderQueuePositionChange(const flox::Order& o, flox::Quantity queueAhead,
                                  flox::Quantity queueTotal) override
  {
    if (_impl && _impl->cb.on_queue_position_change)
    {
      auto fo = packOrder(o);
      _impl->cb.on_queue_position_change(_impl->cb.user_data, &fo, queueAhead.raw(),
                                         queueTotal.raw());
    }
  }
  void onOrderMarketPositionChange(const flox::Order& o, uint8_t position,
                                   int32_t distanceToBestTicks) override
  {
    if (_impl && _impl->cb.on_market_position_change)
    {
      auto fo = packOrder(o);
      _impl->cb.on_market_position_change(_impl->cb.user_data, &fo, position,
                                          distanceToBestTicks);
    }
  }
  void onOrderReplaceSubmitted(const flox::Order& oldOrder,
                               const flox::Order& newOrder) override
  {
    if (_impl && _impl->cb.on_replace_submitted)
    {
      auto fo_old = packOrder(oldOrder);
      auto fo_new = packOrder(newOrder);
      _impl->cb.on_replace_submitted(_impl->cb.user_data, &fo_old, &fo_new);
    }
  }
  void onOrderReplaceAccepted(const flox::Order& oldOrder,
                              const flox::Order& newOrder) override
  {
    if (_impl && _impl->cb.on_replace_accepted)
    {
      auto fo_old = packOrder(oldOrder);
      auto fo_new = packOrder(newOrder);
      _impl->cb.on_replace_accepted(_impl->cb.user_data, &fo_old, &fo_new);
    }
  }
  void onOrderReplaceRejected(const flox::Order& oldOrder, const flox::Order& newOrder,
                              const std::string& reason) override
  {
    if (_impl && _impl->cb.on_replace_rejected)
    {
      auto fo_old = packOrder(oldOrder);
      auto fo_new = packOrder(newOrder);
      _impl->cb.on_replace_rejected(_impl->cb.user_data, &fo_old, &fo_new,
                                    reason.c_str());
    }
  }

 private:
  FloxExecutionListenerImpl* _impl;
};

// Unpack an ABI-stable FloxOrder back into a flox::Order. Used by the
// CapiExecutor adapter when reconstructing orders for the binding's
// submit / replace / OCO callbacks (engine-side already constructs an
// Order from a Signal; we round-trip it through FloxOrder for the C ABI).
inline flox::Order unpackOrder(const FloxOrder& fo) noexcept
{
  flox::Order o{};
  o.id = fo.id;
  o.clientOrderId = fo.client_order_id;
  o.symbol = fo.symbol;
  o.strategyId = fo.strategy_id;
  o.orderTag = fo.order_tag;
  o.side = (fo.side == 0) ? flox::Side::BUY : flox::Side::SELL;
  o.type = static_cast<flox::OrderType>(fo.type);
  o.timeInForce = static_cast<flox::TimeInForce>(fo.time_in_force);
  o.flags.reduceOnly = (fo.flags & 0x01) ? 1 : 0;
  o.flags.closePosition = (fo.flags & 0x02) ? 1 : 0;
  o.flags.postOnly = (fo.flags & 0x04) ? 1 : 0;
  o.flags.holdSide = static_cast<uint8_t>((fo.flags >> 3) & 0x03);
  o.price = flox::Price::fromRaw(fo.price_raw);
  o.quantity = flox::Quantity::fromRaw(fo.quantity_raw);
  o.filledQuantity = flox::Quantity::fromRaw(fo.filled_quantity_raw);
  o.triggerPrice = flox::Price::fromRaw(fo.trigger_price_raw);
  o.trailingOffset = flox::Price::fromRaw(fo.trailing_offset_raw);
  o.createdAt = flox::TimePoint{std::chrono::nanoseconds{fo.created_at_ns}};
  if (fo.exchange_ts_ns != 0)
  {
    o.exchangeTimestamp = flox::TimePoint{std::chrono::nanoseconds{fo.exchange_ts_ns}};
  }
  return o;
}

struct FloxExecutorImpl
{
  FloxExecutorCallbacks cb;
};

// Adapter that exposes a binding's FloxExecutorCallbacks as a
// flox::IOrderExecutor — pluggable into BacktestRunner via setExecutor()
// and into FloxLiveEngineImpl's executor slot.
class CapiExecutor : public flox::IOrderExecutor
{
 public:
  explicit CapiExecutor(FloxExecutorImpl* impl) : _impl(impl) {}

  void start() override
  {
    if (_impl && _impl->cb.on_start)
    {
      _impl->cb.on_start(_impl->cb.user_data);
    }
  }
  void stop() override
  {
    if (_impl && _impl->cb.on_stop)
    {
      _impl->cb.on_stop(_impl->cb.user_data);
    }
  }

  void submitOrder(const flox::Order& o) override
  {
    if (_impl && _impl->cb.submit)
    {
      auto fo = packOrder(o);
      _impl->cb.submit(_impl->cb.user_data, &fo);
    }
  }
  void cancelOrder(flox::OrderId orderId) override
  {
    if (_impl && _impl->cb.cancel)
    {
      _impl->cb.cancel(_impl->cb.user_data, static_cast<uint64_t>(orderId));
    }
  }
  void cancelAllOrders(flox::SymbolId symbol) override
  {
    if (_impl && _impl->cb.cancel_all)
    {
      _impl->cb.cancel_all(_impl->cb.user_data, static_cast<uint32_t>(symbol));
    }
  }
  void replaceOrder(flox::OrderId oldId, const flox::Order& newOrder) override
  {
    if (_impl && _impl->cb.replace)
    {
      auto fo = packOrder(newOrder);
      _impl->cb.replace(_impl->cb.user_data, static_cast<uint64_t>(oldId), &fo);
    }
  }
  void submitOCO(const flox::OCOParams& params) override
  {
    if (_impl && _impl->cb.submit_oco)
    {
      auto fo1 = packOrder(params.order1);
      auto fo2 = packOrder(params.order2);
      _impl->cb.submit_oco(_impl->cb.user_data, &fo1, &fo2);
    }
  }
  flox::ExchangeCapabilities capabilities() const override
  {
    if (_impl == nullptr || _impl->cb.capabilities == nullptr)
    {
      return flox::ExchangeCapabilities{};
    }
    FloxExchangeCapabilities cc{};
    _impl->cb.capabilities(_impl->cb.user_data, &cc);
    flox::ExchangeCapabilities out{};
    out.supportsStopMarket = (cc.supports_stop_market != 0);
    out.supportsStopLimit = (cc.supports_stop_limit != 0);
    out.supportsTakeProfitMarket = (cc.supports_take_profit_market != 0);
    out.supportsTakeProfitLimit = (cc.supports_take_profit_limit != 0);
    out.supportsTrailingStop = (cc.supports_trailing_stop != 0);
    out.supportsIceberg = (cc.supports_iceberg != 0);
    out.supportsOCO = (cc.supports_oco != 0);
    out.supportsGTC = (cc.supports_gtc != 0);
    out.supportsIOC = (cc.supports_ioc != 0);
    out.supportsFOK = (cc.supports_fok != 0);
    out.supportsGTD = (cc.supports_gtd != 0);
    out.supportsPostOnly = (cc.supports_post_only != 0);
    out.supportsReduceOnly = (cc.supports_reduce_only != 0);
    out.supportsClosePosition = (cc.supports_close_position != 0);
    return out;
  }

 private:
  FloxExecutorImpl* _impl;
};

// Adapter that exposes a binding's FloxReplaySourceCallbacks as a
// flox::replay::IMultiSegmentReader so BacktestRunner can drive it via
// the existing forEach contract.
class CapiReplaySourceReader : public flox::replay::IMultiSegmentReader
{
 public:
  explicit CapiReplaySourceReader(FloxReplaySourceImpl* source) : _source(source) {}

  uint64_t forEach(EventCallback callback) override
  {
    if (_source == nullptr || _source->cb.next == nullptr)
    {
      return 0;
    }
    uint64_t count = 0;
    FloxReplayEvent ev{};
    while (_source->cb.next(_source->cb.user_data, &ev) != 0)
    {
      flox::replay::ReplayEvent re;
      re.timestamp_ns = ev.timestamp_ns;
      switch (ev.type)
      {
        case 1:  // Trade
        {
          re.type = flox::replay::EventType::Trade;
          re.trade.exchange_ts_ns = ev.timestamp_ns;
          re.trade.recv_ts_ns = ev.timestamp_ns;
          re.trade.price_raw = ev.trade_price_raw;
          re.trade.qty_raw = ev.trade_quantity_raw;
          re.trade.symbol_id = ev.trade_symbol;
          re.trade.side = ev.trade_is_buy ? 0u : 1u;
          break;
        }
        case 2:  // BookSnapshot
        case 3:  // BookDelta
        {
          re.type = (ev.type == 2) ? flox::replay::EventType::BookSnapshot
                                   : flox::replay::EventType::BookDelta;
          re.book_header.exchange_ts_ns = ev.timestamp_ns;
          re.book_header.recv_ts_ns = ev.timestamp_ns;
          re.book_header.symbol_id = ev.book_symbol;
          re.book_header.bid_count = static_cast<uint16_t>(ev.n_bids);
          re.book_header.ask_count = static_cast<uint16_t>(ev.n_asks);
          re.book_header.type = static_cast<uint8_t>(re.type);
          re.bids.clear();
          re.asks.clear();
          re.bids.reserve(ev.n_bids);
          re.asks.reserve(ev.n_asks);
          for (uint32_t i = 0; i < ev.n_bids; ++i)
          {
            flox::replay::BookLevel lvl{};
            lvl.price_raw = ev.bids[i].price_raw;
            lvl.qty_raw = ev.bids[i].quantity_raw;
            re.bids.push_back(lvl);
          }
          for (uint32_t i = 0; i < ev.n_asks; ++i)
          {
            flox::replay::BookLevel lvl{};
            lvl.price_raw = ev.asks[i].price_raw;
            lvl.qty_raw = ev.asks[i].quantity_raw;
            re.asks.push_back(lvl);
          }
          break;
        }
        default:
          // Unknown event type — skip without aborting playback.
          continue;
      }
      ++count;
      if (!callback(re))
      {
        break;
      }
    }
    return count;
  }

  uint64_t forEachFrom(int64_t start_ts_ns, EventCallback callback) override
  {
    if (_source != nullptr && _source->cb.seek_to != nullptr)
    {
      _source->cb.seek_to(_source->cb.user_data, start_ts_ns);
    }
    return forEach(std::move(callback));
  }

  const std::vector<flox::replay::SegmentInfo>& segments() const override
  {
    return _empty_segments;
  }
  uint64_t totalEvents() const override { return 0; }

 private:
  FloxReplaySourceImpl* _source;
  std::vector<flox::replay::SegmentInfo> _empty_segments;
};

// Build a FloxOrder from a Signal for the binding-side executor hook.
// Maps SignalType → OrderType for the order types that turn into orders;
// flow-control signals (Cancel/CancelAll/Modify/OCO) don't go through
// this path because they call cancel/replace/submit_oco on the executor.
inline FloxOrder signalToFloxOrder(const Signal& sig) noexcept
{
  FloxOrder fo{};
  fo.id = sig.orderId;
  fo.symbol = sig.symbol;
  fo.side = (sig.side == Side::BUY) ? 0 : 1;
  fo.time_in_force = static_cast<uint8_t>(sig.timeInForce);
  uint8_t flags = 0;
  flags |= (sig.reduceOnly ? 0x01 : 0);
  flags |= (sig.postOnly ? 0x04 : 0);
  fo.flags = flags;
  fo.price_raw = sig.price.raw();
  fo.quantity_raw = sig.quantity.raw();
  fo.trigger_price_raw = sig.triggerPrice.raw();
  fo.trailing_offset_raw = sig.trailingOffset.raw();

  switch (sig.type)
  {
    case SignalType::Market:
      fo.type = static_cast<uint8_t>(OrderType::MARKET);
      break;
    case SignalType::Limit:
      fo.type = static_cast<uint8_t>(OrderType::LIMIT);
      break;
    case SignalType::StopMarket:
      fo.type = static_cast<uint8_t>(OrderType::STOP_MARKET);
      break;
    case SignalType::StopLimit:
      fo.type = static_cast<uint8_t>(OrderType::STOP_LIMIT);
      break;
    case SignalType::TakeProfitMarket:
      fo.type = static_cast<uint8_t>(OrderType::TAKE_PROFIT_MARKET);
      break;
    case SignalType::TakeProfitLimit:
      fo.type = static_cast<uint8_t>(OrderType::TAKE_PROFIT_LIMIT);
      break;
    case SignalType::TrailingStop:
      fo.type = static_cast<uint8_t>(OrderType::TRAILING_STOP);
      break;
    default:
      fo.type = static_cast<uint8_t>(OrderType::MARKET);
      break;
  }
  return fo;
}

class RunnerSignalHandler : public ISignalHandler
{
 public:
  RunnerSignalHandler(FloxOnSignalCallback cb, void* ud) : _cb(cb), _ud(ud) {}

  // Set or clear the optional pre-trade hooks. Safe to call from any
  // thread while consumer threads are active: the handle is resolved to a
  // strong reference through the registry, and set() swaps that reference
  // into the slot atomically. See tracker_ownership.h for why a bare
  // pointer swap is not enough here -- a dispatch thread may already be
  // inside the previous hook's callback with the old reference in hand.
  void setRiskManager(FloxRiskManagerImpl* rm)
  {
    _risk.set(TrackerRegistry<FloxRiskManagerImpl>::lookup(rm));
  }
  void setKillSwitch(FloxKillSwitchImpl* ks)
  {
    _kill.set(TrackerRegistry<FloxKillSwitchImpl>::lookup(ks));
  }
  void setOrderValidator(FloxOrderValidatorImpl* ov)
  {
    _validator.set(TrackerRegistry<FloxOrderValidatorImpl>::lookup(ov));
  }
  void setPnLTracker(FloxPnLTrackerImpl* p)
  {
    _pnl.set(TrackerRegistry<FloxPnLTrackerImpl>::lookup(p));
  }
  void setStorageSink(FloxStorageSinkImpl* s)
  {
    _sink.set(TrackerRegistry<FloxStorageSinkImpl>::lookup(s));
  }
  void setExecutor(FloxExecutorImpl* e)
  {
    _executor.set(TrackerRegistry<FloxExecutorImpl>::lookup(e));
  }
  void setTraceRecorder(void* rec) noexcept
  {
    _traceRecorder.store(rec, std::memory_order_release);
  }
  void* traceRecorder() const noexcept
  {
    return _traceRecorder.load(std::memory_order_acquire);
  }
  void setTraceFeedTsNs(int64_t ts) noexcept
  {
    _traceFeedTsNs.store(ts, std::memory_order_relaxed);
  }
  int64_t traceFeedTsNs() const noexcept
  {
    return _traceFeedTsNs.load(std::memory_order_relaxed);
  }

  void onSignal(const Signal& sig) override
  {
    // Auto-capture into the attached TraceRecorder, if any. Done before
    // the user callback so the recorded signal is in flight even if the
    // user mutates state in their callback.
    if (auto* rec = _traceRecorder.load(std::memory_order_acquire))
    {
      auto* recorder = static_cast<flox::run::TraceRecorder*>(rec);
      flox::run::SignalView view;
      view.run_ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
      view.feed_ts_ns = _traceFeedTsNs.load(std::memory_order_relaxed);
      view.signal_id = static_cast<uint32_t>(sig.orderId);
      view.flags = 0;
      switch (sig.type)
      {
        case SignalType::Market:
        case SignalType::Limit:
        case SignalType::StopMarket:
        case SignalType::StopLimit:
        case SignalType::TakeProfitMarket:
        case SignalType::TakeProfitLimit:
        case SignalType::TrailingStop:
        case SignalType::OCO:
          view.flags = static_cast<uint32_t>(flox::run::SignalFlags::Enter);
          break;
        case SignalType::Cancel:
        case SignalType::CancelAll:
          view.flags = static_cast<uint32_t>(flox::run::SignalFlags::Exit);
          break;
        case SignalType::Modify:
        case SignalType::ProvideLiquidity:
        case SignalType::WithdrawLiquidity:
          // Neither an entry nor an exit: a modify keeps the position it
          // already has, and the liquidity pair acts on a pool, not a
          // position. Listed rather than left to a default so the next
          // SignalType added does not silently join them.
          break;
      }
      view.strength_raw = 0;
      view.symbol_ids = {static_cast<uint32_t>(sig.symbol)};
      const char* name = "unknown";
      switch (sig.type)
      {
        case SignalType::Market:
          name = "market";
          break;
        case SignalType::Limit:
          name = "limit";
          break;
        case SignalType::Cancel:
          name = "cancel";
          break;
        case SignalType::CancelAll:
          name = "cancel_all";
          break;
        case SignalType::Modify:
          name = "modify";
          break;
        case SignalType::StopMarket:
          name = "stop_market";
          break;
        case SignalType::StopLimit:
          name = "stop_limit";
          break;
        case SignalType::TakeProfitMarket:
          name = "take_profit_market";
          break;
        case SignalType::TakeProfitLimit:
          name = "take_profit_limit";
          break;
        case SignalType::TrailingStop:
          name = "trailing_stop";
          break;
        case SignalType::OCO:
          name = "oco";
          break;
        case SignalType::ProvideLiquidity:
          name = "provide_liquidity";
          break;
        case SignalType::WithdrawLiquidity:
          name = "withdraw_liquidity";
          break;
      }
      view.name = name;
      recorder->writeSignal(view);
    }

    if (!_cb)
    {
      return;
    }
    FloxSignal fs{};
    fs.order_id = sig.orderId;
    fs.symbol = sig.symbol;
    fs.side = (sig.side == Side::BUY) ? 0 : 1;
    fs.price = sig.price.toDouble();
    fs.quantity = sig.quantity.toDouble();
    fs.trigger_price = sig.triggerPrice.toDouble();
    fs.trailing_offset = sig.trailingOffset.toDouble();
    fs.trailing_bps = sig.trailingCallbackRate;
    fs.new_price = sig.newPrice.toDouble();
    fs.new_quantity = sig.newQuantity.toDouble();
    fs.range_lower = sig.priceLower.toDouble();
    fs.range_upper = sig.priceUpper.toDouble();
    fs.liquidity = sig.liquidity.toDouble();

    switch (sig.type)
    {
      case SignalType::Market:
        fs.order_type = FLOX_SIGNAL_TYPE_MARKET;
        break;
      case SignalType::Limit:
        fs.order_type = FLOX_SIGNAL_TYPE_LIMIT;
        break;
      case SignalType::StopMarket:
        fs.order_type = FLOX_SIGNAL_TYPE_STOP_MARKET;
        break;
      case SignalType::StopLimit:
        fs.order_type = FLOX_SIGNAL_TYPE_STOP_LIMIT;
        break;
      case SignalType::TakeProfitMarket:
        fs.order_type = FLOX_SIGNAL_TYPE_TAKE_PROFIT_MARKET;
        break;
      case SignalType::TakeProfitLimit:
        fs.order_type = FLOX_SIGNAL_TYPE_TAKE_PROFIT_LIMIT;
        break;
      case SignalType::TrailingStop:
        fs.order_type = FLOX_SIGNAL_TYPE_TRAILING_STOP;
        break;
      case SignalType::Cancel:
        fs.order_type = FLOX_SIGNAL_TYPE_CANCEL;
        break;
      case SignalType::CancelAll:
        fs.order_type = FLOX_SIGNAL_TYPE_CANCEL_ALL;
        break;
      case SignalType::Modify:
        fs.order_type = FLOX_SIGNAL_TYPE_MODIFY;
        break;
      case SignalType::OCO:
        fs.order_type = FLOX_SIGNAL_TYPE_OCO;
        break;
      case SignalType::ProvideLiquidity:
        fs.order_type = FLOX_SIGNAL_TYPE_PROVIDE_LIQUIDITY;
        break;
      case SignalType::WithdrawLiquidity:
        fs.order_type = FLOX_SIGNAL_TYPE_WITHDRAW_LIQUIDITY;
        break;
    }

    // Pre-trade gates, evaluated in order: KillSwitch → OrderValidator →
    // RiskManager. Each is optional; an unset hook or a NULL fn pointer
    // is a no-op (let the signal through). Returning 0 drops the signal
    // and skips the remaining hooks.
    // Each get() below returns a strong reference that outlives whatever
    // destroy() may do concurrently on another thread for the rest of this
    // scope -- see tracker_ownership.h.
    if (auto ks = _kill.get(); ks && ks->cb.check != nullptr)
    {
      if (ks->cb.check(ks->cb.user_data, &fs) == 0)
      {
        return;
      }
    }
    if (auto ov = _validator.get(); ov && ov->cb.validate != nullptr)
    {
      if (ov->cb.validate(ov->cb.user_data, &fs) == 0)
      {
        return;
      }
    }
    if (auto rm = _risk.get(); rm && rm->cb.allow != nullptr)
    {
      if (rm->cb.allow(rm->cb.user_data, &fs) == 0)
      {
        return;
      }
    }

    _cb(_ud, &fs);

    // Binding-supplied executor — alternative path for order routing.
    // Runs after the user on_signal so existing on_signal-based wiring
    // (where the user submits orders directly) keeps working unchanged.
    // If a user has both an executor and on_signal-based submission, the
    // order will be sent twice — that's the user's responsibility.
    if (auto exec = _executor.get(); exec)
    {
      switch (sig.type)
      {
        case SignalType::Market:
        case SignalType::Limit:
        case SignalType::StopMarket:
        case SignalType::StopLimit:
        case SignalType::TakeProfitMarket:
        case SignalType::TakeProfitLimit:
        case SignalType::TrailingStop:
          if (exec->cb.submit != nullptr)
          {
            FloxOrder fo = signalToFloxOrder(sig);
            exec->cb.submit(exec->cb.user_data, &fo);
          }
          break;
        case SignalType::Cancel:
          if (exec->cb.cancel != nullptr)
          {
            exec->cb.cancel(exec->cb.user_data, sig.orderId);
          }
          break;
        case SignalType::CancelAll:
          if (exec->cb.cancel_all != nullptr)
          {
            exec->cb.cancel_all(exec->cb.user_data, sig.symbol);
          }
          break;
        case SignalType::Modify:
          if (exec->cb.replace != nullptr)
          {
            FloxOrder fo{};
            fo.id = sig.orderId;
            fo.symbol = sig.symbol;
            fo.side = (sig.side == Side::BUY) ? 0 : 1;
            fo.type = static_cast<uint8_t>(OrderType::LIMIT);
            fo.price_raw = sig.newPrice.raw();
            fo.quantity_raw = sig.newQuantity.raw();
            exec->cb.replace(exec->cb.user_data, sig.orderId, &fo);
          }
          break;
        case SignalType::OCO:
          if (exec->cb.submit_oco != nullptr)
          {
            FloxOrder fo1 = signalToFloxOrder(sig);
            fo1.type = static_cast<uint8_t>(OrderType::LIMIT);
            FloxOrder fo2 = fo1;
            fo2.price_raw = sig.triggerPrice.raw();
            exec->cb.submit_oco(exec->cb.user_data, &fo1, &fo2);
          }
          break;
        case SignalType::ProvideLiquidity:
        case SignalType::WithdrawLiquidity:
          // DEX/AMM liquidity ops are handled by the on-chain connector,
          // not the CEX executor callback surface.
          break;
      }
    }

    // Post-emission observers, fired in declared order: PnL → Storage.
    // Return type is void; observers cannot drop the signal (it's already
    // been delivered). The user callback runs first so the binding's
    // hot-path latency isn't affected by observer cost.
    if (auto pnl = _pnl.get(); pnl && pnl->cb.on_signal != nullptr)
    {
      pnl->cb.on_signal(pnl->cb.user_data, &fs);
    }
    if (auto sink = _sink.get(); sink && sink->cb.store != nullptr)
    {
      sink->cb.store(sink->cb.user_data, &fs);
    }
  }

 private:
  FloxOnSignalCallback _cb;
  void* _ud;
  TrackerSlot<FloxRiskManagerImpl> _risk;
  TrackerSlot<FloxKillSwitchImpl> _kill;
  TrackerSlot<FloxOrderValidatorImpl> _validator;
  TrackerSlot<FloxPnLTrackerImpl> _pnl;
  TrackerSlot<FloxStorageSinkImpl> _sink;
  TrackerSlot<FloxExecutorImpl> _executor;
  // Optional trace recorder. Owned by caller; written into on every
  // signal so the run captures without per-strategy instrumentation.
  std::atomic<void*> _traceRecorder{nullptr};
  std::atomic<int64_t> _traceFeedTsNs{0};
};

struct FloxRunnerImpl
{
  SymbolRegistry* registry;
  RunnerSignalHandler handler;
  std::vector<BridgeStrategy*> strategies;
  std::pmr::unsynchronized_pool_resource pool;

  // Optional market data recorder hook. Owned by caller; non-owning ptr.
  std::atomic<FloxMarketDataRecorderImpl*> recorder{nullptr};
  // Tracks whether on_start has fired without a matching on_stop, so that
  // attaching mid-run or detaching emits the right lifecycle callback.
  std::atomic<bool> recorderRunning{false};

  // Optional binding-supplied executor. Same lifecycle pattern as recorder.
  std::atomic<FloxExecutorImpl*> executor{nullptr};
  std::atomic<bool> executorRunning{false};

  FloxRunnerImpl(SymbolRegistry* reg, FloxOnSignalCallback cb, void* ud)
      : registry(reg), handler(cb, ud)
  {
  }

  void addStrategy(BridgeStrategy* s)
  {
    s->setSignalHandler(&handler);
    strategies.push_back(s);
  }

  void setRiskManager(FloxRiskManagerImpl* rm) { handler.setRiskManager(rm); }
  void setKillSwitch(FloxKillSwitchImpl* ks) { handler.setKillSwitch(ks); }
  void setOrderValidator(FloxOrderValidatorImpl* ov)
  {
    handler.setOrderValidator(ov);
  }
  void setPnLTracker(FloxPnLTrackerImpl* p) { handler.setPnLTracker(p); }
  void setStorageSink(FloxStorageSinkImpl* s) { handler.setStorageSink(s); }
  void attachTraceRecorder(void* rec) { handler.setTraceRecorder(rec); }
  void setTraceFeedTsNs(int64_t ts) { handler.setTraceFeedTsNs(ts); }
  flox::run::TraceRecorder* traceRecorder() const noexcept
  {
    return static_cast<flox::run::TraceRecorder*>(handler.traceRecorder());
  }
  int64_t traceFeedTsNs() const noexcept { return handler.traceFeedTsNs(); }

  // Attach / detach a binding-supplied executor. Lifecycle (on_start /
  // on_stop) is balanced against runner start/stop, with hot-swap
  // semantics so attach-while-running and detach fire the lifecycle
  // callbacks correctly.
  void setExecutor(FloxExecutorImpl* e)
  {
    auto* prev = executor.exchange(e, std::memory_order_acq_rel);
    handler.setExecutor(e);
    if (executorRunning.load(std::memory_order_acquire))
    {
      if (prev != nullptr && prev->cb.on_stop != nullptr)
      {
        prev->cb.on_stop(prev->cb.user_data);
      }
      if (e != nullptr && e->cb.on_start != nullptr)
      {
        e->cb.on_start(e->cb.user_data);
      }
    }
  }

  // Attach / detach a market data recorder. If the runner is already started
  // (recorderRunning == true), fire on_stop on the outgoing recorder and
  // on_start on the incoming one so the lifecycle stays balanced.
  void setMarketDataRecorder(FloxMarketDataRecorderImpl* r)
  {
    auto* prev = recorder.exchange(r, std::memory_order_acq_rel);
    if (recorderRunning.load(std::memory_order_acquire))
    {
      if (prev != nullptr && prev->cb.on_stop != nullptr)
      {
        prev->cb.on_stop(prev->cb.user_data);
      }
      if (r != nullptr && r->cb.on_start != nullptr)
      {
        r->cb.on_start(r->cb.user_data);
      }
    }
  }

  void start()
  {
    for (auto* s : strategies)
    {
      s->start();
    }
    recorderRunning.store(true, std::memory_order_release);
    if (auto* r = recorder.load(std::memory_order_acquire);
        r != nullptr && r->cb.on_start != nullptr)
    {
      r->cb.on_start(r->cb.user_data);
    }
    executorRunning.store(true, std::memory_order_release);
    if (auto* e = executor.load(std::memory_order_acquire);
        e != nullptr && e->cb.on_start != nullptr)
    {
      e->cb.on_start(e->cb.user_data);
    }
  }

  void stop()
  {
    if (auto* e = executor.load(std::memory_order_acquire);
        e != nullptr && e->cb.on_stop != nullptr)
    {
      e->cb.on_stop(e->cb.user_data);
    }
    executorRunning.store(false, std::memory_order_release);
    if (auto* r = recorder.load(std::memory_order_acquire);
        r != nullptr && r->cb.on_stop != nullptr)
    {
      r->cb.on_stop(r->cb.user_data);
    }
    recorderRunning.store(false, std::memory_order_release);
    for (auto* s : strategies)
    {
      s->stop();
    }
  }

  void onTrade(uint32_t symbol, double price, double qty, bool is_buy, int64_t ts_ns)
  {
    Trade trade{};
    trade.symbol = symbol;
    trade.price = Price::fromDouble(price);
    trade.quantity = Quantity::fromDouble(qty);
    trade.isBuy = is_buy;
    trade.exchangeTsNs = UnixNanos(ts_ns);

    TradeEvent ev{};
    ev.trade = trade;

    for (auto* s : strategies)
    {
      s->onTrade(ev);
    }

    if (auto* r = recorder.load(std::memory_order_acquire);
        r != nullptr && r->cb.on_trade != nullptr)
    {
      FloxTradeData td{};
      td.symbol = symbol;
      td.price_raw = trade.price.raw();
      td.quantity_raw = trade.quantity.raw();
      td.is_buy = is_buy ? 1 : 0;
      td.exchange_ts_ns = ts_ns;
      r->cb.on_trade(r->cb.user_data, &td);
    }
  }

  void onBookSnapshot(uint32_t symbol,
                      const double* bid_prices, const double* bid_qtys, uint32_t n_bids,
                      const double* ask_prices, const double* ask_qtys, uint32_t n_asks,
                      int64_t ts_ns)
  {
    BookUpdateEvent ev(&pool);
    ev.update.symbol = symbol;
    ev.update.type = BookUpdateType::SNAPSHOT;
    ev.update.exchangeTsNs = UnixNanos(ts_ns);

    for (uint32_t i = 0; i < n_bids; ++i)
    {
      ev.update.bids.push_back({Price::fromDouble(bid_prices[i]),
                                Quantity::fromDouble(bid_qtys[i])});
    }
    for (uint32_t i = 0; i < n_asks; ++i)
    {
      ev.update.asks.push_back({Price::fromDouble(ask_prices[i]),
                                Quantity::fromDouble(ask_qtys[i])});
    }

    for (auto* s : strategies)
    {
      s->onBookUpdate(ev);
    }

    if (auto* r = recorder.load(std::memory_order_acquire);
        r != nullptr && r->cb.on_book_update != nullptr)
    {
      // Re-pack levels into FloxBookLevel for the C ABI. Stack buffers up
      // to a small threshold; heap-fall back for deep books.
      constexpr uint32_t kStackLevels = 64;
      FloxBookLevel stackBids[kStackLevels];
      FloxBookLevel stackAsks[kStackLevels];
      std::vector<FloxBookLevel> heapBids;
      std::vector<FloxBookLevel> heapAsks;
      FloxBookLevel* bidPtr = stackBids;
      FloxBookLevel* askPtr = stackAsks;
      if (n_bids > kStackLevels)
      {
        heapBids.resize(n_bids);
        bidPtr = heapBids.data();
      }
      if (n_asks > kStackLevels)
      {
        heapAsks.resize(n_asks);
        askPtr = heapAsks.data();
      }
      for (uint32_t i = 0; i < n_bids; ++i)
      {
        bidPtr[i].price_raw = ev.update.bids[i].price.raw();
        bidPtr[i].quantity_raw = ev.update.bids[i].quantity.raw();
      }
      for (uint32_t i = 0; i < n_asks; ++i)
      {
        askPtr[i].price_raw = ev.update.asks[i].price.raw();
        askPtr[i].quantity_raw = ev.update.asks[i].quantity.raw();
      }
      r->cb.on_book_update(r->cb.user_data, symbol, /*is_snapshot=*/1u,
                           bidPtr, n_bids, askPtr, n_asks, ts_ns);
    }
  }

  void onBar(uint32_t symbol, uint8_t bar_type, uint64_t bar_type_param,
             double open, double high, double low, double close,
             double volume, double buy_volume,
             int64_t start_time_ns, int64_t end_time_ns,
             uint8_t close_reason)
  {
    BarEvent ev{};
    ev.symbol = symbol;
    ev.barType = static_cast<BarType>(bar_type);
    ev.barTypeParam = bar_type_param;
    ev.bar.open = Price::fromDouble(open);
    ev.bar.high = Price::fromDouble(high);
    ev.bar.low = Price::fromDouble(low);
    ev.bar.close = Price::fromDouble(close);
    ev.bar.volume = Volume::fromDouble(volume);
    ev.bar.buyVolume = Volume::fromDouble(buy_volume);
    ev.bar.startTime = TimePoint{std::chrono::nanoseconds{start_time_ns}};
    ev.bar.endTime = TimePoint{std::chrono::nanoseconds{end_time_ns}};
    ev.bar.reason = static_cast<BarCloseReason>(close_reason);

    for (auto* s : strategies)
    {
      s->onBar(ev);
    }
  }
};

static FloxRunnerImpl* toRunner(FloxRunnerHandle h)
{
  return static_cast<FloxRunnerImpl*>(h);
}

// ============================================================
// FloxLiveEngineImpl — real Disruptor-based live engine
// ============================================================

struct FloxLiveEngineImpl
{
  SymbolRegistry* registry;
  std::unique_ptr<TradeBus> tradeBus;
  std::unique_ptr<BookUpdateBus> bookBus;
  std::unique_ptr<BarBus> barBus;
  pool::Pool<BookUpdateEvent, config::DEFAULT_CONNECTOR_POOL_CAPACITY> bookPool;

  // Per-strategy signal handlers (owned here, outlive strategies)
  std::vector<std::unique_ptr<RunnerSignalHandler>> handlers;
  std::vector<BridgeStrategy*> strategies;

  // Latest pre-trade hooks attached to this engine. Stored here (rather
  // than only on the handlers) so newly-added strategies inherit the
  // existing setting. Caller owns the *Impl objects; we hold non-owning ptrs.
  std::atomic<FloxRiskManagerImpl*> riskManager{nullptr};
  std::atomic<FloxKillSwitchImpl*> killSwitch{nullptr};
  std::atomic<FloxOrderValidatorImpl*> orderValidator{nullptr};
  std::atomic<FloxPnLTrackerImpl*> pnlTracker{nullptr};
  std::atomic<FloxStorageSinkImpl*> storageSink{nullptr};
  std::atomic<FloxMarketDataRecorderImpl*> recorder{nullptr};
  std::atomic<bool> recorderRunning{false};
  std::atomic<FloxExecutorImpl*> executor{nullptr};
  std::atomic<bool> executorRunning{false};

  explicit FloxLiveEngineImpl(SymbolRegistry* reg)
      : registry(reg),
        tradeBus(std::make_unique<TradeBus>()),
        bookBus(std::make_unique<BookUpdateBus>()),
        barBus(std::make_unique<BarBus>())
  {
  }

  void addStrategy(BridgeStrategy* s, FloxOnSignalCallback cb, void* ud)
  {
    auto h = std::make_unique<RunnerSignalHandler>(cb, ud);
    h->setRiskManager(riskManager.load(std::memory_order_acquire));
    h->setKillSwitch(killSwitch.load(std::memory_order_acquire));
    h->setOrderValidator(orderValidator.load(std::memory_order_acquire));
    h->setPnLTracker(pnlTracker.load(std::memory_order_acquire));
    h->setStorageSink(storageSink.load(std::memory_order_acquire));
    h->setExecutor(executor.load(std::memory_order_acquire));
    s->setSignalHandler(h.get());
    handlers.push_back(std::move(h));
    tradeBus->subscribe(s);
    bookBus->subscribe(s);
    barBus->subscribe(s);
    strategies.push_back(s);
  }

  // Update the pre-trade hooks on every existing handler and remember the
  // setting so subsequently-added strategies inherit it.
  void setRiskManager(FloxRiskManagerImpl* rm)
  {
    riskManager.store(rm, std::memory_order_release);
    for (auto& h : handlers)
    {
      h->setRiskManager(rm);
    }
  }
  void setKillSwitch(FloxKillSwitchImpl* ks)
  {
    killSwitch.store(ks, std::memory_order_release);
    for (auto& h : handlers)
    {
      h->setKillSwitch(ks);
    }
  }
  void setOrderValidator(FloxOrderValidatorImpl* ov)
  {
    orderValidator.store(ov, std::memory_order_release);
    for (auto& h : handlers)
    {
      h->setOrderValidator(ov);
    }
  }
  void setPnLTracker(FloxPnLTrackerImpl* p)
  {
    pnlTracker.store(p, std::memory_order_release);
    for (auto& h : handlers)
    {
      h->setPnLTracker(p);
    }
  }
  void setStorageSink(FloxStorageSinkImpl* s)
  {
    storageSink.store(s, std::memory_order_release);
    for (auto& h : handlers)
    {
      h->setStorageSink(s);
    }
  }

  // Attach / detach a market data recorder. Lifecycle (on_start/on_stop)
  // is balanced against engine.start()/stop(), mirroring runner semantics.
  void setMarketDataRecorder(FloxMarketDataRecorderImpl* r)
  {
    auto* prev = recorder.exchange(r, std::memory_order_acq_rel);
    if (recorderRunning.load(std::memory_order_acquire))
    {
      if (prev != nullptr && prev->cb.on_stop != nullptr)
      {
        prev->cb.on_stop(prev->cb.user_data);
      }
      if (r != nullptr && r->cb.on_start != nullptr)
      {
        r->cb.on_start(r->cb.user_data);
      }
    }
  }

  // Attach / detach a binding-supplied executor. Updates every existing
  // signal handler (each strategy has its own) and remembers the setting
  // so subsequently-added strategies inherit it. Lifecycle balanced
  // against engine.start()/stop().
  void setExecutor(FloxExecutorImpl* e)
  {
    auto* prev = executor.exchange(e, std::memory_order_acq_rel);
    for (auto& h : handlers)
    {
      h->setExecutor(e);
    }
    if (executorRunning.load(std::memory_order_acquire))
    {
      if (prev != nullptr && prev->cb.on_stop != nullptr)
      {
        prev->cb.on_stop(prev->cb.user_data);
      }
      if (e != nullptr && e->cb.on_start != nullptr)
      {
        e->cb.on_start(e->cb.user_data);
      }
    }
  }

  void start()
  {
    tradeBus->start();
    bookBus->start();
    barBus->start();
    for (auto* s : strategies)
    {
      s->start();
    }
    recorderRunning.store(true, std::memory_order_release);
    if (auto* r = recorder.load(std::memory_order_acquire);
        r != nullptr && r->cb.on_start != nullptr)
    {
      r->cb.on_start(r->cb.user_data);
    }
    executorRunning.store(true, std::memory_order_release);
    if (auto* e = executor.load(std::memory_order_acquire);
        e != nullptr && e->cb.on_start != nullptr)
    {
      e->cb.on_start(e->cb.user_data);
    }
  }

  void stop()
  {
    if (auto* e = executor.load(std::memory_order_acquire);
        e != nullptr && e->cb.on_stop != nullptr)
    {
      e->cb.on_stop(e->cb.user_data);
    }
    executorRunning.store(false, std::memory_order_release);
    if (auto* r = recorder.load(std::memory_order_acquire);
        r != nullptr && r->cb.on_stop != nullptr)
    {
      r->cb.on_stop(r->cb.user_data);
    }
    recorderRunning.store(false, std::memory_order_release);
    for (auto* s : strategies)
    {
      s->stop();
    }
    tradeBus->stop();
    bookBus->stop();
    barBus->stop();
  }

  void publishTrade(uint32_t symbol, double price, double qty, bool is_buy, int64_t ts_ns)
  {
    TradeEvent ev{};
    ev.trade.symbol = symbol;
    ev.trade.price = Price::fromDouble(price);
    ev.trade.quantity = Quantity::fromDouble(qty);
    ev.trade.isBuy = is_buy;
    ev.trade.exchangeTsNs = UnixNanos(ts_ns);
    tradeBus->publish(ev);

    // Recorder runs on the publisher thread (caller), before publish
    // becomes visible to consumer-side strategies. This mirrors "the
    // engine is being fed this event"; consumer-side timing isn't part
    // of the recorder contract.
    if (auto* r = recorder.load(std::memory_order_acquire);
        r != nullptr && r->cb.on_trade != nullptr)
    {
      FloxTradeData td{};
      td.symbol = symbol;
      td.price_raw = ev.trade.price.raw();
      td.quantity_raw = ev.trade.quantity.raw();
      td.is_buy = is_buy ? 1 : 0;
      td.exchange_ts_ns = ts_ns;
      r->cb.on_trade(r->cb.user_data, &td);
    }
  }

  void publishBookSnapshot(uint32_t symbol,
                           const double* bid_prices, const double* bid_qtys, uint32_t n_bids,
                           const double* ask_prices, const double* ask_qtys, uint32_t n_asks,
                           int64_t ts_ns)
  {
    auto evOpt = bookPool.acquire();
    if (!evOpt)
    {
      return;
    }
    auto& ev = *evOpt;
    ev->update.symbol = symbol;
    ev->update.type = BookUpdateType::SNAPSHOT;
    ev->update.exchangeTsNs = UnixNanos(ts_ns);
    ev->update.bids.clear();
    ev->update.asks.clear();
    for (uint32_t i = 0; i < n_bids; ++i)
    {
      ev->update.bids.push_back({Price::fromDouble(bid_prices[i]),
                                 Quantity::fromDouble(bid_qtys[i])});
    }
    for (uint32_t i = 0; i < n_asks; ++i)
    {
      ev->update.asks.push_back({Price::fromDouble(ask_prices[i]),
                                 Quantity::fromDouble(ask_qtys[i])});
    }

    // Mirror to recorder before publish — see publishTrade comment.
    if (auto* r = recorder.load(std::memory_order_acquire);
        r != nullptr && r->cb.on_book_update != nullptr)
    {
      constexpr uint32_t kStackLevels = 64;
      FloxBookLevel stackBids[kStackLevels];
      FloxBookLevel stackAsks[kStackLevels];
      std::vector<FloxBookLevel> heapBids;
      std::vector<FloxBookLevel> heapAsks;
      FloxBookLevel* bidPtr = stackBids;
      FloxBookLevel* askPtr = stackAsks;
      if (n_bids > kStackLevels)
      {
        heapBids.resize(n_bids);
        bidPtr = heapBids.data();
      }
      if (n_asks > kStackLevels)
      {
        heapAsks.resize(n_asks);
        askPtr = heapAsks.data();
      }
      for (uint32_t i = 0; i < n_bids; ++i)
      {
        bidPtr[i].price_raw = ev->update.bids[i].price.raw();
        bidPtr[i].quantity_raw = ev->update.bids[i].quantity.raw();
      }
      for (uint32_t i = 0; i < n_asks; ++i)
      {
        askPtr[i].price_raw = ev->update.asks[i].price.raw();
        askPtr[i].quantity_raw = ev->update.asks[i].quantity.raw();
      }
      r->cb.on_book_update(r->cb.user_data, symbol, /*is_snapshot=*/1u,
                           bidPtr, n_bids, askPtr, n_asks, ts_ns);
    }

    bookBus->publish(std::move(evOpt.value()));
  }

  void publishBar(uint32_t symbol, uint8_t bar_type, uint64_t bar_type_param,
                  double open, double high, double low, double close,
                  double volume, double buy_volume,
                  int64_t start_time_ns, int64_t end_time_ns,
                  uint8_t close_reason)
  {
    BarEvent ev{};
    ev.symbol = symbol;
    ev.barType = static_cast<BarType>(bar_type);
    ev.barTypeParam = bar_type_param;
    ev.bar.open = Price::fromDouble(open);
    ev.bar.high = Price::fromDouble(high);
    ev.bar.low = Price::fromDouble(low);
    ev.bar.close = Price::fromDouble(close);
    ev.bar.volume = Volume::fromDouble(volume);
    ev.bar.buyVolume = Volume::fromDouble(buy_volume);
    ev.bar.startTime = TimePoint{std::chrono::nanoseconds{start_time_ns}};
    ev.bar.endTime = TimePoint{std::chrono::nanoseconds{end_time_ns}};
    ev.bar.reason = static_cast<BarCloseReason>(close_reason);
    barBus->publish(ev);
  }
};

static FloxLiveEngineImpl* toLiveEngine(FloxLiveEngineHandle h)
{
  return static_cast<FloxLiveEngineImpl*>(h);
}

// ──────────────────────────────────────────────────────────────
// OhlcvBacktestReader — synthesises trade events from OHLCV bars
// ──────────────────────────────────────────────────────────────

class OhlcvBacktestReader : public replay::IMultiSegmentReader
{
 public:
  struct Bar
  {
    int64_t ts_ns;
    int64_t price_raw;
    uint32_t symbol_id;
  };

  explicit OhlcvBacktestReader(std::vector<Bar> bars) : _bars(std::move(bars)) {}

  uint64_t forEach(EventCallback cb) override
  {
    uint64_t n = 0;
    for (const auto& b : _bars)
    {
      if (!cb(make(b)))
      {
        break;
      }
      ++n;
    }
    return n;
  }

  uint64_t forEachFrom(int64_t start_ns, EventCallback cb) override
  {
    uint64_t n = 0;
    for (const auto& b : _bars)
    {
      if (b.ts_ns < start_ns)
      {
        continue;
      }
      if (!cb(make(b)))
      {
        break;
      }
      ++n;
    }
    return n;
  }

  const std::vector<replay::SegmentInfo>& segments() const override { return _segs; }
  uint64_t totalEvents() const override { return _bars.size(); }

 private:
  static replay::ReplayEvent make(const Bar& b)
  {
    replay::ReplayEvent ev{};
    ev.type = replay::EventType::Trade;
    ev.timestamp_ns = b.ts_ns;
    ev.trade.exchange_ts_ns = b.ts_ns;
    ev.trade.price_raw = b.price_raw;
    ev.trade.qty_raw = Quantity::fromDouble(1.0).raw();
    ev.trade.symbol_id = b.symbol_id;
    ev.trade.side = 0;  // buy, in the tape's encoding
    return ev;
  }

  std::vector<Bar> _bars;
  std::vector<replay::SegmentInfo> _segs;
};

// ──────────────────────────────────────────────────────────────
// Adapters: wrap callback-shaped FloxRiskManagerImpl /
// FloxKillSwitchImpl / FloxOrderValidatorImpl / FloxPnLTrackerImpl
// into engine-side I* interfaces so the new BacktestRunner setters
// (which are C++-typed) accept the existing C ABI handles bindings
// already create. The live runner doesn't need this bridge — it
// gates at signal-level inside RunnerSignalHandler — but the
// engine-level BacktestRunner takes Order-shaped callbacks, so we
// build a FloxSignal from the Order and forward.
// ──────────────────────────────────────────────────────────────

// flox::OrderType and the FLOX_SIGNAL_TYPE_* wire codes disagree on 0
// and 1 (OrderType::LIMIT is 0, the wire code for LIMIT is 1), so a raw
// static_cast here made FloxSignal.order_type mean one thing on the
// strategy-signal path and the opposite on the risk-manager /
// kill-switch / order-validator / PnL-tracker callbacks. Map explicitly.
inline uint8_t signalTypeCodeFromOrderType(OrderType type) noexcept
{
  switch (type)
  {
    case OrderType::MARKET:
      return FLOX_SIGNAL_TYPE_MARKET;
    case OrderType::LIMIT:
      return FLOX_SIGNAL_TYPE_LIMIT;
    case OrderType::STOP_MARKET:
      return FLOX_SIGNAL_TYPE_STOP_MARKET;
    case OrderType::STOP_LIMIT:
      return FLOX_SIGNAL_TYPE_STOP_LIMIT;
    case OrderType::TAKE_PROFIT_MARKET:
      return FLOX_SIGNAL_TYPE_TAKE_PROFIT_MARKET;
    case OrderType::TAKE_PROFIT_LIMIT:
      return FLOX_SIGNAL_TYPE_TAKE_PROFIT_LIMIT;
    case OrderType::TRAILING_STOP:
      return FLOX_SIGNAL_TYPE_TRAILING_STOP;
    case OrderType::ICEBERG:
      return FLOX_SIGNAL_TYPE_ICEBERG;
  }
  return FLOX_SIGNAL_TYPE_MARKET;
}

inline FloxSignal orderToFloxSignal(const Order& order) noexcept
{
  FloxSignal fs{};
  fs.order_id = order.id;
  fs.symbol = order.symbol;
  fs.side = (order.side == Side::BUY) ? 0 : 1;
  fs.price = order.price.toDouble();
  fs.quantity = order.quantity.toDouble();
  fs.order_type = signalTypeCodeFromOrderType(order.type);
  return fs;
}

class CapiBacktestRiskManager : public flox::IRiskManager
{
 public:
  explicit CapiBacktestRiskManager(FloxRiskManagerImpl* impl) : _impl(impl) {}

  bool allow(const Order& order) const override
  {
    if (_impl == nullptr || _impl->cb.allow == nullptr)
    {
      return true;
    }
    FloxSignal fs = orderToFloxSignal(order);
    return _impl->cb.allow(_impl->cb.user_data, &fs) != 0;
  }

 private:
  FloxRiskManagerImpl* _impl;
};

class CapiBacktestKillSwitch : public flox::IKillSwitch
{
 public:
  explicit CapiBacktestKillSwitch(FloxKillSwitchImpl* impl) : _impl(impl) {}

  // The C ABI kill switch is a per-signal check. The engine contract
  // is "check(order) may trigger; isTriggered() reports current state".
  //
  // Triggering latches, and only flox_kill_switch_reset clears it. The
  // assignment this used to be let a single "allow" answer un-trigger the
  // switch: after a callback said HALT, the next five orders executed and two
  // round trips completed. The same user code through the Python adapter
  // stayed halted, so one strategy produced two different backtest results
  // depending on which binding ran it.
  void check(const Order& order) override
  {
    if (_impl == nullptr || _impl->cb.check == nullptr)
    {
      return;
    }
    FloxSignal fs = orderToFloxSignal(order);
    // C ABI: 0 → drop / kill-switch active.
    if (_impl->cb.check(_impl->cb.user_data, &fs) == 0)
    {
      _triggered = true;
    }
  }
  void trigger(const std::string& reason) override
  {
    _triggered = true;
    _reason = reason;
  }
  bool isTriggered() const override { return _triggered; }
  std::string reason() const override { return _reason; }

 private:
  FloxKillSwitchImpl* _impl;
  bool _triggered{false};
  std::string _reason;
};

class CapiBacktestOrderValidator : public flox::IOrderValidator
{
 public:
  explicit CapiBacktestOrderValidator(FloxOrderValidatorImpl* impl) : _impl(impl) {}

  // The C ABI validator returns 0/1 without a reason string. Engine
  // contract returns a `reason` parameter — leave it empty when the
  // C ABI rejects so the rejected event still surfaces upward; the
  // user-visible reason can be plumbed through the binding's own
  // PyOrderValidator if richer messaging is needed.
  bool validate(const Order& order, std::string& /*reason*/) const override
  {
    if (_impl == nullptr || _impl->cb.validate == nullptr)
    {
      return true;
    }
    FloxSignal fs = orderToFloxSignal(order);
    return _impl->cb.validate(_impl->cb.user_data, &fs) != 0;
  }

 private:
  FloxOrderValidatorImpl* _impl;
};

class CapiBacktestPnLTracker : public flox::IPnLTracker
{
 public:
  explicit CapiBacktestPnLTracker(FloxPnLTrackerImpl* impl) : _impl(impl) {}

  // Engine fires onOrderFilled per fill; C ABI signature takes a
  // FloxSignal. Convert and forward — bindings that want richer fill
  // detail should attach an ExecutionListener instead.
  void onOrderFilled(const Order& order) override
  {
    if (_impl == nullptr || _impl->cb.on_signal == nullptr)
    {
      return;
    }
    FloxSignal fs = orderToFloxSignal(order);
    _impl->cb.on_signal(_impl->cb.user_data, &fs);
  }

 private:
  FloxPnLTrackerImpl* _impl;
};

// ──────────────────────────────────────────────────────────────
// FloxBacktestRunnerImpl
// ──────────────────────────────────────────────────────────────

struct FloxBacktestRunnerImpl
{
  SymbolRegistry* registry;
  std::unique_ptr<BacktestRunner> runner;
  // Adapters created when bindings register execution listeners. Owned
  // here so they outlive the listener registration on the runner.
  std::vector<std::unique_ptr<CapiExecutionListener>> executionListenerAdapters;
  // Adapter for the binding-supplied executor. Owned so it outlives the
  // runner's non-owning pointer. Replaced on every set call.
  std::unique_ptr<CapiExecutor> executorAdapter;
  // Adapters for the four pre-trade gate hooks. The runner stores
  // raw IRiskManager / IKillSwitch / etc pointers; we own the
  // adapters so they outlive the runner's non-owning references.
  // Replaced on every set call (NULL detaches).
  std::unique_ptr<CapiBacktestRiskManager> riskAdapter;
  std::unique_ptr<CapiBacktestKillSwitch> killAdapter;
  std::unique_ptr<CapiBacktestOrderValidator> validatorAdapter;
  std::unique_ptr<CapiBacktestPnLTracker> pnlAdapter;
  // Most recent BacktestResult, kept after each run_* call so bindings
  // can fetch the equity curve / trades without re-running the
  // backtest. Replaced on every run.
  std::optional<BacktestResult> lastResult;

  explicit FloxBacktestRunnerImpl(SymbolRegistry* reg, double feeRate, double initialCapital)
      : registry(reg)
  {
    BacktestConfig cfg{};
    cfg.feeRate = feeRate;
    cfg.initialCapital = initialCapital;
    cfg.usePercentageFee = true;
    runner = std::make_unique<BacktestRunner>(cfg);
  }

  void addExecutionListener(FloxExecutionListenerImpl* impl)
  {
    if (impl == nullptr)
    {
      return;
    }
    auto id = static_cast<flox::SubscriberId>(executionListenerAdapters.size() + 1);
    auto adapter = std::make_unique<CapiExecutionListener>(id, impl);
    runner->addExecutionListener(adapter.get());
    executionListenerAdapters.push_back(std::move(adapter));
  }

  void setExecutor(FloxExecutorImpl* impl)
  {
    if (impl == nullptr)
    {
      runner->setExecutor(nullptr);
      executorAdapter.reset();
      return;
    }
    executorAdapter = std::make_unique<CapiExecutor>(impl);
    runner->setExecutor(executorAdapter.get());
  }

  void setStrategy(BridgeStrategy* bridge)
  {
    runner->setStrategy(bridge);
  }

  // Pre-trade gate parity with the live runner. NULL detaches.
  // The adapter owns conversion from FloxSignal-shaped callbacks
  // into the engine's Order-shaped I* interfaces.
  void setRiskManager(FloxRiskManagerImpl* rm)
  {
    if (rm == nullptr)
    {
      runner->setRiskManager(nullptr);
      riskAdapter.reset();
      return;
    }
    riskAdapter = std::make_unique<CapiBacktestRiskManager>(rm);
    runner->setRiskManager(riskAdapter.get());
  }
  void setKillSwitch(FloxKillSwitchImpl* ks)
  {
    if (ks == nullptr)
    {
      runner->setKillSwitch(nullptr);
      killAdapter.reset();
      return;
    }
    killAdapter = std::make_unique<CapiBacktestKillSwitch>(ks);
    runner->setKillSwitch(killAdapter.get());
  }
  void setOrderValidator(FloxOrderValidatorImpl* ov)
  {
    if (ov == nullptr)
    {
      runner->setOrderValidator(nullptr);
      validatorAdapter.reset();
      return;
    }
    validatorAdapter = std::make_unique<CapiBacktestOrderValidator>(ov);
    runner->setOrderValidator(validatorAdapter.get());
  }
  void setPnLTracker(FloxPnLTrackerImpl* tracker)
  {
    if (tracker == nullptr)
    {
      runner->setPnLTracker(nullptr);
      pnlAdapter.reset();
      return;
    }
    pnlAdapter = std::make_unique<CapiBacktestPnLTracker>(tracker);
    runner->setPnLTracker(pnlAdapter.get());
  }

  // Guesses a timestamp's unit from its magnitude and scales it to
  // nanoseconds. This is ONLY valid where the unit is genuinely unknown --
  // a CSV column can hold seconds, milliseconds, microseconds, or
  // nanoseconds depending on who wrote the file, and there is no contract
  // to consult. runCsv() is the one caller with that excuse.
  //
  // runOhlcv() and runFullBars() are not: their C ABI parameters are
  // documented and named as nanoseconds already (timestamps_ns,
  // start_time_ns / end_time_ns). Running an already-correct nanosecond
  // value back through this guesser corrupts it instead of leaving it
  // alone -- anything under 1e12 ns (the first eleven and a half days
  // after the epoch, an ordinary timestamp for synthetic/offset test data)
  // reads as "probably seconds" and gets rescaled by another 1e9, which
  // silently wraps on overflow in a release build and aborts under a
  // sanitizer. Contract-known callers pass their values through untouched.
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

  uint32_t resolveSymbol(const char* symbol) const
  {
    if (!symbol || symbol[0] == '\0')
    {
      auto all = registry->getAllSymbols();
      return all.empty() ? 0 : all.front().id;
    }
    auto all = registry->getAllSymbols();
    for (const auto& info : all)
    {
      if (info.symbol == symbol)
      {
        return info.id;
      }
    }
    return 0;
  }

  static void fillStats(const BacktestStats& s, FloxBacktestStats* out)
  {
    out->totalTrades = s.totalTrades;
    out->winningTrades = s.winningTrades;
    out->losingTrades = s.losingTrades;
    out->maxConsecutiveWins = s.maxConsecutiveWins;
    out->maxConsecutiveLosses = s.maxConsecutiveLosses;
    out->initialCapital = s.initialCapital;
    out->finalCapital = s.finalCapital;
    out->totalPnl = s.totalPnl;
    out->totalFees = s.totalFees;
    out->netPnl = s.netPnl;
    out->grossProfit = s.grossProfit;
    out->grossLoss = s.grossLoss;
    out->maxDrawdown = s.maxDrawdown;
    out->maxDrawdownPct = s.maxDrawdownPct;
    out->winRate = s.winRate;
    out->profitFactor = s.profitFactor;
    out->avgWin = s.avgWin;
    out->avgLoss = s.avgLoss;
    out->avgWinLossRatio = s.avgWinLossRatio;
    out->sharpeRatio = s.sharpeRatio;
    out->sortinoRatio = s.sortinoRatio;
    out->calmarRatio = s.calmarRatio;
    out->returnPct = s.returnPct;
  }

  int runBars(std::vector<OhlcvBacktestReader::Bar> bars, FloxBacktestStats* out)
  {
    OhlcvBacktestReader reader(std::move(bars));
    BacktestResult result = runner->run(reader);
    if (out)
    {
      fillStats(result.computeStats(), out);
    }
    lastResult = std::move(result);
    return 1;
  }

  int runTape(const char* tape_dir, FloxBacktestStats* out)
  {
    if (!runner)
    {
      return 0;
    }
    try
    {
      BacktestResult result = runner->runTape(std::filesystem::path(tape_dir));
      if (out)
      {
        fillStats(result.computeStats(), out);
      }
      lastResult = std::move(result);
      return 1;
    }
    catch (const std::exception&)
    {
      return 0;
    }
  }

  int runTapes(const char* const* tape_dirs, uint32_t n_dirs,
               FloxBacktestStats* out)
  {
    if (!runner || !tape_dirs || n_dirs == 0)
    {
      return 0;
    }
    try
    {
      std::vector<std::filesystem::path> paths;
      paths.reserve(n_dirs);
      for (uint32_t i = 0; i < n_dirs; ++i)
      {
        paths.emplace_back(tape_dirs[i] ? tape_dirs[i] : "");
      }
      BacktestResult result = runner->runTapes(paths);
      if (out)
      {
        fillStats(result.computeStats(), out);
      }
      lastResult = std::move(result);
      return 1;
    }
    catch (const std::exception&)
    {
      return 0;
    }
  }

  int runCsv(const char* path, const char* symbol, FloxBacktestStats* out)
  {
    uint32_t id = resolveSymbol(symbol);
    std::ifstream f(path);
    if (!f.is_open())
    {
      return 0;
    }

    std::vector<OhlcvBacktestReader::Bar> bars;
    std::string line;
    std::getline(f, line);  // skip header
    while (std::getline(f, line))
    {
      if (line.empty())
      {
        continue;
      }
      std::istringstream ss(line);
      std::string tok;
      std::getline(ss, tok, ',');
      int64_t ts = normalizeTs(std::stoll(tok));
      std::getline(ss, tok, ',');  // open
      std::getline(ss, tok, ',');  // high
      std::getline(ss, tok, ',');  // low
      std::getline(ss, tok, ',');
      double c = std::stod(tok);
      bars.push_back({ts, Price::fromDouble(c).raw(), id});
    }
    return runBars(std::move(bars), out);
  }

  int runOhlcv(const int64_t* ts, const double* close, uint32_t n,
               const char* symbol, FloxBacktestStats* out)
  {
    uint32_t id = resolveSymbol(symbol);
    std::vector<OhlcvBacktestReader::Bar> bars;
    bars.reserve(n);
    for (uint32_t i = 0; i < n; ++i)
    {
      // ts[i] is flox_backtest_runner_run_ohlcv's documented
      // timestamps_ns -- already nanoseconds by contract, not run through
      // normalizeTs()'s unit guesser (see its comment).
      bars.push_back({ts[i], Price::fromDouble(close[i]).raw(), id});
    }
    return runBars(std::move(bars), out);
  }

  int runFullBars(const int64_t* start_ns, const int64_t* end_ns,
                  const double* open, const double* high, const double* low,
                  const double* close, const double* volume, uint32_t n,
                  const char* symbol, uint8_t bar_type, uint64_t bar_type_param,
                  FloxBacktestStats* out)
  {
    uint32_t id = resolveSymbol(symbol);
    std::vector<BarEvent> events;
    events.reserve(n);
    for (uint32_t i = 0; i < n; ++i)
    {
      BarEvent ev{};
      ev.symbol = id;
      ev.barType = static_cast<BarType>(bar_type);
      ev.barTypeParam = bar_type_param;
      ev.bar.open = Price::fromDouble(open[i]);
      ev.bar.high = Price::fromDouble(high[i]);
      ev.bar.low = Price::fromDouble(low[i]);
      ev.bar.close = Price::fromDouble(close[i]);
      ev.bar.volume = Volume::fromDouble(volume ? volume[i] : 0.0);
      // start_ns[i] / end_ns[i] are flox_backtest_runner_run_bars's
      // documented start_time_ns / end_time_ns -- already nanoseconds by
      // contract, not run through normalizeTs()'s unit guesser (see its
      // comment).
      ev.bar.startTime = TimePoint{std::chrono::nanoseconds{start_ns[i]}};
      ev.bar.endTime = TimePoint{std::chrono::nanoseconds{end_ns[i]}};
      ev.bar.reason = BarCloseReason::Threshold;
      events.push_back(ev);
    }
    BacktestResult result = runner->runBars(events);
    if (out)
    {
      fillStats(result.computeStats(), out);
    }
    lastResult = std::move(result);
    return 1;
  }

  int runReplaySource(FloxReplaySourceImpl* source, FloxBacktestStats* out)
  {
    if (source == nullptr)
    {
      return 0;
    }
    if (source->cb.on_start != nullptr)
    {
      source->cb.on_start(source->cb.user_data);
    }
    CapiReplaySourceReader reader(source);
    BacktestResult result = runner->run(reader);
    if (source->cb.on_stop != nullptr)
    {
      source->cb.on_stop(source->cb.user_data);
    }
    if (out)
    {
      fillStats(result.computeStats(), out);
    }
    lastResult = std::move(result);
    return 1;
  }
};

static FloxBacktestRunnerImpl* toBacktestRunner(FloxBacktestRunnerHandle h)
{
  return static_cast<FloxBacktestRunnerImpl*>(h);
}

}  // namespace capi_impl

using namespace capi_impl;

// ============================================================
// FloxRiskManager C API
// ============================================================

FloxRiskManagerHandle flox_risk_manager_create(FloxRiskManagerCallbacks callbacks)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return static_cast<FloxRiskManagerHandle>(
      TrackerRegistry<FloxRiskManagerImpl>::create(FloxRiskManagerImpl{callbacks}));
  FLOX_CAPI_LEAVE;
}

FloxRiskManagerHandle flox_risk_manager_create_p(const FloxRiskManagerCallbacks* callbacks)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  FloxRiskManagerCallbacks cbs = callbacks ? *callbacks : FloxRiskManagerCallbacks{};
  return flox_risk_manager_create(cbs);
  FLOX_CAPI_LEAVE;
}

void flox_risk_manager_destroy(FloxRiskManagerHandle rm)
{
  FLOX_CAPI_ENTER_DESTROY(rm);
  TrackerRegistry<FloxRiskManagerImpl>::destroy(static_cast<FloxRiskManagerImpl*>(rm));
  FLOX_CAPI_LEAVE_VOID;
}

FloxKillSwitchHandle flox_kill_switch_create(FloxKillSwitchCallbacks callbacks)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return static_cast<FloxKillSwitchHandle>(
      TrackerRegistry<FloxKillSwitchImpl>::create(FloxKillSwitchImpl{callbacks}));
  FLOX_CAPI_LEAVE;
}

FloxKillSwitchHandle flox_kill_switch_create_p(const FloxKillSwitchCallbacks* callbacks)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  FloxKillSwitchCallbacks cbs = callbacks ? *callbacks : FloxKillSwitchCallbacks{};
  return flox_kill_switch_create(cbs);
  FLOX_CAPI_LEAVE;
}

void flox_kill_switch_destroy(FloxKillSwitchHandle ks)
{
  FLOX_CAPI_ENTER_DESTROY(ks);
  TrackerRegistry<FloxKillSwitchImpl>::destroy(static_cast<FloxKillSwitchImpl*>(ks));
  FLOX_CAPI_LEAVE_VOID;
}

FloxOrderValidatorHandle flox_order_validator_create(FloxOrderValidatorCallbacks callbacks)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return static_cast<FloxOrderValidatorHandle>(
      TrackerRegistry<FloxOrderValidatorImpl>::create(FloxOrderValidatorImpl{callbacks}));
  FLOX_CAPI_LEAVE;
}

FloxOrderValidatorHandle flox_order_validator_create_p(const FloxOrderValidatorCallbacks* callbacks)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  FloxOrderValidatorCallbacks cbs = callbacks ? *callbacks : FloxOrderValidatorCallbacks{};
  return flox_order_validator_create(cbs);
  FLOX_CAPI_LEAVE;
}

void flox_order_validator_destroy(FloxOrderValidatorHandle ov)
{
  FLOX_CAPI_ENTER_DESTROY(ov);
  TrackerRegistry<FloxOrderValidatorImpl>::destroy(static_cast<FloxOrderValidatorImpl*>(ov));
  FLOX_CAPI_LEAVE_VOID;
}

FloxPnLTrackerHandle flox_pnl_tracker_create(FloxPnLTrackerCallbacks callbacks)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return static_cast<FloxPnLTrackerHandle>(
      TrackerRegistry<FloxPnLTrackerImpl>::create(FloxPnLTrackerImpl{callbacks}));
  FLOX_CAPI_LEAVE;
}

FloxPnLTrackerHandle flox_pnl_tracker_create_p(const FloxPnLTrackerCallbacks* callbacks)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  FloxPnLTrackerCallbacks cbs = callbacks ? *callbacks : FloxPnLTrackerCallbacks{};
  return flox_pnl_tracker_create(cbs);
  FLOX_CAPI_LEAVE;
}

void flox_pnl_tracker_destroy(FloxPnLTrackerHandle tracker)
{
  FLOX_CAPI_ENTER_DESTROY(tracker);
  TrackerRegistry<FloxPnLTrackerImpl>::destroy(static_cast<FloxPnLTrackerImpl*>(tracker));
  FLOX_CAPI_LEAVE_VOID;
}

FloxStorageSinkHandle flox_storage_sink_create(FloxStorageSinkCallbacks callbacks)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return static_cast<FloxStorageSinkHandle>(
      TrackerRegistry<FloxStorageSinkImpl>::create(FloxStorageSinkImpl{callbacks}));
  FLOX_CAPI_LEAVE;
}

FloxStorageSinkHandle flox_storage_sink_create_p(const FloxStorageSinkCallbacks* callbacks)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  FloxStorageSinkCallbacks cbs = callbacks ? *callbacks : FloxStorageSinkCallbacks{};
  return flox_storage_sink_create(cbs);
  FLOX_CAPI_LEAVE;
}

void flox_storage_sink_destroy(FloxStorageSinkHandle sink)
{
  FLOX_CAPI_ENTER_DESTROY(sink);
  TrackerRegistry<FloxStorageSinkImpl>::destroy(static_cast<FloxStorageSinkImpl*>(sink));
  FLOX_CAPI_LEAVE_VOID;
}

FloxMarketDataRecorderHandle flox_market_data_recorder_create(
    FloxMarketDataRecorderCallbacks callbacks)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return static_cast<FloxMarketDataRecorderHandle>(
      new FloxMarketDataRecorderImpl{callbacks});
  FLOX_CAPI_LEAVE;
}

FloxMarketDataRecorderHandle flox_market_data_recorder_create_p(
    const FloxMarketDataRecorderCallbacks* callbacks)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  FloxMarketDataRecorderCallbacks cbs = callbacks ? *callbacks : FloxMarketDataRecorderCallbacks{};
  return flox_market_data_recorder_create(cbs);
  FLOX_CAPI_LEAVE;
}

void flox_market_data_recorder_destroy(FloxMarketDataRecorderHandle recorder)
{
  FLOX_CAPI_ENTER_DESTROY(recorder);
  delete static_cast<FloxMarketDataRecorderImpl*>(recorder);
  FLOX_CAPI_LEAVE_VOID;
}

// ── Binary-log recorder hook ──────────────────────────────────────────
namespace capi_impl
{
struct FloxBinaryLogRecorderHookImpl
{
  flox::replay::BinaryLogRecorderHook hook;
  // FloxMarketDataRecorderImpl whose callbacks bridge engine events
  // straight into hook.onTrade / onBookUpdate. user_data points at
  // `this`. Lifetime is tied to the owning impl — DO NOT separately
  // free via flox_market_data_recorder_destroy.
  FloxMarketDataRecorderImpl recorder_view{};

  explicit FloxBinaryLogRecorderHookImpl(flox::replay::BinaryLogRecorderHookConfig cfg)
      : hook(std::move(cfg))
  {
  }
};

static void blrhOnTrade(void* ud, const FloxTradeData* t)
{
  if (!t)
  {
    return;
  }
  auto* self = static_cast<FloxBinaryLogRecorderHookImpl*>(ud);
  self->hook.onTrade(t->symbol, t->price_raw, t->quantity_raw,
                     t->is_buy != 0, t->exchange_ts_ns, /*recv_ts_ns=*/0);
}

static void blrhOnBookUpdate(void* ud, uint32_t symbol, uint8_t is_snapshot,
                             const FloxBookLevel* bids, uint32_t n_bids,
                             const FloxBookLevel* asks, uint32_t n_asks,
                             int64_t exchange_ts_ns)
{
  auto* self = static_cast<FloxBinaryLogRecorderHookImpl*>(ud);
  auto* bid_levels = reinterpret_cast<const flox::replay::BookLevel*>(bids);
  auto* ask_levels = reinterpret_cast<const flox::replay::BookLevel*>(asks);
  self->hook.onBookUpdate(symbol, is_snapshot != 0, bid_levels, n_bids,
                          ask_levels, n_asks, exchange_ts_ns, /*recv_ts_ns=*/0);
}

static void blrhOnStart(void* ud)
{
  static_cast<FloxBinaryLogRecorderHookImpl*>(ud)->hook.start();
}

static void blrhOnStop(void* ud)
{
  static_cast<FloxBinaryLogRecorderHookImpl*>(ud)->hook.stop();
}
}  // namespace capi_impl

FloxBinaryLogRecorderHookHandle
flox_binary_log_recorder_hook_create(const char* output_dir,
                                     uint64_t max_segment_mb,
                                     uint8_t exchange_id,
                                     uint8_t compression)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return flox_binary_log_recorder_hook_create_ex(output_dir, max_segment_mb,
                                                 exchange_id, compression,
                                                 nullptr, nullptr);
  FLOX_CAPI_LEAVE;
}

FloxBinaryLogRecorderHookHandle
flox_binary_log_recorder_hook_create_ex(const char* output_dir,
                                        uint64_t max_segment_mb,
                                        uint8_t exchange_id,
                                        uint8_t compression,
                                        const char* exchange_name,
                                        const char* instrument_type)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  flox::replay::BinaryLogRecorderHookConfig cfg{};
  cfg.output_dir = output_dir ? output_dir : "";
  cfg.max_segment_bytes = max_segment_mb * 1024ull * 1024ull;
  cfg.exchange_id = exchange_id;
  cfg.compression = static_cast<flox::replay::CompressionType>(compression);

  const bool has_exchange = exchange_name && *exchange_name;
  const bool has_instrument = instrument_type && *instrument_type;
  if (has_exchange || has_instrument)
  {
    flox::replay::RecordingMetadata meta{};
    if (has_exchange)
    {
      meta.exchange = exchange_name;
    }
    if (has_instrument)
    {
      meta.instrument_type = instrument_type;
    }
    cfg.metadata = std::move(meta);
  }

  auto* impl = new capi_impl::FloxBinaryLogRecorderHookImpl(std::move(cfg));
  impl->recorder_view.cb.on_trade = &capi_impl::blrhOnTrade;
  impl->recorder_view.cb.on_book_update = &capi_impl::blrhOnBookUpdate;
  impl->recorder_view.cb.on_start = &capi_impl::blrhOnStart;
  impl->recorder_view.cb.on_stop = &capi_impl::blrhOnStop;
  impl->recorder_view.cb.user_data = impl;
  // Borrowed: the recorder handle callers get from
  // flox_binary_log_recorder_hook_as_recorder points at this member, so
  // flox_market_data_recorder_destroy on it would free part of the hook.
  // The header has always said not to; registering it means a caller that
  // does anyway gets a no-op instead of a corrupted heap.
  FloxBorrowedHandles::add(&impl->recorder_view);
  return static_cast<FloxBinaryLogRecorderHookHandle>(impl);
  FLOX_CAPI_LEAVE;
}

void flox_binary_log_recorder_hook_destroy(FloxBinaryLogRecorderHookHandle h)
{
  FLOX_CAPI_ENTER_DESTROY(h);
  auto* impl = static_cast<capi_impl::FloxBinaryLogRecorderHookImpl*>(h);
  FloxBorrowedHandles::remove(&impl->recorder_view);
  delete impl;
  FLOX_CAPI_LEAVE_VOID;
}

FloxMarketDataRecorderHandle
flox_binary_log_recorder_hook_as_recorder(FloxBinaryLogRecorderHookHandle h)
{
  FLOX_CAPI_ENTER(h);
  if (!h)
  {
    return nullptr;
  }
  return static_cast<FloxMarketDataRecorderHandle>(
      &static_cast<capi_impl::FloxBinaryLogRecorderHookImpl*>(h)->recorder_view);
  FLOX_CAPI_LEAVE;
}

void flox_binary_log_recorder_hook_add_symbol(FloxBinaryLogRecorderHookHandle h,
                                              uint32_t symbol_id,
                                              const char* name,
                                              const char* base,
                                              const char* quote,
                                              int8_t price_precision,
                                              int8_t qty_precision)
{
  FLOX_CAPI_ENTER_VOID(h);
  if (!h)
  {
    return;
  }
  flox::replay::SymbolInfo info;
  info.symbol_id = symbol_id;
  info.name = name ? name : "";
  info.base_asset = base ? base : "";
  info.quote_asset = quote ? quote : "";
  info.price_precision = price_precision;
  info.qty_precision = qty_precision;
  static_cast<capi_impl::FloxBinaryLogRecorderHookImpl*>(h)->hook.addSymbol(info);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_binary_log_recorder_hook_flush(FloxBinaryLogRecorderHookHandle h)
{
  FLOX_CAPI_ENTER_VOID(h);
  if (h)
  {
    static_cast<capi_impl::FloxBinaryLogRecorderHookImpl*>(h)->hook.flush();
  }
  FLOX_CAPI_LEAVE_VOID;
}

FloxWriterStats flox_binary_log_recorder_hook_stats(FloxBinaryLogRecorderHookHandle h)
{
  FLOX_CAPI_ENTER(h);
  if (!h)
  {
    return {};
  }
  auto s = static_cast<capi_impl::FloxBinaryLogRecorderHookImpl*>(h)->hook.stats();
  return {s.bytes_written, s.trades_written + s.book_updates_written,
          s.segments_created, s.trades_written};
  FLOX_CAPI_LEAVE;
}

void flox_binary_log_recorder_hook_stats_p(void* h, void* out)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  auto s = flox_binary_log_recorder_hook_stats(static_cast<FloxBinaryLogRecorderHookHandle>(h));
  memcpy(out, &s, sizeof(s));
  FLOX_CAPI_LEAVE_VOID;
}

FloxReplaySourceHandle flox_replay_source_create(FloxReplaySourceCallbacks callbacks)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return static_cast<FloxReplaySourceHandle>(
      new FloxReplaySourceImpl{callbacks});
  FLOX_CAPI_LEAVE;
}

FloxReplaySourceHandle flox_replay_source_create_p(const FloxReplaySourceCallbacks* callbacks)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  FloxReplaySourceCallbacks cbs = callbacks ? *callbacks : FloxReplaySourceCallbacks{};
  return flox_replay_source_create(cbs);
  FLOX_CAPI_LEAVE;
}

void flox_replay_source_destroy(FloxReplaySourceHandle source)
{
  FLOX_CAPI_ENTER_DESTROY(source);
  delete static_cast<FloxReplaySourceImpl*>(source);
  FLOX_CAPI_LEAVE_VOID;
}

uint8_t flox_replay_source_seek_to(FloxReplaySourceHandle source, int64_t timestamp_ns)
{
  FLOX_CAPI_ENTER(source);
  auto* s = static_cast<FloxReplaySourceImpl*>(source);
  if (s == nullptr || s->cb.seek_to == nullptr)
  {
    return 0;
  }
  return s->cb.seek_to(s->cb.user_data, timestamp_ns);
  FLOX_CAPI_LEAVE;
}

FloxExecutionListenerHandle
flox_execution_listener_create(FloxExecutionListenerCallbacks callbacks)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return static_cast<FloxExecutionListenerHandle>(
      new FloxExecutionListenerImpl{callbacks});
  FLOX_CAPI_LEAVE;
}

FloxExecutionListenerHandle
flox_execution_listener_create_p(const FloxExecutionListenerCallbacks* callbacks)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  FloxExecutionListenerCallbacks cbs = callbacks ? *callbacks : FloxExecutionListenerCallbacks{};
  return flox_execution_listener_create(cbs);
  FLOX_CAPI_LEAVE;
}

void flox_execution_listener_destroy(FloxExecutionListenerHandle listener)
{
  FLOX_CAPI_ENTER_DESTROY(listener);
  delete static_cast<FloxExecutionListenerImpl*>(listener);
  FLOX_CAPI_LEAVE_VOID;
}

FloxExecutorHandle flox_executor_create(FloxExecutorCallbacks callbacks)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return static_cast<FloxExecutorHandle>(
      TrackerRegistry<FloxExecutorImpl>::create(FloxExecutorImpl{callbacks}));
  FLOX_CAPI_LEAVE;
}

FloxExecutorHandle flox_executor_create_p(const FloxExecutorCallbacks* callbacks)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  FloxExecutorCallbacks cbs = callbacks ? *callbacks : FloxExecutorCallbacks{};
  return flox_executor_create(cbs);
  FLOX_CAPI_LEAVE;
}

void flox_executor_destroy(FloxExecutorHandle executor)
{
  FLOX_CAPI_ENTER_DESTROY(executor);
  TrackerRegistry<FloxExecutorImpl>::destroy(static_cast<FloxExecutorImpl*>(executor));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_executor_get_capabilities(FloxExecutorHandle executor,
                                    FloxExchangeCapabilities* caps_out)
{
  FLOX_CAPI_ENTER_VOID(executor);
  if (caps_out == nullptr)
  {
    return;
  }
  *caps_out = FloxExchangeCapabilities{};
  auto* impl = static_cast<FloxExecutorImpl*>(executor);
  if (impl == nullptr || impl->cb.capabilities == nullptr)
  {
    return;
  }
  impl->cb.capabilities(impl->cb.user_data, caps_out);
  FLOX_CAPI_LEAVE_VOID;
}

// ============================================================
// Logger callback adapter
// ============================================================

namespace
{

class CapiCallbackLogger : public flox::ILogger
{
 public:
  CapiCallbackLogger(FloxLogCallback cb, void* ud) : _cb(cb), _ud(ud) {}

  void info(std::string_view msg) override { dispatch(0, msg); }
  void warn(std::string_view msg) override { dispatch(1, msg); }
  void error(std::string_view msg) override { dispatch(2, msg); }

 private:
  void dispatch(int32_t level, std::string_view msg)
  {
    // string_view is not guaranteed null-terminated; the C callback expects
    // const char*. Construct a std::string for the call duration.
    std::string s(msg);
    _cb(_ud, level, s.c_str());
  }

  FloxLogCallback _cb;
  void* _ud;
};

// Owned by the C API; outlives the registered call. Reset via
// flox_set_log_callback(NULL, ...).
std::unique_ptr<CapiCallbackLogger> g_capiLogger;

}  // namespace

void flox_set_log_callback(FloxLogCallback callback, void* user_data)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  if (callback == nullptr)
  {
    flox::setGlobalLogger(nullptr);
    g_capiLogger.reset();
    return;
  }
  auto next = std::make_unique<CapiCallbackLogger>(callback, user_data);
  flox::setGlobalLogger(next.get());
  // Replace AFTER swapping the pointer so concurrent log calls never see
  // a dangling adapter. If two flox_set_log_callback calls race, the loser
  // simply destroys its adapter without ever being installed.
  g_capiLogger = std::move(next);
  FLOX_CAPI_LEAVE_VOID;
}

FloxRunnerHandle flox_runner_create(FloxRegistryHandle registry,
                                    FloxOnSignalCallback on_signal,
                                    void* user_data)
{
  FLOX_CAPI_ENTER(registry);
  return static_cast<FloxRunnerHandle>(
      new FloxRunnerImpl(toRegistry(registry), on_signal, user_data));
  FLOX_CAPI_LEAVE;
}

void flox_runner_destroy(FloxRunnerHandle runner)
{
  FLOX_CAPI_ENTER_DESTROY(runner);
  delete toRunner(runner);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_runner_add_strategy(FloxRunnerHandle runner, FloxStrategyHandle strategy)
{
  FLOX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->addStrategy(toStrategy(strategy));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_runner_set_risk_manager(FloxRunnerHandle runner, FloxRiskManagerHandle rm)
{
  FLOX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->setRiskManager(static_cast<capi_impl::FloxRiskManagerImpl*>(rm));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_runner_set_kill_switch(FloxRunnerHandle runner, FloxKillSwitchHandle ks)
{
  FLOX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->setKillSwitch(static_cast<capi_impl::FloxKillSwitchImpl*>(ks));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_runner_set_order_validator(FloxRunnerHandle runner, FloxOrderValidatorHandle ov)
{
  FLOX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->setOrderValidator(
      static_cast<capi_impl::FloxOrderValidatorImpl*>(ov));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_runner_set_pnl_tracker(FloxRunnerHandle runner, FloxPnLTrackerHandle tracker)
{
  FLOX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->setPnLTracker(
      static_cast<capi_impl::FloxPnLTrackerImpl*>(tracker));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_runner_set_storage_sink(FloxRunnerHandle runner, FloxStorageSinkHandle sink)
{
  FLOX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->setStorageSink(
      static_cast<capi_impl::FloxStorageSinkImpl*>(sink));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_runner_set_executor(FloxRunnerHandle runner, FloxExecutorHandle executor)
{
  FLOX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->setExecutor(
      static_cast<capi_impl::FloxExecutorImpl*>(executor));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_runner_set_market_data_recorder(FloxRunnerHandle runner,
                                          FloxMarketDataRecorderHandle recorder)
{
  FLOX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->setMarketDataRecorder(
      static_cast<capi_impl::FloxMarketDataRecorderImpl*>(recorder));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_runner_attach_trace_recorder(FloxRunnerHandle runner, FloxRunRecorderHandle recorder)
{
  FLOX_CAPI_ENTER_VOID(runner);
  // recorder is a `flox::run::TraceRecorder*` (from
  // `flox_run_recorder_create`) or NULL to detach.
  toRunner(runner)->attachTraceRecorder(static_cast<void*>(recorder));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_runner_set_trace_feed_ts_ns(FloxRunnerHandle runner, int64_t feed_ts_ns)
{
  FLOX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->setTraceFeedTsNs(feed_ts_ns);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_runner_trace_order_event(FloxRunnerHandle runner, uint64_t order_id,
                                   uint64_t parent_signal_id, uint32_t symbol_id,
                                   uint8_t event_kind, uint8_t side, uint8_t order_type,
                                   int64_t price_raw, int64_t qty_raw, uint32_t flags)
{
  FLOX_CAPI_ENTER_VOID(runner);
  auto* rec = toRunner(runner)->traceRecorder();
  if (!rec)
  {
    return;
  }
  flox::run::OrderEventView e;
  e.run_ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
  e.feed_ts_ns = toRunner(runner)->traceFeedTsNs();
  e.order_id = order_id;
  e.parent_signal_id = parent_signal_id;
  e.symbol_id = symbol_id;
  e.event_kind = static_cast<flox::run::OrderEventKind>(event_kind);
  e.side = side;
  e.order_type = order_type;
  e.price_raw = price_raw;
  e.qty_raw = qty_raw;
  e.flags = flags;
  rec->writeOrderEvent(e);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_runner_trace_fill(FloxRunnerHandle runner, uint64_t order_id, uint64_t fill_id,
                            int64_t price_raw, int64_t qty_raw, int64_t fee_raw,
                            uint32_t symbol_id, uint8_t side, uint8_t liquidity)
{
  FLOX_CAPI_ENTER_VOID(runner);
  auto* rec = toRunner(runner)->traceRecorder();
  if (!rec)
  {
    return;
  }
  flox::run::FillView f;
  f.run_ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
  f.feed_ts_ns = toRunner(runner)->traceFeedTsNs();
  f.order_id = order_id;
  f.fill_id = fill_id;
  f.price_raw = price_raw;
  f.qty_raw = qty_raw;
  f.fee_raw = fee_raw;
  f.symbol_id = symbol_id;
  f.side = side;
  f.liquidity = static_cast<flox::run::FillLiquidity>(liquidity);
  rec->writeFill(f);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_runner_start(FloxRunnerHandle runner)
{
  FLOX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->start();
  FLOX_CAPI_LEAVE_VOID;
}

void flox_runner_stop(FloxRunnerHandle runner)
{
  FLOX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->stop();
  FLOX_CAPI_LEAVE_VOID;
}

void flox_runner_on_trade(FloxRunnerHandle runner, uint32_t symbol,
                          double price, double qty, uint8_t is_buy,
                          int64_t exchange_ts_ns)
{
  FLOX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->onTrade(symbol, price, qty, is_buy != 0, exchange_ts_ns);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_runner_on_book_snapshot(FloxRunnerHandle runner, uint32_t symbol,
                                  const double* bid_prices, const double* bid_qtys,
                                  uint32_t n_bids,
                                  const double* ask_prices, const double* ask_qtys,
                                  uint32_t n_asks,
                                  int64_t exchange_ts_ns)
{
  FLOX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->onBookSnapshot(symbol,
                                   bid_prices, bid_qtys, n_bids,
                                   ask_prices, ask_qtys, n_asks,
                                   exchange_ts_ns);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_runner_on_bar(FloxRunnerHandle runner, uint32_t symbol,
                        uint8_t bar_type, uint64_t bar_type_param,
                        double open, double high, double low, double close,
                        double volume, double buy_volume,
                        int64_t start_time_ns, int64_t end_time_ns,
                        uint8_t close_reason)
{
  FLOX_CAPI_ENTER_VOID(runner);
  toRunner(runner)->onBar(symbol, bar_type, bar_type_param,
                          open, high, low, close, volume, buy_volume,
                          start_time_ns, end_time_ns, close_reason);
  FLOX_CAPI_LEAVE_VOID;
}

// ============================================================
// FloxLiveEngine C API
// ============================================================

FloxLiveEngineHandle flox_live_engine_create(FloxRegistryHandle registry)
{
  FLOX_CAPI_ENTER(registry);
  return static_cast<FloxLiveEngineHandle>(new FloxLiveEngineImpl(toRegistry(registry)));
  FLOX_CAPI_LEAVE;
}

void flox_live_engine_destroy(FloxLiveEngineHandle engine)
{
  FLOX_CAPI_ENTER_DESTROY(engine);
  delete toLiveEngine(engine);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_live_engine_add_strategy(FloxLiveEngineHandle engine,
                                   FloxStrategyHandle strategy,
                                   FloxOnSignalCallback on_signal,
                                   void* user_data)
{
  FLOX_CAPI_ENTER_VOID(engine);
  toLiveEngine(engine)->addStrategy(toStrategy(strategy), on_signal, user_data);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_live_engine_set_risk_manager(FloxLiveEngineHandle engine, FloxRiskManagerHandle rm)
{
  FLOX_CAPI_ENTER_VOID(engine);
  toLiveEngine(engine)->setRiskManager(
      static_cast<capi_impl::FloxRiskManagerImpl*>(rm));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_live_engine_set_kill_switch(FloxLiveEngineHandle engine, FloxKillSwitchHandle ks)
{
  FLOX_CAPI_ENTER_VOID(engine);
  toLiveEngine(engine)->setKillSwitch(
      static_cast<capi_impl::FloxKillSwitchImpl*>(ks));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_live_engine_set_order_validator(FloxLiveEngineHandle engine,
                                          FloxOrderValidatorHandle ov)
{
  FLOX_CAPI_ENTER_VOID(engine);
  toLiveEngine(engine)->setOrderValidator(
      static_cast<capi_impl::FloxOrderValidatorImpl*>(ov));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_live_engine_set_pnl_tracker(FloxLiveEngineHandle engine,
                                      FloxPnLTrackerHandle tracker)
{
  FLOX_CAPI_ENTER_VOID(engine);
  toLiveEngine(engine)->setPnLTracker(
      static_cast<capi_impl::FloxPnLTrackerImpl*>(tracker));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_live_engine_set_storage_sink(FloxLiveEngineHandle engine,
                                       FloxStorageSinkHandle sink)
{
  FLOX_CAPI_ENTER_VOID(engine);
  toLiveEngine(engine)->setStorageSink(
      static_cast<capi_impl::FloxStorageSinkImpl*>(sink));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_live_engine_set_market_data_recorder(FloxLiveEngineHandle engine,
                                               FloxMarketDataRecorderHandle recorder)
{
  FLOX_CAPI_ENTER_VOID(engine);
  toLiveEngine(engine)->setMarketDataRecorder(
      static_cast<capi_impl::FloxMarketDataRecorderImpl*>(recorder));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_live_engine_set_executor(FloxLiveEngineHandle engine,
                                   FloxExecutorHandle executor)
{
  FLOX_CAPI_ENTER_VOID(engine);
  toLiveEngine(engine)->setExecutor(
      static_cast<capi_impl::FloxExecutorImpl*>(executor));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_live_engine_start(FloxLiveEngineHandle engine)
{
  FLOX_CAPI_ENTER_VOID(engine);
  toLiveEngine(engine)->start();
  FLOX_CAPI_LEAVE_VOID;
}

void flox_live_engine_stop(FloxLiveEngineHandle engine)
{
  FLOX_CAPI_ENTER_VOID(engine);
  toLiveEngine(engine)->stop();
  FLOX_CAPI_LEAVE_VOID;
}

void flox_live_engine_publish_trade(FloxLiveEngineHandle engine,
                                    uint32_t symbol,
                                    double price, double qty, uint8_t is_buy,
                                    int64_t exchange_ts_ns)
{
  FLOX_CAPI_ENTER_VOID(engine);
  toLiveEngine(engine)->publishTrade(symbol, price, qty, is_buy != 0, exchange_ts_ns);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_live_engine_publish_book_snapshot(FloxLiveEngineHandle engine,
                                            uint32_t symbol,
                                            const double* bid_prices,
                                            const double* bid_qtys,
                                            uint32_t n_bids,
                                            const double* ask_prices,
                                            const double* ask_qtys,
                                            uint32_t n_asks,
                                            int64_t exchange_ts_ns)
{
  FLOX_CAPI_ENTER_VOID(engine);
  toLiveEngine(engine)->publishBookSnapshot(symbol,
                                            bid_prices, bid_qtys, n_bids,
                                            ask_prices, ask_qtys, n_asks,
                                            exchange_ts_ns);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_live_engine_publish_bar(FloxLiveEngineHandle engine,
                                  uint32_t symbol,
                                  uint8_t bar_type, uint64_t bar_type_param,
                                  double open, double high, double low, double close,
                                  double volume, double buy_volume,
                                  int64_t start_time_ns, int64_t end_time_ns,
                                  uint8_t close_reason)
{
  FLOX_CAPI_ENTER_VOID(engine);
  toLiveEngine(engine)->publishBar(symbol, bar_type, bar_type_param,
                                   open, high, low, close, volume, buy_volume,
                                   start_time_ns, end_time_ns, close_reason);
  FLOX_CAPI_LEAVE_VOID;
}

// ============================================================
// FloxBacktestRunner C API
// ============================================================

FloxBacktestRunnerHandle flox_backtest_runner_create(FloxRegistryHandle registry,
                                                     double fee_rate,
                                                     double initial_capital)
{
  FLOX_CAPI_ENTER(registry);
  return static_cast<FloxBacktestRunnerHandle>(
      new FloxBacktestRunnerImpl(toRegistry(registry), fee_rate, initial_capital));
  FLOX_CAPI_LEAVE;
}

void flox_backtest_runner_destroy(FloxBacktestRunnerHandle h)
{
  FLOX_CAPI_ENTER_DESTROY(h);
  delete toBacktestRunner(h);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_backtest_runner_set_strategy(FloxBacktestRunnerHandle h,
                                       FloxStrategyHandle strategy)
{
  FLOX_CAPI_ENTER_VOID(h);
  toBacktestRunner(h)->setStrategy(toStrategy(strategy));
  FLOX_CAPI_LEAVE_VOID;
}

int flox_backtest_runner_run_csv(FloxBacktestRunnerHandle h,
                                 const char* path,
                                 const char* symbol,
                                 FloxBacktestStats* out)
{
  FLOX_CAPI_ENTER(h);
  return toBacktestRunner(h)->runCsv(path, symbol, out);
  FLOX_CAPI_LEAVE;
}

int flox_backtest_runner_run_tape(FloxBacktestRunnerHandle h,
                                  const char* tape_dir,
                                  FloxBacktestStats* out)
{
  FLOX_CAPI_ENTER(h);
  return toBacktestRunner(h)->runTape(tape_dir, out);
  FLOX_CAPI_LEAVE;
}

int flox_backtest_runner_run_tapes(FloxBacktestRunnerHandle h,
                                   const char* const* tape_dirs,
                                   uint32_t n_dirs,
                                   FloxBacktestStats* out)
{
  FLOX_CAPI_ENTER(h);
  return toBacktestRunner(h)->runTapes(tape_dirs, n_dirs, out);
  FLOX_CAPI_LEAVE;
}

int flox_backtest_runner_run_ohlcv(FloxBacktestRunnerHandle h,
                                   const int64_t* ts,
                                   const double* close,
                                   uint32_t n,
                                   const char* symbol,
                                   FloxBacktestStats* out)
{
  FLOX_CAPI_ENTER(h);
  return toBacktestRunner(h)->runOhlcv(ts, close, n, symbol, out);
  FLOX_CAPI_LEAVE;
}

int flox_backtest_runner_run_bars(FloxBacktestRunnerHandle h,
                                  const int64_t* start_time_ns,
                                  const int64_t* end_time_ns,
                                  const double* open,
                                  const double* high,
                                  const double* low,
                                  const double* close,
                                  const double* volume,
                                  uint32_t n,
                                  const char* symbol,
                                  uint8_t bar_type,
                                  uint64_t bar_type_param,
                                  FloxBacktestStats* out)
{
  FLOX_CAPI_ENTER(h);
  return toBacktestRunner(h)->runFullBars(start_time_ns, end_time_ns,
                                          open, high, low, close, volume,
                                          n, symbol, bar_type, bar_type_param, out);
  FLOX_CAPI_LEAVE;
}

int flox_backtest_runner_run_replay_source(FloxBacktestRunnerHandle h,
                                           FloxReplaySourceHandle source,
                                           FloxBacktestStats* out)
{
  FLOX_CAPI_ENTER(h);
  return toBacktestRunner(h)->runReplaySource(
      static_cast<capi_impl::FloxReplaySourceImpl*>(source), out);
  FLOX_CAPI_LEAVE;
}

FloxBacktestResultHandle flox_backtest_runner_take_result(FloxBacktestRunnerHandle h)
{
  FLOX_CAPI_ENTER(h);
  auto* impl = toBacktestRunner(h);
  if (!impl->lastResult.has_value())
  {
    return nullptr;
  }
  auto* out = new FloxBacktestResultImpl();
  out->config = impl->lastResult->config();
  out->result = std::make_unique<BacktestResult>(*impl->lastResult);
  return static_cast<FloxBacktestResultHandle>(out);
  FLOX_CAPI_LEAVE;
}

void flox_backtest_runner_add_execution_listener(FloxBacktestRunnerHandle h,
                                                 FloxExecutionListenerHandle listener)
{
  FLOX_CAPI_ENTER_VOID(h);
  toBacktestRunner(h)->addExecutionListener(
      static_cast<capi_impl::FloxExecutionListenerImpl*>(listener));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_backtest_runner_add_journey_tracer(FloxBacktestRunnerHandle h,
                                             FloxOrderJourneyTracerHandle tracer)
{
  FLOX_CAPI_ENTER_VOID(h);
  if (h == nullptr || tracer == nullptr)
  {
    return;
  }
  auto* runner = toBacktestRunner(h)->runner.get();
  runner->addExecutionListener(static_cast<flox::OrderJourneyTracer*>(tracer));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_backtest_runner_set_executor(FloxBacktestRunnerHandle h,
                                       FloxExecutorHandle executor)
{
  FLOX_CAPI_ENTER_VOID(h);
  toBacktestRunner(h)->setExecutor(
      static_cast<capi_impl::FloxExecutorImpl*>(executor));
  FLOX_CAPI_LEAVE_VOID;
}

// Pre-trade gate parity with the live runner. The Impl wrappers
// already inherit from the IRiskManager / IKillSwitch / IOrderValidator
// / IPnLTracker interfaces, so the cast handed to the BacktestRunner
// setters is direct.
void flox_backtest_runner_set_risk_manager(FloxBacktestRunnerHandle h,
                                           FloxRiskManagerHandle rm)
{
  FLOX_CAPI_ENTER_VOID(h);
  toBacktestRunner(h)->setRiskManager(
      static_cast<capi_impl::FloxRiskManagerImpl*>(rm));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_backtest_runner_set_kill_switch(FloxBacktestRunnerHandle h,
                                          FloxKillSwitchHandle ks)
{
  FLOX_CAPI_ENTER_VOID(h);
  toBacktestRunner(h)->setKillSwitch(
      static_cast<capi_impl::FloxKillSwitchImpl*>(ks));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_backtest_runner_set_order_validator(FloxBacktestRunnerHandle h,
                                              FloxOrderValidatorHandle ov)
{
  FLOX_CAPI_ENTER_VOID(h);
  toBacktestRunner(h)->setOrderValidator(
      static_cast<capi_impl::FloxOrderValidatorImpl*>(ov));
  FLOX_CAPI_LEAVE_VOID;
}

void flox_backtest_runner_set_pnl_tracker(FloxBacktestRunnerHandle h,
                                          FloxPnLTrackerHandle tracker)
{
  FLOX_CAPI_ENTER_VOID(h);
  toBacktestRunner(h)->setPnLTracker(
      static_cast<capi_impl::FloxPnLTrackerImpl*>(tracker));
  FLOX_CAPI_LEAVE_VOID;
}

// ============================================================
// Walk-forward
// ============================================================

namespace
{

uint32_t resolveSymbolId(SymbolRegistry* reg, const char* symbol)
{
  if (!symbol || symbol[0] == '\0')
  {
    auto all = reg->getAllSymbols();
    return all.empty() ? 0 : all.front().id;
  }
  for (const auto& info : reg->getAllSymbols())
  {
    if (info.symbol == symbol)
    {
      return info.id;
    }
  }
  return 0;
}

int64_t normalizeWfTs(int64_t t)
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

// Callers treat an empty result as "could not load" (see the bars.empty()
// check in flox_walk_forward_run_csv), which is the only failure channel this
// signature has. std::stoll/std::stod throw std::invalid_argument on a
// malformed row, and this runs under extern "C", where an escaping exception
// is undefined behaviour rather than a catchable error. Abort the whole load
// instead of skipping bad rows: a partially-parsed tape would silently produce
// a backtest over data the caller never supplied.
std::vector<OhlcvReplaySource::Bar> loadOhlcvBarsCsv(const char* path,
                                                     uint32_t symbolId) noexcept
{
  try
  {
    std::vector<OhlcvReplaySource::Bar> out;
    std::ifstream f(path);
    if (!f.is_open())
    {
      return out;
    }
    std::string line;
    std::getline(f, line);  // header
    while (std::getline(f, line))
    {
      if (line.empty())
      {
        continue;
      }
      std::istringstream ss(line);
      std::string tok;
      std::getline(ss, tok, ',');
      int64_t ts = normalizeWfTs(std::stoll(tok));
      std::getline(ss, tok, ',');  // open
      std::getline(ss, tok, ',');  // high
      std::getline(ss, tok, ',');  // low
      std::getline(ss, tok, ',');
      double c = std::stod(tok);
      OhlcvReplaySource::Bar bar;
      bar.ts_ns = ts;
      bar.price_raw = Price::fromDouble(c).raw();
      bar.symbol_id = symbolId;
      out.push_back(bar);
    }
    return out;
  }
  catch (const std::exception&)
  {
    return {};
  }
}

void fillStatsStruct(const BacktestStats& s, FloxBacktestStats* out)
{
  out->totalTrades = s.totalTrades;
  out->winningTrades = s.winningTrades;
  out->losingTrades = s.losingTrades;
  out->maxConsecutiveWins = s.maxConsecutiveWins;
  out->maxConsecutiveLosses = s.maxConsecutiveLosses;
  out->initialCapital = s.initialCapital;
  out->finalCapital = s.finalCapital;
  out->totalPnl = s.totalPnl;
  out->totalFees = s.totalFees;
  out->netPnl = s.netPnl;
  out->grossProfit = s.grossProfit;
  out->grossLoss = s.grossLoss;
  out->maxDrawdown = s.maxDrawdown;
  out->maxDrawdownPct = s.maxDrawdownPct;
  out->winRate = s.winRate;
  out->profitFactor = s.profitFactor;
  out->avgWin = s.avgWin;
  out->avgLoss = s.avgLoss;
  out->avgWinLossRatio = s.avgWinLossRatio;
  out->avgTradeDurationNs = s.avgTradeDurationNs;
  out->medianTradeDurationNs = s.medianTradeDurationNs;
  out->maxTradeDurationNs = s.maxTradeDurationNs;
  out->sharpeRatio = s.sharpeRatio;
  out->sortinoRatio = s.sortinoRatio;
  out->calmarRatio = s.calmarRatio;
  out->timeWeightedReturn = s.timeWeightedReturn;
  out->returnPct = s.returnPct;
  out->startTimeNs = s.startTimeNs.raw();
  out->endTimeNs = s.endTimeNs.raw();
}

}  // namespace

uint32_t flox_walk_forward_run_csv(FloxRegistryHandle reg_handle,
                                   const char* csv_path, const char* symbol,
                                   double fee_rate, double initial_capital,
                                   const FloxWalkForwardConfig* cfg,
                                   FloxWalkForwardFactoryFn factory,
                                   void* user_data,
                                   FloxWalkForwardFold* folds_out,
                                   uint32_t max_folds)
{
  FLOX_CAPI_ENTER(reg_handle);
  if (!cfg || !csv_path || !factory)
  {
    return 0;
  }
  SymbolRegistry* reg = toRegistry(reg_handle);
  uint32_t symId = resolveSymbolId(reg, symbol);
  std::vector<OhlcvReplaySource::Bar> bars = loadOhlcvBarsCsv(csv_path, symId);
  if (bars.empty())
  {
    return 0;
  }

  BacktestConfig bcfg{};
  bcfg.feeRate = fee_rate;
  bcfg.initialCapital = initial_capital;
  bcfg.usePercentageFee = true;

  WalkForwardConfig wcfg{};
  wcfg.mode = (cfg->mode == 0) ? WalkForwardMode::Anchored
                               : WalkForwardMode::Sliding;
  wcfg.trainSize = cfg->train_size;
  wcfg.testSize = cfg->test_size;
  wcfg.step = cfg->step;
  wcfg.minTrainSize = cfg->min_train_size;

  // If the caller only wants a count, compute it from the config + bar
  // count without invoking the factory or running anything. This lets
  // bindings size the output buffer cheaply.
  if (!folds_out || max_folds == 0)
  {
    const uint64_t n = static_cast<uint64_t>(bars.size());
    const uint64_t step = wcfg.step == 0 ? wcfg.testSize : wcfg.step;
    if (wcfg.testSize == 0 || step == 0)
    {
      return 0;
    }
    if (wcfg.mode == WalkForwardMode::Anchored)
    {
      const uint64_t firstSplit =
          wcfg.minTrainSize > 0 ? wcfg.minTrainSize : wcfg.testSize;
      if (firstSplit + wcfg.testSize > n)
      {
        return 0;
      }
      return static_cast<uint32_t>(
          (n - firstSplit - wcfg.testSize) / step + 1);
    }
    if (wcfg.trainSize + wcfg.testSize > n)
    {
      return 0;
    }
    return static_cast<uint32_t>(
        (n - wcfg.trainSize - wcfg.testSize) / step + 1);
  }

  WalkForwardRunner wfr(bcfg, wcfg);
  wfr.setStrategyFactory([factory, user_data](std::size_t foldIdx) -> IStrategy*
                         {
                           FloxStrategyHandle h = factory(user_data, foldIdx);
                           return reinterpret_cast<BridgeStrategy*>(h); });

  auto folds = wfr.run(bars);
  const uint32_t total = static_cast<uint32_t>(folds.size());
  const uint32_t n = (total < max_folds) ? total : max_folds;
  for (uint32_t i = 0; i < n; ++i)
  {
    const auto& f = folds[i];
    folds_out[i].fold_index = f.foldIndex;
    folds_out[i].train_start_bar = f.trainStartBar;
    folds_out[i].train_end_bar = f.trainEndBar;
    folds_out[i].test_start_bar = f.testStartBar;
    folds_out[i].test_end_bar = f.testEndBar;
    folds_out[i].train_start_ns = static_cast<int64_t>(f.trainStartNs);
    folds_out[i].train_end_ns = static_cast<int64_t>(f.trainEndNs);
    folds_out[i].test_start_ns = static_cast<int64_t>(f.testStartNs);
    folds_out[i].test_end_ns = static_cast<int64_t>(f.testEndNs);
    fillStatsStruct(f.trainStats, &folds_out[i].train_stats);
    fillStatsStruct(f.testStats, &folds_out[i].test_stats);
  }
  return n;
  FLOX_CAPI_LEAVE;
}

// ============================================================
// Grid search (sequential)
// ============================================================

struct FloxGridSearchImpl
{
  GridSearch core;
};

FloxGridSearchHandle flox_grid_search_create()
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return static_cast<FloxGridSearchHandle>(new FloxGridSearchImpl());
  FLOX_CAPI_LEAVE;
}

void flox_grid_search_destroy(FloxGridSearchHandle gs)
{
  FLOX_CAPI_ENTER_DESTROY(gs);
  delete static_cast<FloxGridSearchImpl*>(gs);
  FLOX_CAPI_LEAVE_VOID;
}

void flox_grid_search_add_axis(FloxGridSearchHandle gs,
                               const double* values, uint32_t num_values)
{
  FLOX_CAPI_ENTER_VOID(gs);
  if (!gs || !values)
  {
    return;
  }
  std::vector<double> v(values, values + num_values);
  static_cast<FloxGridSearchImpl*>(gs)->core.addAxis(std::move(v));
  FLOX_CAPI_LEAVE_VOID;
}

uint64_t flox_grid_search_total(FloxGridSearchHandle gs)
{
  FLOX_CAPI_ENTER(gs);
  return gs ? static_cast<FloxGridSearchImpl*>(gs)->core.totalCombinations() : 0;
  FLOX_CAPI_LEAVE;
}

uint32_t flox_grid_search_params_for_index(FloxGridSearchHandle gs,
                                           uint64_t index,
                                           double* params_out,
                                           uint32_t max_params)
{
  FLOX_CAPI_ENTER(gs);
  if (!gs)
  {
    return 0;
  }
  auto p = static_cast<FloxGridSearchImpl*>(gs)->core.paramsForIndex(index);
  if (!params_out)
  {
    return static_cast<uint32_t>(p.size());
  }
  const uint32_t n = std::min(static_cast<uint32_t>(p.size()), max_params);
  for (uint32_t i = 0; i < n; ++i)
  {
    params_out[i] = p[i];
  }
  return n;
  FLOX_CAPI_LEAVE;
}

uint64_t flox_grid_search_run(FloxGridSearchHandle gs,
                              FloxGridSearchFactoryFn factory,
                              void* user_data,
                              FloxBacktestStats* stats_out,
                              uint32_t max_results)
{
  FLOX_CAPI_ENTER(gs);
  if (!gs || !factory)
  {
    return 0;
  }
  auto* impl = static_cast<FloxGridSearchImpl*>(gs);
  const uint64_t total = impl->core.totalCombinations();
  if (!stats_out)
  {
    return total;
  }
  // Run sequentially via direct calls to factory; we don't go through
  // GridSearch::run because the factory expects a C-side stats fill,
  // not a BacktestResult.
  const uint64_t n = std::min<uint64_t>(total, max_results);
  for (uint64_t i = 0; i < n; ++i)
  {
    auto params = impl->core.paramsForIndex(i);
    FloxBacktestStats s{};
    factory(user_data, i, params.data(),
            static_cast<uint32_t>(params.size()), &s);
    stats_out[i] = s;
  }
  return n;
  FLOX_CAPI_LEAVE;
}

// ============================================================
// Heatmap rendering
// ============================================================

uint64_t flox_render_heatmap_html(const FloxHeatmapData* data,
                                  char* out_buf, uint64_t max_size)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  if (!data || data->z == nullptr || data->rows == 0 || data->cols == 0)
  {
    return 0;
  }
  flox::report::HeatmapData hd;
  const std::size_t n = static_cast<std::size_t>(data->rows) * data->cols;
  hd.z.assign(data->z, data->z + n);
  hd.rows = data->rows;
  hd.cols = data->cols;
  if (data->row_labels && data->num_row_labels > 0)
  {
    hd.rowLabels.reserve(data->num_row_labels);
    for (uint32_t i = 0; i < data->num_row_labels; ++i)
    {
      hd.rowLabels.emplace_back(data->row_labels[i] ? data->row_labels[i] : "");
    }
  }
  if (data->col_labels && data->num_col_labels > 0)
  {
    hd.colLabels.reserve(data->num_col_labels);
    for (uint32_t i = 0; i < data->num_col_labels; ++i)
    {
      hd.colLabels.emplace_back(data->col_labels[i] ? data->col_labels[i] : "");
    }
  }
  if (data->title)
  {
    hd.title = data->title;
  }
  if (data->x_axis_name)
  {
    hd.xAxisName = data->x_axis_name;
  }
  if (data->y_axis_name)
  {
    hd.yAxisName = data->y_axis_name;
  }
  if (data->metric_name)
  {
    hd.metricName = data->metric_name;
  }

  std::string html = flox::report::renderHeatmapHtml(hd);
  const uint64_t total = static_cast<uint64_t>(html.size());
  if (!out_buf)
  {
    return total;
  }
  const uint64_t to_copy = (total < max_size) ? total : max_size;
  std::memcpy(out_buf, html.data(), to_copy);
  return total;
  FLOX_CAPI_LEAVE;
}

// ── Latency models ────────────────────────────────────────────────

namespace
{
flox::LatencyModel* asLatency(FloxLatencyModelHandle h)
{
  return static_cast<flox::LatencyModel*>(h);
}
}  // namespace

extern "C" FloxLatencyModelHandle flox_latency_constant_create(int64_t feed_ns,
                                                               int64_t order_ns,
                                                               int64_t fill_ns)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  try
  {
    return new flox::ConstantLatency(feed_ns, order_ns, fill_ns);
  }
  catch (...)
  {
    return nullptr;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" FloxLatencyModelHandle flox_latency_gaussian_create(double feed_mean_ns,
                                                               double feed_stddev_ns,
                                                               double order_mean_ns,
                                                               double order_stddev_ns,
                                                               double fill_mean_ns,
                                                               double fill_stddev_ns,
                                                               uint64_t seed)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  try
  {
    return new flox::GaussianLatency(feed_mean_ns, feed_stddev_ns,
                                     order_mean_ns, order_stddev_ns,
                                     fill_mean_ns, fill_stddev_ns, seed);
  }
  catch (...)
  {
    return nullptr;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" FloxLatencyModelHandle flox_latency_exponential_create(double feed_mean_ns,
                                                                  double order_mean_ns,
                                                                  double fill_mean_ns,
                                                                  uint64_t seed)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  try
  {
    return new flox::ExponentialLatency(feed_mean_ns, order_mean_ns, fill_mean_ns, seed);
  }
  catch (...)
  {
    return nullptr;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" FloxLatencyModelHandle flox_latency_empirical_create(const int64_t* feed_samples,
                                                                size_t feed_count,
                                                                const int64_t* order_samples,
                                                                size_t order_count,
                                                                const int64_t* fill_samples,
                                                                size_t fill_count,
                                                                uint64_t seed)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  try
  {
    std::vector<int64_t> feed(feed_samples, feed_samples + feed_count);
    std::vector<int64_t> order(order_samples, order_samples + order_count);
    std::vector<int64_t> fill(fill_samples, fill_samples + fill_count);
    return new flox::EmpiricalLatency(std::move(feed), std::move(order),
                                      std::move(fill), seed);
  }
  catch (...)
  {
    return nullptr;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_latency_destroy(FloxLatencyModelHandle model)
{
  FLOX_CAPI_ENTER_DESTROY(model);
  delete asLatency(model);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" int64_t flox_latency_feed_delay(FloxLatencyModelHandle model)
{
  FLOX_CAPI_ENTER(model);
  return model ? asLatency(model)->feedDelay() : 0;
  FLOX_CAPI_LEAVE;
}

extern "C" int64_t flox_latency_order_delay(FloxLatencyModelHandle model)
{
  FLOX_CAPI_ENTER(model);
  return model ? asLatency(model)->orderDelay() : 0;
  FLOX_CAPI_LEAVE;
}

extern "C" int64_t flox_latency_fill_delay(FloxLatencyModelHandle model)
{
  FLOX_CAPI_ENTER(model);
  return model ? asLatency(model)->fillDelay() : 0;
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_latency_sample(FloxLatencyModelHandle model, FloxLatencySample* out)
{
  FLOX_CAPI_ENTER_VOID(model);
  if (!model || !out)
  {
    return;
  }
  flox::LatencySample s = asLatency(model)->sample();
  out->feed_ns = s.feed_ns;
  out->order_ns = s.order_ns;
  out->fill_ns = s.fill_ns;
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_latency_reset(FloxLatencyModelHandle model, uint64_t seed)
{
  FLOX_CAPI_ENTER_VOID(model);
  if (model)
  {
    asLatency(model)->reset(seed);
  }
  FLOX_CAPI_LEAVE_VOID;
}

// ── Tape diff ─────────────────────────────────────────────────────

namespace
{
flox::replay::TapeDiffResult* asDiff(FloxTapeDiffHandle h)
{
  return static_cast<flox::replay::TapeDiffResult*>(h);
}
}  // namespace

extern "C" FloxTapeDiffHandle flox_tape_diff_create(const char* left_path,
                                                    const char* right_path,
                                                    uint32_t max_mismatches,
                                                    int64_t field_tolerance_ns)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  if (!left_path || !right_path)
  {
    return nullptr;
  }
  if (!std::filesystem::is_directory(left_path) ||
      !std::filesystem::is_directory(right_path))
  {
    return nullptr;
  }
  try
  {
    flox::replay::TapeDiffOptions opts;
    opts.max_mismatches = max_mismatches;
    opts.field_tolerance_ns = field_tolerance_ns;
    auto* result = new flox::replay::TapeDiffResult(
        flox::replay::diffTapes(left_path, right_path, opts));
    return result;
  }
  catch (...)
  {
    return nullptr;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_tape_diff_destroy(FloxTapeDiffHandle handle)
{
  FLOX_CAPI_ENTER_DESTROY(handle);
  delete asDiff(handle);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" uint64_t flox_tape_diff_left_count(FloxTapeDiffHandle handle)
{
  FLOX_CAPI_ENTER(handle);
  return handle ? asDiff(handle)->left_count : 0;
  FLOX_CAPI_LEAVE;
}

extern "C" uint64_t flox_tape_diff_right_count(FloxTapeDiffHandle handle)
{
  FLOX_CAPI_ENTER(handle);
  return handle ? asDiff(handle)->right_count : 0;
  FLOX_CAPI_LEAVE;
}

extern "C" uint8_t flox_tape_diff_first_divergence(FloxTapeDiffHandle handle,
                                                   uint64_t* out_index)
{
  FLOX_CAPI_ENTER(handle);
  if (!handle)
  {
    return 0;
  }
  auto& r = *asDiff(handle);
  if (!r.first_divergence_index.has_value())
  {
    return 0;
  }
  if (out_index)
  {
    *out_index = *r.first_divergence_index;
  }
  return 1;
  FLOX_CAPI_LEAVE;
}

extern "C" uint8_t flox_tape_diff_equal(FloxTapeDiffHandle handle)
{
  FLOX_CAPI_ENTER(handle);
  return (handle && asDiff(handle)->equal) ? 1 : 0;
  FLOX_CAPI_LEAVE;
}

extern "C" uint64_t flox_tape_diff_mismatch_count(FloxTapeDiffHandle handle)
{
  FLOX_CAPI_ENTER(handle);
  return handle ? asDiff(handle)->mismatches.size() : 0;
  FLOX_CAPI_LEAVE;
}

extern "C" uint64_t flox_tape_diff_copy_mismatches(FloxTapeDiffHandle handle,
                                                   FloxTapeDiffMismatch* out,
                                                   uint64_t max_entries)
{
  FLOX_CAPI_ENTER(handle);
  if (!handle)
  {
    return 0;
  }
  auto& r = *asDiff(handle);
  const uint64_t total = r.mismatches.size();
  if (!out)
  {
    return total;
  }
  const uint64_t to_copy = std::min<uint64_t>(total, max_entries);
  for (uint64_t i = 0; i < to_copy; ++i)
  {
    const auto& m = r.mismatches[i];
    out[i].index = m.index;
    out[i].left.exchange_ts_ns = m.left.exchange_ts_ns;
    out[i].left.price_raw = m.left.price_raw;
    out[i].left.qty_raw = m.left.qty_raw;
    out[i].left.symbol_id = m.left.symbol_id;
    out[i].left.side = m.left.side;
    out[i].right.exchange_ts_ns = m.right.exchange_ts_ns;
    out[i].right.price_raw = m.right.price_raw;
    out[i].right.qty_raw = m.right.qty_raw;
    out[i].right.symbol_id = m.right.symbol_id;
    out[i].right.side = m.right.side;
  }
  return to_copy;
  FLOX_CAPI_LEAVE;
}

// ── Portfolio risk aggregator ─────────────────────────────────────

namespace
{
flox::risk::PortfolioRiskAggregator* asPortfolio(FloxPortfolioRiskHandle h)
{
  return static_cast<flox::risk::PortfolioRiskAggregator*>(h);
}

// Defined alongside the breach scratch table further down; declared here
// because _destroy has to drop the entry before the address is recycled.
void eraseBreachScratch(FloxPortfolioRiskHandle handle) noexcept;

flox::risk::RiskRules unpackRules(const FloxPortfolioRiskRules* r)
{
  flox::risk::RiskRules out;
  if (!r)
  {
    return out;
  }
  if (r->has_max_drawdown_pct)
  {
    out.max_drawdown_pct = r->max_drawdown_pct;
  }
  if (r->has_max_daily_loss)
  {
    out.max_daily_loss = r->max_daily_loss;
  }
  if (r->has_max_gross_exposure)
  {
    out.max_gross_exposure = r->max_gross_exposure;
  }
  if (r->has_max_concentration_pct)
  {
    out.max_concentration_pct = r->max_concentration_pct;
  }
  return out;
}
}  // namespace

extern "C" FloxPortfolioRiskHandle flox_portfolio_risk_create(
    const FloxPortfolioRiskRules* rules, double initial_equity)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  try
  {
    return new flox::risk::PortfolioRiskAggregator(unpackRules(rules), initial_equity);
  }
  catch (...)
  {
    return nullptr;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_portfolio_risk_destroy(FloxPortfolioRiskHandle handle)
{
  FLOX_CAPI_ENTER_DESTROY(handle);
  eraseBreachScratch(handle);
  delete asPortfolio(handle);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_portfolio_risk_update(FloxPortfolioRiskHandle handle,
                                           const char* name,
                                           const FloxStrategyAccountFields* fields,
                                           uint8_t field_mask)
{
  FLOX_CAPI_ENTER_VOID(handle);
  if (!handle || !name || !fields)
  {
    return;
  }
  flox::risk::StrategyAccount row;
  row.name = name;
  row.realized_pnl = fields->realized_pnl;
  row.unrealized_pnl = fields->unrealized_pnl;
  row.fees = fields->fees;
  row.gross_exposure = fields->gross_exposure;
  row.net_exposure = fields->net_exposure;
  row.trade_count = fields->trade_count;
  asPortfolio(handle)->update(name, row, field_mask);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_portfolio_risk_remove(FloxPortfolioRiskHandle handle, const char* name)
{
  FLOX_CAPI_ENTER_VOID(handle);
  if (!handle || !name)
  {
    return;
  }
  asPortfolio(handle)->remove(name);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_portfolio_risk_reset_kill_switch(FloxPortfolioRiskHandle handle)
{
  FLOX_CAPI_ENTER_VOID(handle);
  if (handle)
  {
    asPortfolio(handle)->resetKillSwitch();
  }
  FLOX_CAPI_LEAVE_VOID;
}

namespace
{
// Scratch storage behind the count-then-read pair, holding the strings a
// FloxBreach points into.
//
// It used to be a thread_local map keyed by the handle address, which failed
// twice over. Splitting the pair across threads -- the one reason a C layer
// exists at all -- left the reader with nothing and a zero return, which
// reads exactly like "no breaches". And the entry outlived the handle, so
// when the allocator handed the same address to a new object, a freshly
// created, clean risk handle answered breach_at with the destroyed session's
// max_daily_loss breach and reported success.
//
// One process-wide table under a mutex closes both: every thread sees the
// same entry, and _destroy erases it, so a reused address starts empty.
struct PortfolioBreachScratch
{
  std::vector<flox::risk::Breach> breaches;
};

std::mutex g_breach_scratch_mutex;
std::map<FloxPortfolioRiskHandle, PortfolioBreachScratch> g_breach_scratch;

void eraseBreachScratch(FloxPortfolioRiskHandle handle) noexcept
{
  try
  {
    const std::lock_guard<std::mutex> lock(g_breach_scratch_mutex);
    g_breach_scratch.erase(handle);
  }
  catch (...)
  {
  }
}

void writeBreach(FloxBreach* out, const flox::risk::Breach& b)
{
  if (!out)
  {
    return;
  }
  out->rule = b.rule.c_str();
  out->value = b.value;
  out->limit = b.limit;
  out->detail = b.detail.c_str();
}
}  // namespace

extern "C" uint8_t flox_portfolio_risk_check_order(FloxPortfolioRiskHandle handle,
                                                   const char* strategy, double notional,
                                                   const char* side, FloxBreach* out_breach)
{
  FLOX_CAPI_ENTER(handle);
  if (!handle)
  {
    return 0;
  }
  auto opt = asPortfolio(handle)->checkOrder(strategy ? strategy : "",
                                             notional, side ? side : "");
  if (!opt.has_value())
  {
    return 0;
  }
  const std::lock_guard<std::mutex> lock(g_breach_scratch_mutex);
  auto& scratch = g_breach_scratch[handle];
  scratch.breaches = {*opt};
  writeBreach(out_breach, scratch.breaches.front());
  return 1;
  FLOX_CAPI_LEAVE;
}

extern "C" double flox_portfolio_risk_total_daily_pnl(FloxPortfolioRiskHandle handle)
{
  FLOX_CAPI_ENTER(handle);
  return handle ? asPortfolio(handle)->snapshot().total_daily_pnl : 0.0;
  FLOX_CAPI_LEAVE;
}

extern "C" double flox_portfolio_risk_total_gross_exposure(FloxPortfolioRiskHandle handle)
{
  FLOX_CAPI_ENTER(handle);
  return handle ? asPortfolio(handle)->snapshot().total_gross_exposure : 0.0;
  FLOX_CAPI_LEAVE;
}

extern "C" double flox_portfolio_risk_current_equity(FloxPortfolioRiskHandle handle)
{
  FLOX_CAPI_ENTER(handle);
  return handle ? asPortfolio(handle)->snapshot().current_equity : 0.0;
  FLOX_CAPI_LEAVE;
}

extern "C" double flox_portfolio_risk_drawdown_pct(FloxPortfolioRiskHandle handle)
{
  FLOX_CAPI_ENTER(handle);
  return handle ? asPortfolio(handle)->snapshot().drawdown_pct : 0.0;
  FLOX_CAPI_LEAVE;
}

extern "C" uint8_t flox_portfolio_risk_kill_switch_active(FloxPortfolioRiskHandle handle)
{
  FLOX_CAPI_ENTER(handle);
  return (handle && asPortfolio(handle)->snapshot().kill_switch_active) ? 1 : 0;
  FLOX_CAPI_LEAVE;
}

extern "C" uint64_t flox_portfolio_risk_breach_count(FloxPortfolioRiskHandle handle)
{
  FLOX_CAPI_ENTER(handle);
  if (!handle)
  {
    return 0;
  }
  auto snap = asPortfolio(handle)->snapshot();
  const std::lock_guard<std::mutex> lock(g_breach_scratch_mutex);
  auto& scratch = g_breach_scratch[handle];
  scratch.breaches = std::move(snap.active_breaches);
  return scratch.breaches.size();
  FLOX_CAPI_LEAVE;
}

extern "C" uint8_t flox_portfolio_risk_breach_at(FloxPortfolioRiskHandle handle,
                                                 uint64_t index, FloxBreach* out)
{
  FLOX_CAPI_ENTER(handle);
  if (!handle || !out)
  {
    return 0;
  }
  const std::lock_guard<std::mutex> lock(g_breach_scratch_mutex);
  auto it = g_breach_scratch.find(handle);
  if (it == g_breach_scratch.end())
  {
    return 0;
  }
  if (index >= it->second.breaches.size())
  {
    return 0;
  }
  writeBreach(out, it->second.breaches[index]);
  return 1;
  FLOX_CAPI_LEAVE;
}

extern "C" uint64_t flox_portfolio_risk_account_count(FloxPortfolioRiskHandle handle)
{
  FLOX_CAPI_ENTER(handle);
  return handle ? asPortfolio(handle)->snapshot().accounts.size() : 0;
  FLOX_CAPI_LEAVE;
}

// ── Execution algorithms ──────────────────────────────────────────

namespace
{
flox::execution::ExecutionAlgo* asAlgo(FloxExecAlgoHandle h)
{
  return static_cast<flox::execution::ExecutionAlgo*>(h);
}
}  // namespace

extern "C" FloxExecAlgoHandle flox_exec_twap_create(double target_qty, uint8_t side,
                                                    uint32_t symbol, uint8_t type,
                                                    double limit_price,
                                                    int64_t duration_ns,
                                                    uint32_t slice_count,
                                                    int64_t start_time_ns)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  try
  {
    return new flox::execution::TWAPExecutor(
        target_qty,
        static_cast<flox::execution::Side>(side),
        symbol,
        static_cast<flox::execution::OrderType>(type),
        limit_price,
        duration_ns, slice_count, start_time_ns);
  }
  catch (...)
  {
    return nullptr;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" FloxExecAlgoHandle flox_exec_vwap_create(double target_qty, uint8_t side,
                                                    uint32_t symbol, uint8_t type,
                                                    double limit_price,
                                                    const int64_t* volume_curve_ts,
                                                    const double* volume_curve_vol,
                                                    size_t n)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  try
  {
    std::vector<std::pair<int64_t, double>> curve;
    curve.reserve(n);
    for (size_t i = 0; i < n; ++i)
    {
      curve.emplace_back(volume_curve_ts[i], volume_curve_vol[i]);
    }
    return new flox::execution::VWAPExecutor(
        target_qty,
        static_cast<flox::execution::Side>(side),
        symbol,
        static_cast<flox::execution::OrderType>(type),
        limit_price,
        std::move(curve));
  }
  catch (...)
  {
    return nullptr;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" FloxExecAlgoHandle flox_exec_iceberg_create(double target_qty, uint8_t side,
                                                       uint32_t symbol, uint8_t type,
                                                       double limit_price,
                                                       double visible_qty)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  try
  {
    return new flox::execution::IcebergExecutor(
        target_qty,
        static_cast<flox::execution::Side>(side),
        symbol,
        static_cast<flox::execution::OrderType>(type),
        limit_price, visible_qty);
  }
  catch (...)
  {
    return nullptr;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" FloxExecAlgoHandle flox_exec_pov_create(double target_qty, uint8_t side,
                                                   uint32_t symbol, uint8_t type,
                                                   double limit_price,
                                                   double participation_rate,
                                                   double min_slice_qty)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  try
  {
    return new flox::execution::POVExecutor(
        target_qty,
        static_cast<flox::execution::Side>(side),
        symbol,
        static_cast<flox::execution::OrderType>(type),
        limit_price, participation_rate, min_slice_qty);
  }
  catch (...)
  {
    return nullptr;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_exec_destroy(FloxExecAlgoHandle handle)
{
  FLOX_CAPI_ENTER_DESTROY(handle);
  delete asAlgo(handle);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_exec_step(FloxExecAlgoHandle handle, int64_t now_ns)
{
  FLOX_CAPI_ENTER_VOID(handle);
  if (handle)
  {
    asAlgo(handle)->step(now_ns);
  }
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_exec_report_fill(FloxExecAlgoHandle handle, double qty)
{
  FLOX_CAPI_ENTER_VOID(handle);
  if (handle)
  {
    asAlgo(handle)->reportFill(qty);
  }
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_exec_observe_volume(FloxExecAlgoHandle handle, double qty)
{
  FLOX_CAPI_ENTER_VOID(handle);
  if (handle)
  {
    asAlgo(handle)->observeVolume(qty);
  }
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" size_t flox_exec_pending_count(FloxExecAlgoHandle handle)
{
  FLOX_CAPI_ENTER(handle);
  return handle ? asAlgo(handle)->pending().size() : 0;
  FLOX_CAPI_LEAVE;
}

extern "C" uint8_t flox_exec_pending_at(FloxExecAlgoHandle handle, size_t index,
                                        FloxExecChildOrder* out)
{
  FLOX_CAPI_ENTER(handle);
  if (!handle || !out)
  {
    return 0;
  }
  const auto& p = asAlgo(handle)->pending();
  if (index >= p.size())
  {
    return 0;
  }
  const auto& c = p[index];
  out->order_id = c.order_id;
  out->timestamp_ns = c.timestamp_ns;
  out->qty = c.qty;
  out->price = c.price;
  out->type = static_cast<uint8_t>(c.type);
  return 1;
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_exec_clear_pending(FloxExecAlgoHandle handle)
{
  FLOX_CAPI_ENTER_VOID(handle);
  if (handle)
  {
    asAlgo(handle)->clearPending();
  }
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" double flox_exec_target_qty(FloxExecAlgoHandle handle)
{
  FLOX_CAPI_ENTER(handle);
  return handle ? asAlgo(handle)->targetQty() : 0.0;
  FLOX_CAPI_LEAVE;
}

extern "C" double flox_exec_submitted_qty(FloxExecAlgoHandle handle)
{
  FLOX_CAPI_ENTER(handle);
  return handle ? asAlgo(handle)->submittedQty() : 0.0;
  FLOX_CAPI_LEAVE;
}

extern "C" double flox_exec_filled_qty(FloxExecAlgoHandle handle)
{
  FLOX_CAPI_ENTER(handle);
  return handle ? asAlgo(handle)->filledQty() : 0.0;
  FLOX_CAPI_LEAVE;
}

extern "C" double flox_exec_remaining_qty(FloxExecAlgoHandle handle)
{
  FLOX_CAPI_ENTER(handle);
  return handle ? asAlgo(handle)->remainingQty() : 0.0;
  FLOX_CAPI_LEAVE;
}

extern "C" uint8_t flox_exec_is_done(FloxExecAlgoHandle handle)
{
  FLOX_CAPI_ENTER(handle);
  return (handle && asAlgo(handle)->isDone()) ? 1 : 0;
  FLOX_CAPI_LEAVE;
}

// ── Delta book compression ────────────────────────────────────────

namespace
{
flox::replay::DeltaBookEncoder* asEncoder(FloxDeltaBookEncoderHandle h)
{
  return static_cast<flox::replay::DeltaBookEncoder*>(h);
}
flox::replay::DeltaBookReplayer* asReplayer(FloxDeltaBookReplayerHandle h)
{
  return static_cast<flox::replay::DeltaBookReplayer*>(h);
}

std::vector<flox::replay::BookLevel> toLevels(const FloxBookLevel* arr, size_t n)
{
  std::vector<flox::replay::BookLevel> out(n);
  for (size_t i = 0; i < n; ++i)
  {
    out[i].price_raw = arr[i].price_raw;
    out[i].qty_raw = arr[i].quantity_raw;
  }
  return out;
}

uint64_t copyLevelsTo(const std::vector<flox::replay::BookLevel>& src,
                      FloxBookLevel* out, uint64_t max_entries)
{
  const uint64_t n = std::min<uint64_t>(src.size(), max_entries);
  for (uint64_t i = 0; i < n; ++i)
  {
    out[i].price_raw = src[i].price_raw;
    out[i].quantity_raw = src[i].qty_raw;
  }
  return n;
}

// Per-handle scratch for last encode/replay output. The C ABI returns
// counts plus a follow-up copy() call, matching the pattern used by
// flox_tape_diff and flox_portfolio_risk for variable-length data.
struct EncoderScratch
{
  std::vector<flox::replay::BookLevel> bids;
  std::vector<flox::replay::BookLevel> asks;
};

// Same reasoning as the portfolio risk scratch above: process-wide under a
// mutex, erased by _destroy, so the encode/copy pair works across threads and
// a reused address cannot serve a dead handle's levels.
std::mutex g_encoder_scratch_mutex;
std::map<FloxDeltaBookEncoderHandle, EncoderScratch> g_encoder_scratch;
std::map<FloxDeltaBookReplayerHandle, EncoderScratch> g_replayer_scratch;
}  // namespace

extern "C" FloxDeltaBookEncoderHandle flox_delta_book_encoder_create(uint32_t anchor_every)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  try
  {
    return new flox::replay::DeltaBookEncoder(anchor_every);
  }
  catch (...)
  {
    return nullptr;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_delta_book_encoder_destroy(FloxDeltaBookEncoderHandle handle)
{
  FLOX_CAPI_ENTER_DESTROY(handle);
  {
    const std::lock_guard<std::mutex> lock(g_encoder_scratch_mutex);
    g_encoder_scratch.erase(handle);
  }
  delete asEncoder(handle);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_delta_book_encoder_reset(FloxDeltaBookEncoderHandle handle, uint32_t symbol_id)
{
  FLOX_CAPI_ENTER_VOID(handle);
  if (handle)
  {
    asEncoder(handle)->reset(symbol_id);
  }
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_delta_book_encoder_reset_all(FloxDeltaBookEncoderHandle handle)
{
  FLOX_CAPI_ENTER_VOID(handle);
  if (handle)
  {
    asEncoder(handle)->resetAll();
  }
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_delta_book_encoder_encode(FloxDeltaBookEncoderHandle handle,
                                               uint32_t symbol_id,
                                               const FloxBookLevel* bids, size_t bid_count,
                                               const FloxBookLevel* asks, size_t ask_count,
                                               uint8_t* out_is_delta,
                                               uint64_t* out_bid_count,
                                               uint64_t* out_ask_count)
{
  FLOX_CAPI_ENTER_VOID(handle);
  if (!handle)
  {
    return;
  }
  auto bids_in = toLevels(bids, bid_count);
  auto asks_in = toLevels(asks, ask_count);
  auto result = asEncoder(handle)->encode(symbol_id, bids_in, asks_in);
  const std::lock_guard<std::mutex> lock(g_encoder_scratch_mutex);
  auto& scratch = g_encoder_scratch[handle];
  scratch.bids = std::move(result.bids);
  scratch.asks = std::move(result.asks);
  if (out_is_delta)
  {
    *out_is_delta = result.is_delta ? 1 : 0;
  }
  if (out_bid_count)
  {
    *out_bid_count = scratch.bids.size();
  }
  if (out_ask_count)
  {
    *out_ask_count = scratch.asks.size();
  }
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" uint64_t flox_delta_book_encoder_copy_bids(FloxDeltaBookEncoderHandle handle,
                                                      FloxBookLevel* out, uint64_t max_entries)
{
  FLOX_CAPI_ENTER(handle);
  if (!handle || !out)
  {
    return 0;
  }
  const std::lock_guard<std::mutex> lock(g_encoder_scratch_mutex);
  auto it = g_encoder_scratch.find(handle);
  if (it == g_encoder_scratch.end())
  {
    return 0;
  }
  return copyLevelsTo(it->second.bids, out, max_entries);
  FLOX_CAPI_LEAVE;
}

extern "C" uint64_t flox_delta_book_encoder_copy_asks(FloxDeltaBookEncoderHandle handle,
                                                      FloxBookLevel* out, uint64_t max_entries)
{
  FLOX_CAPI_ENTER(handle);
  if (!handle || !out)
  {
    return 0;
  }
  const std::lock_guard<std::mutex> lock(g_encoder_scratch_mutex);
  auto it = g_encoder_scratch.find(handle);
  if (it == g_encoder_scratch.end())
  {
    return 0;
  }
  return copyLevelsTo(it->second.asks, out, max_entries);
  FLOX_CAPI_LEAVE;
}

extern "C" FloxDeltaBookReplayerHandle flox_delta_book_replayer_create(void)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  try
  {
    return new flox::replay::DeltaBookReplayer();
  }
  catch (...)
  {
    return nullptr;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_delta_book_replayer_destroy(FloxDeltaBookReplayerHandle handle)
{
  FLOX_CAPI_ENTER_DESTROY(handle);
  {
    const std::lock_guard<std::mutex> lock(g_encoder_scratch_mutex);
    g_replayer_scratch.erase(handle);
  }
  delete asReplayer(handle);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_delta_book_replayer_reset(FloxDeltaBookReplayerHandle handle, uint32_t symbol_id)
{
  FLOX_CAPI_ENTER_VOID(handle);
  if (handle)
  {
    asReplayer(handle)->reset(symbol_id);
  }
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_delta_book_replayer_apply(FloxDeltaBookReplayerHandle handle,
                                               uint8_t type, uint32_t symbol_id,
                                               const FloxBookLevel* bids, size_t bid_count,
                                               const FloxBookLevel* asks, size_t ask_count,
                                               uint64_t* out_bid_count,
                                               uint64_t* out_ask_count)
{
  FLOX_CAPI_ENTER_VOID(handle);
  if (!handle)
  {
    return;
  }
  auto bids_in = toLevels(bids, bid_count);
  auto asks_in = toLevels(asks, ask_count);
  auto snap = asReplayer(handle)->apply(type, symbol_id, bids_in, asks_in);
  const std::lock_guard<std::mutex> lock(g_encoder_scratch_mutex);
  auto& scratch = g_replayer_scratch[handle];
  scratch.bids = std::move(snap.bids);
  scratch.asks = std::move(snap.asks);
  if (out_bid_count)
  {
    *out_bid_count = scratch.bids.size();
  }
  if (out_ask_count)
  {
    *out_ask_count = scratch.asks.size();
  }
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" uint64_t flox_delta_book_replayer_copy_bids(FloxDeltaBookReplayerHandle handle,
                                                       FloxBookLevel* out, uint64_t max_entries)
{
  FLOX_CAPI_ENTER(handle);
  if (!handle || !out)
  {
    return 0;
  }
  const std::lock_guard<std::mutex> lock(g_encoder_scratch_mutex);
  auto it = g_replayer_scratch.find(handle);
  if (it == g_replayer_scratch.end())
  {
    return 0;
  }
  return copyLevelsTo(it->second.bids, out, max_entries);
  FLOX_CAPI_LEAVE;
}

extern "C" uint64_t flox_delta_book_replayer_copy_asks(FloxDeltaBookReplayerHandle handle,
                                                       FloxBookLevel* out, uint64_t max_entries)
{
  FLOX_CAPI_ENTER(handle);
  if (!handle || !out)
  {
    return 0;
  }
  const std::lock_guard<std::mutex> lock(g_encoder_scratch_mutex);
  auto it = g_replayer_scratch.find(handle);
  if (it == g_replayer_scratch.end())
  {
    return 0;
  }
  return copyLevelsTo(it->second.asks, out, max_entries);
  FLOX_CAPI_LEAVE;
}

// ============================================================
// Strategy run trace (.floxrun) — implementation
// ============================================================

namespace
{

inline flox::run::TraceRecorder* asRecorder(FloxRunRecorderHandle h)
{
  return static_cast<flox::run::TraceRecorder*>(h);
}

struct RunReaderState
{
  std::unique_ptr<flox::run::TraceReader> reader;
  std::vector<flox::run::OwnedSignal> signals;
  std::vector<flox::run::OwnedOrderEvent> orders;
  std::vector<flox::run::FillRecord> fills;
};

inline RunReaderState* asReader(FloxRunReaderHandle h)
{
  return static_cast<RunReaderState*>(h);
}

uint64_t copyStringTo(const std::string& src, char* out, uint64_t max_bytes)
{
  if (out == nullptr || max_bytes == 0)
  {
    return src.size();
  }
  uint64_t n = std::min(static_cast<uint64_t>(src.size()), max_bytes);
  std::memcpy(out, src.data(), n);
  return n;
}

}  // namespace

extern "C" FloxRunRecorderHandle flox_run_recorder_create(const char* path,
                                                          const char* strategy_id,
                                                          const char* strategy_hash,
                                                          int64_t run_started_ns)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  if (path == nullptr)
  {
    return nullptr;
  }
  flox::run::TraceRecorderOptions opts;
  opts.strategy_id = strategy_id ? strategy_id : "";
  opts.strategy_hash = strategy_hash ? strategy_hash : "";
  opts.run_started_ns = run_started_ns;
  try
  {
    return new flox::run::TraceRecorder(path, std::move(opts));
  }
  catch (...)
  {
    return nullptr;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_run_recorder_destroy(FloxRunRecorderHandle handle)
{
  FLOX_CAPI_ENTER_DESTROY(handle);
  delete asRecorder(handle);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_run_recorder_add_tape_ref(FloxRunRecorderHandle handle,
                                               const char* path,
                                               const char* content_hash,
                                               int64_t first_event_ns,
                                               int64_t last_event_ns)
{
  FLOX_CAPI_ENTER_VOID(handle);
  auto* rec = asRecorder(handle);
  if (rec == nullptr || path == nullptr)
  {
    return;
  }
  flox::run::TapeRef ref;
  ref.path = path;
  ref.content_hash = content_hash ? content_hash : "";
  ref.first_event_ns = first_event_ns;
  ref.last_event_ns = last_event_ns;
  rec->addTapeRef(std::move(ref));
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_run_recorder_set_run_ended_ns(FloxRunRecorderHandle handle, int64_t ns)
{
  FLOX_CAPI_ENTER_VOID(handle);
  if (auto* rec = asRecorder(handle))
  {
    rec->setRunEndedNs(ns);
  }
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_run_recorder_write_signal(FloxRunRecorderHandle handle,
                                               int64_t run_ts_ns, int64_t feed_ts_ns,
                                               uint32_t signal_id, uint32_t flags,
                                               int64_t strength_raw,
                                               const char* name, size_t name_len,
                                               const uint32_t* symbol_ids, size_t symbol_count,
                                               const uint8_t* payload, size_t payload_len)
{
  FLOX_CAPI_ENTER_VOID(handle);
  auto* rec = asRecorder(handle);
  if (rec == nullptr)
  {
    return;
  }
  flox::run::SignalView s;
  s.run_ts_ns = run_ts_ns;
  s.feed_ts_ns = feed_ts_ns;
  s.signal_id = signal_id;
  s.flags = flags;
  s.strength_raw = strength_raw;
  if (name && name_len > 0)
  {
    s.name = std::string_view(name, name_len);
  }
  if (symbol_ids && symbol_count > 0)
  {
    s.symbol_ids.assign(symbol_ids, symbol_ids + symbol_count);
  }
  if (payload && payload_len > 0)
  {
    s.payload = std::string_view(reinterpret_cast<const char*>(payload), payload_len);
  }
  rec->writeSignal(s);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_run_recorder_write_order_event(FloxRunRecorderHandle handle,
                                                    int64_t run_ts_ns, int64_t feed_ts_ns,
                                                    uint64_t order_id, uint64_t parent_signal_id,
                                                    int64_t price_raw, int64_t qty_raw,
                                                    uint32_t symbol_id, uint8_t event_kind,
                                                    uint8_t side, uint8_t order_type,
                                                    uint32_t flags,
                                                    const char* reason, size_t reason_len)
{
  FLOX_CAPI_ENTER_VOID(handle);
  auto* rec = asRecorder(handle);
  if (rec == nullptr)
  {
    return;
  }
  flox::run::OrderEventView e;
  e.run_ts_ns = run_ts_ns;
  e.feed_ts_ns = feed_ts_ns;
  e.order_id = order_id;
  e.parent_signal_id = parent_signal_id;
  e.price_raw = price_raw;
  e.qty_raw = qty_raw;
  e.symbol_id = symbol_id;
  e.event_kind = static_cast<flox::run::OrderEventKind>(event_kind);
  e.side = side;
  e.order_type = order_type;
  e.flags = flags;
  if (reason && reason_len > 0)
  {
    e.reason = std::string_view(reason, reason_len);
  }
  rec->writeOrderEvent(e);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_run_recorder_write_fill(FloxRunRecorderHandle handle,
                                             int64_t run_ts_ns, int64_t feed_ts_ns,
                                             uint64_t order_id, uint64_t fill_id,
                                             int64_t price_raw, int64_t qty_raw, int64_t fee_raw,
                                             uint32_t symbol_id, uint8_t side, uint8_t liquidity)
{
  FLOX_CAPI_ENTER_VOID(handle);
  auto* rec = asRecorder(handle);
  if (rec == nullptr)
  {
    return;
  }
  flox::run::FillView f;
  f.run_ts_ns = run_ts_ns;
  f.feed_ts_ns = feed_ts_ns;
  f.order_id = order_id;
  f.fill_id = fill_id;
  f.price_raw = price_raw;
  f.qty_raw = qty_raw;
  f.fee_raw = fee_raw;
  f.symbol_id = symbol_id;
  f.side = side;
  f.liquidity = static_cast<flox::run::FillLiquidity>(liquidity);
  rec->writeFill(f);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_run_recorder_close(FloxRunRecorderHandle handle)
{
  FLOX_CAPI_ENTER_VOID(handle);
  if (auto* rec = asRecorder(handle))
  {
    rec->close();
  }
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" FloxRunReaderHandle flox_run_reader_open(const char* path)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  if (path == nullptr)
  {
    return nullptr;
  }
  try
  {
    auto state = std::make_unique<RunReaderState>();
    state->reader = std::make_unique<flox::run::TraceReader>(path);
    state->signals = state->reader->readAllSignals();
    state->orders = state->reader->readAllOrderEvents();
    state->fills = state->reader->readAllFills();
    return state.release();
  }
  catch (...)
  {
    return nullptr;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_run_reader_close(FloxRunReaderHandle handle)
{
  FLOX_CAPI_ENTER_VOID(handle);
  delete asReader(handle);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" uint64_t flox_run_reader_strategy_id(FloxRunReaderHandle handle, char* out, uint64_t max_bytes)
{
  FLOX_CAPI_ENTER(handle);
  auto* state = asReader(handle);
  if (state == nullptr)
  {
    return 0;
  }
  return copyStringTo(state->reader->manifest().strategy_id, out, max_bytes);
  FLOX_CAPI_LEAVE;
}

extern "C" uint64_t flox_run_reader_strategy_hash(FloxRunReaderHandle handle, char* out, uint64_t max_bytes)
{
  FLOX_CAPI_ENTER(handle);
  auto* state = asReader(handle);
  if (state == nullptr)
  {
    return 0;
  }
  return copyStringTo(state->reader->manifest().strategy_hash, out, max_bytes);
  FLOX_CAPI_LEAVE;
}

extern "C" int64_t flox_run_reader_run_started_ns(FloxRunReaderHandle handle)
{
  FLOX_CAPI_ENTER(handle);
  auto* state = asReader(handle);
  return state ? state->reader->manifest().run_started_ns : 0;
  FLOX_CAPI_LEAVE;
}

extern "C" int64_t flox_run_reader_run_ended_ns(FloxRunReaderHandle handle)
{
  FLOX_CAPI_ENTER(handle);
  auto* state = asReader(handle);
  return state ? state->reader->manifest().run_ended_ns : 0;
  FLOX_CAPI_LEAVE;
}

extern "C" uint64_t flox_run_reader_tape_ref_count(FloxRunReaderHandle handle)
{
  FLOX_CAPI_ENTER(handle);
  auto* state = asReader(handle);
  return state ? state->reader->manifest().tape_refs.size() : 0;
  FLOX_CAPI_LEAVE;
}

extern "C" uint64_t flox_run_reader_tape_ref_path(FloxRunReaderHandle handle, uint64_t index, char* out, uint64_t max_bytes)
{
  FLOX_CAPI_ENTER(handle);
  auto* state = asReader(handle);
  if (state == nullptr)
  {
    return 0;
  }
  const auto& refs = state->reader->manifest().tape_refs;
  if (index >= refs.size())
  {
    return 0;
  }
  return copyStringTo(refs[index].path, out, max_bytes);
  FLOX_CAPI_LEAVE;
}

extern "C" uint64_t flox_run_reader_signal_count(FloxRunReaderHandle handle)
{
  FLOX_CAPI_ENTER(handle);
  auto* state = asReader(handle);
  return state ? state->signals.size() : 0;
  FLOX_CAPI_LEAVE;
}

extern "C" uint64_t flox_run_reader_order_event_count(FloxRunReaderHandle handle)
{
  FLOX_CAPI_ENTER(handle);
  auto* state = asReader(handle);
  return state ? state->orders.size() : 0;
  FLOX_CAPI_LEAVE;
}

extern "C" uint64_t flox_run_reader_fill_count(FloxRunReaderHandle handle)
{
  FLOX_CAPI_ENTER(handle);
  auto* state = asReader(handle);
  return state ? state->fills.size() : 0;
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_run_reader_signal_header(FloxRunReaderHandle handle, uint64_t index,
                                              int64_t* out_run_ts, int64_t* out_feed_ts,
                                              uint32_t* out_signal_id, uint32_t* out_flags,
                                              int64_t* out_strength_raw,
                                              uint64_t* out_name_len, uint64_t* out_symbol_count,
                                              uint64_t* out_payload_len)
{
  FLOX_CAPI_ENTER_VOID(handle);
  auto* state = asReader(handle);
  if (state == nullptr || index >= state->signals.size())
  {
    return;
  }
  const auto& s = state->signals[index];
  if (out_run_ts)
  {
    *out_run_ts = s.run_ts_ns;
  }
  if (out_feed_ts)
  {
    *out_feed_ts = s.feed_ts_ns;
  }
  if (out_signal_id)
  {
    *out_signal_id = s.signal_id;
  }
  if (out_flags)
  {
    *out_flags = s.flags;
  }
  if (out_strength_raw)
  {
    *out_strength_raw = s.strength_raw;
  }
  if (out_name_len)
  {
    *out_name_len = s.name.size();
  }
  if (out_symbol_count)
  {
    *out_symbol_count = s.symbol_ids.size();
  }
  if (out_payload_len)
  {
    *out_payload_len = s.payload.size();
  }
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" uint64_t flox_run_reader_signal_name(FloxRunReaderHandle handle, uint64_t index, char* out, uint64_t max_bytes)
{
  FLOX_CAPI_ENTER(handle);
  auto* state = asReader(handle);
  if (state == nullptr || index >= state->signals.size())
  {
    return 0;
  }
  return copyStringTo(state->signals[index].name, out, max_bytes);
  FLOX_CAPI_LEAVE;
}

extern "C" uint64_t flox_run_reader_signal_symbol_ids(FloxRunReaderHandle handle, uint64_t index, uint32_t* out, uint64_t max_entries)
{
  FLOX_CAPI_ENTER(handle);
  auto* state = asReader(handle);
  if (state == nullptr || index >= state->signals.size())
  {
    return 0;
  }
  const auto& ids = state->signals[index].symbol_ids;
  if (out == nullptr || max_entries == 0)
  {
    return ids.size();
  }
  uint64_t n = std::min(static_cast<uint64_t>(ids.size()), max_entries);
  for (uint64_t i = 0; i < n; ++i)
  {
    out[i] = ids[i];
  }
  return n;
  FLOX_CAPI_LEAVE;
}

extern "C" uint64_t flox_run_reader_signal_payload(FloxRunReaderHandle handle, uint64_t index, uint8_t* out, uint64_t max_bytes)
{
  FLOX_CAPI_ENTER(handle);
  auto* state = asReader(handle);
  if (state == nullptr || index >= state->signals.size())
  {
    return 0;
  }
  const auto& p = state->signals[index].payload;
  if (out == nullptr || max_bytes == 0)
  {
    return p.size();
  }
  uint64_t n = std::min(static_cast<uint64_t>(p.size()), max_bytes);
  std::memcpy(out, p.data(), n);
  return n;
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_run_reader_order_event_header(FloxRunReaderHandle handle, uint64_t index,
                                                   int64_t* out_run_ts, int64_t* out_feed_ts,
                                                   uint64_t* out_order_id, uint64_t* out_parent_signal_id,
                                                   int64_t* out_price_raw, int64_t* out_qty_raw,
                                                   uint32_t* out_symbol_id, uint8_t* out_event_kind,
                                                   uint8_t* out_side, uint8_t* out_order_type,
                                                   uint32_t* out_flags, uint64_t* out_reason_len)
{
  FLOX_CAPI_ENTER_VOID(handle);
  auto* state = asReader(handle);
  if (state == nullptr || index >= state->orders.size())
  {
    return;
  }
  const auto& e = state->orders[index];
  if (out_run_ts)
  {
    *out_run_ts = e.run_ts_ns;
  }
  if (out_feed_ts)
  {
    *out_feed_ts = e.feed_ts_ns;
  }
  if (out_order_id)
  {
    *out_order_id = e.order_id;
  }
  if (out_parent_signal_id)
  {
    *out_parent_signal_id = e.parent_signal_id;
  }
  if (out_price_raw)
  {
    *out_price_raw = e.price_raw;
  }
  if (out_qty_raw)
  {
    *out_qty_raw = e.qty_raw;
  }
  if (out_symbol_id)
  {
    *out_symbol_id = e.symbol_id;
  }
  if (out_event_kind)
  {
    *out_event_kind = static_cast<uint8_t>(e.event_kind);
  }
  if (out_side)
  {
    *out_side = e.side;
  }
  if (out_order_type)
  {
    *out_order_type = e.order_type;
  }
  if (out_flags)
  {
    *out_flags = e.flags;
  }
  if (out_reason_len)
  {
    *out_reason_len = e.reason.size();
  }
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" uint64_t flox_run_reader_order_event_reason(FloxRunReaderHandle handle, uint64_t index, char* out, uint64_t max_bytes)
{
  FLOX_CAPI_ENTER(handle);
  auto* state = asReader(handle);
  if (state == nullptr || index >= state->orders.size())
  {
    return 0;
  }
  return copyStringTo(state->orders[index].reason, out, max_bytes);
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_run_reader_fill(FloxRunReaderHandle handle, uint64_t index,
                                     int64_t* out_run_ts, int64_t* out_feed_ts,
                                     uint64_t* out_order_id, uint64_t* out_fill_id,
                                     int64_t* out_price_raw, int64_t* out_qty_raw, int64_t* out_fee_raw,
                                     uint32_t* out_symbol_id, uint8_t* out_side, uint8_t* out_liquidity)
{
  FLOX_CAPI_ENTER_VOID(handle);
  auto* state = asReader(handle);
  if (state == nullptr || index >= state->fills.size())
  {
    return;
  }
  const auto& f = state->fills[index];
  if (out_run_ts)
  {
    *out_run_ts = f.run_ts_ns;
  }
  if (out_feed_ts)
  {
    *out_feed_ts = f.feed_ts_ns;
  }
  if (out_order_id)
  {
    *out_order_id = f.order_id;
  }
  if (out_fill_id)
  {
    *out_fill_id = f.fill_id;
  }
  if (out_price_raw)
  {
    *out_price_raw = f.price_raw;
  }
  if (out_qty_raw)
  {
    *out_qty_raw = f.qty_raw;
  }
  if (out_fee_raw)
  {
    *out_fee_raw = f.fee_raw;
  }
  if (out_symbol_id)
  {
    *out_symbol_id = f.symbol_id;
  }
  if (out_side)
  {
    *out_side = f.side;
  }
  if (out_liquidity)
  {
    *out_liquidity = f.liquidity;
  }
  FLOX_CAPI_LEAVE_VOID;
}

namespace
{
inline flox::testing::BarDispatchRecorder* toBarDispatchRecorder(FloxBarDispatchRecorderHandle h)
{
  return static_cast<flox::testing::BarDispatchRecorder*>(h);
}
}  // namespace

extern "C" FloxBarDispatchRecorderHandle flox_bar_dispatch_recorder_create(void)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return new flox::testing::BarDispatchRecorder();
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_bar_dispatch_recorder_destroy(FloxBarDispatchRecorderHandle h)
{
  FLOX_CAPI_ENTER_DESTROY(h);
  delete toBarDispatchRecorder(h);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" uint32_t flox_bar_dispatch_recorder_add_time_seconds(FloxBarDispatchRecorderHandle h,
                                                                uint32_t seconds)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toBarDispatchRecorder(h)->addTimeIntervalSeconds(seconds));
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_bar_dispatch_recorder_on_trade(FloxBarDispatchRecorderHandle h,
                                                    uint32_t symbol, double price, double qty,
                                                    int64_t ts_ns)
{
  FLOX_CAPI_ENTER_VOID(h);
  toBarDispatchRecorder(h)->onTrade(symbol, price, qty, ts_ns);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_bar_dispatch_recorder_finalize(FloxBarDispatchRecorderHandle h)
{
  FLOX_CAPI_ENTER_VOID(h);
  toBarDispatchRecorder(h)->finalize();
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" uint32_t flox_bar_dispatch_recorder_count(FloxBarDispatchRecorderHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toBarDispatchRecorder(h)->count());
  FLOX_CAPI_LEAVE;
}

extern "C" uint8_t flox_bar_dispatch_recorder_type_at(FloxBarDispatchRecorderHandle h,
                                                      uint32_t index)
{
  FLOX_CAPI_ENTER(h);
  return toBarDispatchRecorder(h)->typeAt(index);
  FLOX_CAPI_LEAVE;
}

extern "C" uint64_t flox_bar_dispatch_recorder_param_at(FloxBarDispatchRecorderHandle h,
                                                        uint32_t index)
{
  FLOX_CAPI_ENTER(h);
  return toBarDispatchRecorder(h)->paramAt(index);
  FLOX_CAPI_LEAVE;
}

// ============================================================
// Streaming tape aggregators
// ============================================================

namespace
{

// Tagged holder. The C ABI exposes a single FloxAggregatorHandle
// (void*), but each result accessor needs to know the concrete type
// to do a typed static_cast. The `kind` tag is checked on every
// `*_read_result` entry; mismatched kind → return 0 rows so a buggy
// binding can't read garbage past the end of a smaller Row struct.
struct AggregatorHolder
{
  enum Kind : uint8_t
  {
    KIND_EVENT_TYPE_STATS,
    KIND_BIN_COUNT,
    KIND_VOLUME_BIN,
    KIND_OHLC_BIN,
    KIND_PEAK,
    KIND_QUANTILE,
    KIND_BOOK_SNAPSHOT_BIN,
  };
  Kind kind;
  std::unique_ptr<replay::IAggregator> impl;

  template <typename T>
  T* as(Kind expected)
  {
    if (kind != expected)
    {
      return nullptr;
    }
    return static_cast<T*>(impl.get());
  }
};

inline AggregatorHolder* toAgg(FloxAggregatorHandle h)
{
  return static_cast<AggregatorHolder*>(h);
}

inline replay::AggregatorEventFilter toAggFilter(FloxAggregatorEventFilter f)
{
  switch (f)
  {
    case FLOX_AGG_FILTER_TRADES:
      return replay::AggregatorEventFilter::Trades;
    case FLOX_AGG_FILTER_BOOKS_ONLY:
      return replay::AggregatorEventFilter::BooksOnly;
    case FLOX_AGG_FILTER_BOTH:
    default:
      return replay::AggregatorEventFilter::Both;
  }
}

inline std::vector<uint32_t> copySymbolFilter(const uint32_t* sf, uint32_t count)
{
  if (sf == nullptr || count == 0)
  {
    return {};
  }
  return std::vector<uint32_t>(sf, sf + count);
}

}  // namespace

extern "C" FloxAggregatorHandle flox_event_type_stats_aggregator_create(
    FloxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
    uint32_t symbol_filter_count)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  auto* holder = new AggregatorHolder{
      AggregatorHolder::KIND_EVENT_TYPE_STATS,
      std::make_unique<replay::EventTypeStatsAggregator>(
          toAggFilter(event_filter),
          copySymbolFilter(symbol_filter, symbol_filter_count))};
  return holder;
  FLOX_CAPI_LEAVE;
}

extern "C" FloxAggregatorHandle flox_bin_count_aggregator_create(
    int64_t bucket_ns, uint8_t by_side, uint8_t by_symbol,
    FloxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
    uint32_t symbol_filter_count)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  auto* holder = new AggregatorHolder{
      AggregatorHolder::KIND_BIN_COUNT,
      std::make_unique<replay::BinCountAggregator>(
          bucket_ns, by_side != 0, by_symbol != 0, toAggFilter(event_filter),
          copySymbolFilter(symbol_filter, symbol_filter_count))};
  return holder;
  FLOX_CAPI_LEAVE;
}

extern "C" FloxAggregatorHandle flox_volume_bin_aggregator_create(
    int64_t bucket_ns, uint8_t by_side, uint8_t by_symbol,
    FloxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
    uint32_t symbol_filter_count)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  auto* holder = new AggregatorHolder{
      AggregatorHolder::KIND_VOLUME_BIN,
      std::make_unique<replay::VolumeBinAggregator>(
          bucket_ns, by_side != 0, by_symbol != 0, toAggFilter(event_filter),
          copySymbolFilter(symbol_filter, symbol_filter_count))};
  return holder;
  FLOX_CAPI_LEAVE;
}

extern "C" FloxAggregatorHandle flox_ohlc_bin_aggregator_create(
    int64_t bucket_ns, uint8_t by_symbol,
    FloxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
    uint32_t symbol_filter_count)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  auto* holder = new AggregatorHolder{
      AggregatorHolder::KIND_OHLC_BIN,
      std::make_unique<replay::OHLCBinAggregator>(
          bucket_ns, by_symbol != 0, toAggFilter(event_filter),
          copySymbolFilter(symbol_filter, symbol_filter_count))};
  return holder;
  FLOX_CAPI_LEAVE;
}

extern "C" FloxAggregatorHandle flox_peak_aggregator_create(
    const int64_t* window_ns_list, uint32_t window_count, uint32_t top_n,
    uint32_t oversample_factor, FloxAggregatorEventFilter event_filter,
    const uint32_t* symbol_filter, uint32_t symbol_filter_count)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  std::vector<int64_t> windows;
  if (window_ns_list != nullptr && window_count > 0)
  {
    windows.assign(window_ns_list, window_ns_list + window_count);
  }
  // 0 → engine default (100). PeakAggregator's ctor maxes the factor
  // at 1 internally so a 0 here would collapse the heap to top_n
  // candidates only — pass 100 to preserve the spec'd default.
  const std::size_t oversample =
      oversample_factor == 0 ? std::size_t{100} : std::size_t{oversample_factor};
  auto* holder = new AggregatorHolder{
      AggregatorHolder::KIND_PEAK,
      std::make_unique<replay::PeakAggregator>(
          std::move(windows), top_n, oversample, toAggFilter(event_filter),
          copySymbolFilter(symbol_filter, symbol_filter_count))};
  return holder;
  FLOX_CAPI_LEAVE;
}

extern "C" FloxAggregatorHandle flox_quantile_aggregator_create(
    const int64_t* window_ns_list, uint32_t window_count,
    const double* quantiles, uint32_t quantile_count,
    FloxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
    uint32_t symbol_filter_count)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  std::vector<int64_t> windows;
  if (window_ns_list != nullptr && window_count > 0)
  {
    windows.assign(window_ns_list, window_ns_list + window_count);
  }
  std::vector<double> qs;
  if (quantiles != nullptr && quantile_count > 0)
  {
    qs.assign(quantiles, quantiles + quantile_count);
  }
  auto* holder = new AggregatorHolder{
      AggregatorHolder::KIND_QUANTILE,
      std::make_unique<replay::QuantileAggregator>(
          std::move(windows), std::move(qs), toAggFilter(event_filter),
          copySymbolFilter(symbol_filter, symbol_filter_count))};
  return holder;
  FLOX_CAPI_LEAVE;
}

extern "C" FloxAggregatorHandle flox_book_snapshot_bin_aggregator_create(
    int64_t bucket_ns, uint16_t levels,
    FloxAggregatorEventFilter event_filter, const uint32_t* symbol_filter,
    uint32_t symbol_filter_count)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  auto* holder = new AggregatorHolder{
      AggregatorHolder::KIND_BOOK_SNAPSHOT_BIN,
      std::make_unique<replay::BookSnapshotBinAggregator>(
          bucket_ns, levels, toAggFilter(event_filter),
          copySymbolFilter(symbol_filter, symbol_filter_count))};
  return holder;
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_aggregator_destroy(FloxAggregatorHandle h)
{
  FLOX_CAPI_ENTER_DESTROY(h);
  delete toAgg(h);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_data_reader_set_progress_callback(
    FloxDataReaderHandle reader, FloxProgressCallback cb, void* user_data,
    uint32_t interval_ms)
{
  FLOX_CAPI_ENTER_VOID(reader);
  if (reader == nullptr)
  {
    return;
  }
  auto* r = static_cast<replay::BinaryLogReader*>(reader);
  if (cb == nullptr)
  {
    r->clearProgressCallback();
    return;
  }
  std::chrono::milliseconds interval{interval_ms == 0 ? 1000u : interval_ms};
  r->setProgressCallback(
      [cb, user_data](double pct, int64_t cursor_ts_ns) -> bool
      { return cb(user_data, pct, cursor_ts_ns) != 0; },
      interval);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_data_reader_clear_progress_callback(
    FloxDataReaderHandle reader)
{
  FLOX_CAPI_ENTER_VOID(reader);
  if (reader == nullptr)
  {
    return;
  }
  static_cast<replay::BinaryLogReader*>(reader)->clearProgressCallback();
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" uint8_t flox_data_reader_run(FloxDataReaderHandle reader,
                                        FloxAggregatorHandle* aggregators,
                                        uint32_t aggregator_count,
                                        uint32_t n_threads)
{
  FLOX_CAPI_ENTER(reader);
  if (reader == nullptr)
  {
    return 0;
  }
  std::vector<replay::IAggregator*> raw;
  if (aggregators != nullptr && aggregator_count > 0)
  {
    raw.reserve(aggregator_count);
    for (uint32_t i = 0; i < aggregator_count; ++i)
    {
      auto* holder = toAgg(aggregators[i]);
      raw.push_back(holder != nullptr ? holder->impl.get() : nullptr);
    }
  }
  auto* r = static_cast<replay::BinaryLogReader*>(reader);
  const std::size_t nt = n_threads == 0 ? std::size_t{1} : std::size_t{n_threads};
  // Aggregators may refuse a configuration by throwing (e.g. the book
  // snapshot aggregator refuses parallel runs from cloneEmpty). A C++
  // exception must not cross the C ABI — map it to the failure return.
  try
  {
    return r->run(raw, nt) ? 1 : 0;
  }
  catch (const std::exception&)
  {
    return 0;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" uint8_t flox_merged_tape_reader_run(FloxMergedTapeReaderHandle reader,
                                               FloxAggregatorHandle* aggregators,
                                               uint32_t aggregator_count,
                                               uint32_t /*n_threads*/)
{
  FLOX_CAPI_ENTER(reader);
  // n_threads reserved for future; MergedTapeReader::run is single-
  // threaded for now (per-instance symbol rekey would not align
  // across worker partitions).
  if (reader == nullptr)
  {
    return 0;
  }
  std::vector<replay::IAggregator*> raw;
  if (aggregators != nullptr && aggregator_count > 0)
  {
    raw.reserve(aggregator_count);
    for (uint32_t i = 0; i < aggregator_count; ++i)
    {
      auto* holder = toAgg(aggregators[i]);
      raw.push_back(holder != nullptr ? holder->impl.get() : nullptr);
    }
  }
  auto* impl = static_cast<capi_impl::FloxMergedTapeReaderImpl*>(reader);
  if (!impl || !impl->reader)
  {
    return 0;
  }
  return impl->reader->run(raw) ? 1 : 0;
  FLOX_CAPI_LEAVE;
}

extern "C" uint32_t flox_event_type_stats_read_result(FloxAggregatorHandle h,
                                                      FloxEventTypeStatsRow* rows_out,
                                                      uint32_t max_rows)
{
  FLOX_CAPI_ENTER(h);
  auto* holder = toAgg(h);
  if (holder == nullptr)
  {
    return 0;
  }
  auto* impl = holder->as<replay::EventTypeStatsAggregator>(
      AggregatorHolder::KIND_EVENT_TYPE_STATS);
  if (impl == nullptr)
  {
    return 0;
  }
  const auto& rows = impl->result();
  if (rows_out == nullptr || max_rows == 0)
  {
    return static_cast<uint32_t>(rows.size());
  }
  const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(rows.size(), max_rows));
  for (uint32_t i = 0; i < n; ++i)
  {
    rows_out[i].symbol_id = rows[i].symbol_id;
    rows_out[i].trades = rows[i].trades;
    rows_out[i].book_snapshots = rows[i].book_snapshots;
    rows_out[i].book_deltas = rows[i].book_deltas;
  }
  return n;
  FLOX_CAPI_LEAVE;
}

extern "C" uint32_t flox_bin_count_read_result(FloxAggregatorHandle h,
                                               FloxBinCountRow* rows_out,
                                               uint32_t max_rows)
{
  FLOX_CAPI_ENTER(h);
  auto* holder = toAgg(h);
  if (holder == nullptr)
  {
    return 0;
  }
  auto* impl = holder->as<replay::BinCountAggregator>(AggregatorHolder::KIND_BIN_COUNT);
  if (impl == nullptr)
  {
    return 0;
  }
  const auto& rows = impl->result();
  if (rows_out == nullptr || max_rows == 0)
  {
    return static_cast<uint32_t>(rows.size());
  }
  const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(rows.size(), max_rows));
  for (uint32_t i = 0; i < n; ++i)
  {
    rows_out[i].bucket_ts_ns = rows[i].bucket_ts_ns;
    rows_out[i].symbol_id = rows[i].symbol_id;
    rows_out[i].side = rows[i].side;
    rows_out[i].count = rows[i].count;
  }
  return n;
  FLOX_CAPI_LEAVE;
}

extern "C" uint32_t flox_volume_bin_read_result(FloxAggregatorHandle h,
                                                FloxVolumeBinRow* rows_out,
                                                uint32_t max_rows)
{
  FLOX_CAPI_ENTER(h);
  auto* holder = toAgg(h);
  if (holder == nullptr)
  {
    return 0;
  }
  auto* impl = holder->as<replay::VolumeBinAggregator>(AggregatorHolder::KIND_VOLUME_BIN);
  if (impl == nullptr)
  {
    return 0;
  }
  const auto& rows = impl->result();
  if (rows_out == nullptr || max_rows == 0)
  {
    return static_cast<uint32_t>(rows.size());
  }
  const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(rows.size(), max_rows));
  for (uint32_t i = 0; i < n; ++i)
  {
    rows_out[i].bucket_ts_ns = rows[i].bucket_ts_ns;
    rows_out[i].symbol_id = rows[i].symbol_id;
    rows_out[i].side = rows[i].side;
    rows_out[i].qty_raw = rows[i].qty_raw;
  }
  return n;
  FLOX_CAPI_LEAVE;
}

extern "C" uint32_t flox_ohlc_bin_read_result(FloxAggregatorHandle h,
                                              FloxOHLCBinRow* rows_out,
                                              uint32_t max_rows)
{
  FLOX_CAPI_ENTER(h);
  auto* holder = toAgg(h);
  if (holder == nullptr)
  {
    return 0;
  }
  auto* impl = holder->as<replay::OHLCBinAggregator>(AggregatorHolder::KIND_OHLC_BIN);
  if (impl == nullptr)
  {
    return 0;
  }
  const auto& rows = impl->result();
  if (rows_out == nullptr || max_rows == 0)
  {
    return static_cast<uint32_t>(rows.size());
  }
  const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(rows.size(), max_rows));
  for (uint32_t i = 0; i < n; ++i)
  {
    rows_out[i].bucket_ts_ns = rows[i].bucket_ts_ns;
    rows_out[i].symbol_id = rows[i].symbol_id;
    rows_out[i].open_raw = rows[i].open_raw;
    rows_out[i].high_raw = rows[i].high_raw;
    rows_out[i].low_raw = rows[i].low_raw;
    rows_out[i].close_raw = rows[i].close_raw;
  }
  return n;
  FLOX_CAPI_LEAVE;
}

extern "C" uint32_t flox_peak_read_result(FloxAggregatorHandle h,
                                          FloxPeakRow* rows_out, uint32_t max_rows)
{
  FLOX_CAPI_ENTER(h);
  auto* holder = toAgg(h);
  if (holder == nullptr)
  {
    return 0;
  }
  auto* impl = holder->as<replay::PeakAggregator>(AggregatorHolder::KIND_PEAK);
  if (impl == nullptr)
  {
    return 0;
  }
  const auto& rows = impl->result();
  if (rows_out == nullptr || max_rows == 0)
  {
    return static_cast<uint32_t>(rows.size());
  }
  const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(rows.size(), max_rows));
  for (uint32_t i = 0; i < n; ++i)
  {
    rows_out[i].window_ns = rows[i].window_ns;
    rows_out[i].count = rows[i].count;
    rows_out[i].start_ns = rows[i].start_ns;
  }
  return n;
  FLOX_CAPI_LEAVE;
}

extern "C" uint32_t flox_quantile_read_result(FloxAggregatorHandle h,
                                              FloxQuantileRow* rows_out,
                                              uint32_t max_rows)
{
  FLOX_CAPI_ENTER(h);
  auto* holder = toAgg(h);
  if (holder == nullptr)
  {
    return 0;
  }
  auto* impl = holder->as<replay::QuantileAggregator>(AggregatorHolder::KIND_QUANTILE);
  if (impl == nullptr)
  {
    return 0;
  }
  const auto& rows = impl->result();
  if (rows_out == nullptr || max_rows == 0)
  {
    return static_cast<uint32_t>(rows.size());
  }
  const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(rows.size(), max_rows));
  for (uint32_t i = 0; i < n; ++i)
  {
    rows_out[i].window_ns = rows[i].window_ns;
    rows_out[i].quantile = rows[i].quantile;
    rows_out[i].count = rows[i].count;
  }
  return n;
  FLOX_CAPI_LEAVE;
}

extern "C" uint32_t flox_book_snapshot_bin_read_result(
    FloxAggregatorHandle h, FloxBookSnapshotBinRow* rows_out, uint32_t max_rows)
{
  FLOX_CAPI_ENTER(h);
  auto* holder = toAgg(h);
  if (holder == nullptr)
  {
    return 0;
  }
  auto* impl = holder->as<replay::BookSnapshotBinAggregator>(
      AggregatorHolder::KIND_BOOK_SNAPSHOT_BIN);
  if (impl == nullptr)
  {
    return 0;
  }
  const auto& rows = impl->result();
  if (rows_out == nullptr || max_rows == 0)
  {
    return static_cast<uint32_t>(rows.size());
  }
  const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(rows.size(), max_rows));
  for (uint32_t i = 0; i < n; ++i)
  {
    rows_out[i].bucket_ts_ns = rows[i].bucket_ts_ns;
    rows_out[i].symbol_id = rows[i].symbol_id;
    rows_out[i].level = rows[i].level;
    rows_out[i].flags = rows[i].flags;
    rows_out[i].bid_price_raw = rows[i].bid_price_raw;
    rows_out[i].bid_qty_raw = rows[i].bid_qty_raw;
    rows_out[i].ask_price_raw = rows[i].ask_price_raw;
    rows_out[i].ask_qty_raw = rows[i].ask_qty_raw;
  }
  return n;
  FLOX_CAPI_LEAVE;
}

// ============================================================
// Live queue position estimator
// ============================================================

#include "flox/backtest/account.h"
#include "flox/backtest/fee_schedule.h"
#include "flox/backtest/funding_schedule.h"
#include "flox/backtest/liquidation_engine.h"
#include "flox/execution/live_queue_position_estimator.h"

namespace
{
inline flox::LiveQueuePositionEstimator* toLiveQ(FloxLiveQueuePositionHandle h)
{
  return static_cast<flox::LiveQueuePositionEstimator*>(h);
}
inline flox::FundingSchedule* toFunding(FloxFundingScheduleHandle h)
{
  return static_cast<flox::FundingSchedule*>(h);
}
inline flox::FeeSchedule* toFee(FloxFeeScheduleHandle h)
{
  return static_cast<flox::FeeSchedule*>(h);
}
inline flox::Account* toAccount(FloxAccountHandle h)
{
  return static_cast<flox::Account*>(h);
}
}  // namespace

extern "C" FloxFeeScheduleHandle flox_fee_schedule_create(void)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return new flox::FeeSchedule();
  FLOX_CAPI_LEAVE;
}
extern "C" void flox_fee_schedule_destroy(FloxFeeScheduleHandle h)
{
  FLOX_CAPI_ENTER_DESTROY(h);
  delete toFee(h);
  FLOX_CAPI_LEAVE_VOID;
}
extern "C" void flox_fee_schedule_add_tier(FloxFeeScheduleHandle h,
                                           double min_notional_30d, double maker_bps,
                                           double taker_bps)
{
  FLOX_CAPI_ENTER_VOID(h);
  toFee(h)->addTier(min_notional_30d, maker_bps, taker_bps);
  FLOX_CAPI_LEAVE_VOID;
}
extern "C" int flox_fee_schedule_load_profile(FloxFeeScheduleHandle h, const char* name)
{
  FLOX_CAPI_ENTER(h);
  if (!h || !name)
  {
    return 0;
  }
  std::string n = name;
  if (n == "binance_um_futures")
  {
    *toFee(h) = flox::FeeSchedule::binance_um_futures();
  }
  else if (n == "bybit_linear")
  {
    *toFee(h) = flox::FeeSchedule::bybit_linear();
  }
  else if (n == "okx_swap")
  {
    *toFee(h) = flox::FeeSchedule::okx_swap();
  }
  else if (n == "deribit")
  {
    *toFee(h) = flox::FeeSchedule::deribit();
  }
  else
  {
    return 0;
  }
  return 1;
  FLOX_CAPI_LEAVE;
}
extern "C" void flox_fee_schedule_record_fill(FloxFeeScheduleHandle h, int64_t ts_ns,
                                              double notional)
{
  FLOX_CAPI_ENTER_VOID(h);
  toFee(h)->recordFill(ts_ns, notional);
  FLOX_CAPI_LEAVE_VOID;
}
extern "C" double flox_fee_schedule_fee_for(FloxFeeScheduleHandle h, int64_t ts_ns,
                                            double notional, uint8_t is_maker)
{
  FLOX_CAPI_ENTER(h);
  return toFee(h)->feeFor(ts_ns, notional, is_maker != 0);
  FLOX_CAPI_LEAVE;
}
extern "C" uint32_t flox_fee_schedule_current_tier(FloxFeeScheduleHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toFee(h)->currentTierIndex());
  FLOX_CAPI_LEAVE;
}
extern "C" double flox_fee_schedule_rolling_notional(FloxFeeScheduleHandle h)
{
  FLOX_CAPI_ENTER(h);
  return toFee(h)->rollingNotional30d();
  FLOX_CAPI_LEAVE;
}
extern "C" uint32_t flox_fee_schedule_tier_transitions(FloxFeeScheduleHandle h,
                                                       int64_t* out_buf,
                                                       uint32_t max_events)
{
  FLOX_CAPI_ENTER(h);
  const auto& v = toFee(h)->tierTransitionTsNs();
  if (out_buf == nullptr || max_events == 0)
  {
    return static_cast<uint32_t>(v.size());
  }
  uint32_t n = std::min<uint32_t>(max_events, static_cast<uint32_t>(v.size()));
  for (uint32_t i = 0; i < n; ++i)
  {
    out_buf[i] = v[i];
  }
  return n;
  FLOX_CAPI_LEAVE;
}
extern "C" void flox_fee_schedule_reset_rolling(FloxFeeScheduleHandle h)
{
  FLOX_CAPI_ENTER_VOID(h);
  toFee(h)->resetRolling();
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" FloxFundingScheduleHandle flox_funding_schedule_create(void)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return new flox::FundingSchedule();
  FLOX_CAPI_LEAVE;
}
extern "C" void flox_funding_schedule_destroy(FloxFundingScheduleHandle h)
{
  FLOX_CAPI_ENTER_DESTROY(h);
  delete toFunding(h);
  FLOX_CAPI_LEAVE_VOID;
}
extern "C" void flox_funding_schedule_set_constant(FloxFundingScheduleHandle h,
                                                   int64_t interval_ns, double rate)
{
  FLOX_CAPI_ENTER_VOID(h);
  *toFunding(h) = flox::FundingSchedule::constant(interval_ns, rate);
  FLOX_CAPI_LEAVE_VOID;
}
extern "C" void flox_funding_schedule_set_tape(FloxFundingScheduleHandle h,
                                               const int64_t* timestamps_ns,
                                               const double* rates, uint32_t n)
{
  FLOX_CAPI_ENTER_VOID(h);
  std::vector<std::pair<int64_t, double>> events;
  events.reserve(n);
  for (uint32_t i = 0; i < n; ++i)
  {
    events.emplace_back(timestamps_ns ? timestamps_ns[i] : 0, rates ? rates[i] : 0.0);
  }
  *toFunding(h) = flox::FundingSchedule::tape(std::move(events));
  FLOX_CAPI_LEAVE_VOID;
}
extern "C" void flox_funding_schedule_set_tape_by_symbol(
    FloxFundingScheduleHandle h, const int64_t* timestamps_ns,
    const uint32_t* symbols, const double* rates, uint32_t n)
{
  FLOX_CAPI_ENTER_VOID(h);
  std::vector<flox::FundingTapeEntry> entries;
  entries.reserve(n);
  for (uint32_t i = 0; i < n; ++i)
  {
    flox::FundingTapeEntry e;
    e.timestampNs = UnixNanos::fromRaw(timestamps_ns ? timestamps_ns[i] : 0);
    e.symbol = symbols ? static_cast<flox::SymbolId>(symbols[i])
                       : flox::FundingTapeEntry::kAnySymbol;
    e.rate = rates ? rates[i] : 0.0;
    entries.push_back(e);
  }
  *toFunding(h) = flox::FundingSchedule::tapeBySymbol(std::move(entries));
  FLOX_CAPI_LEAVE_VOID;
}
extern "C" uint8_t flox_funding_schedule_load_tape(FloxFundingScheduleHandle h,
                                                   const char* path)
{
  FLOX_CAPI_ENTER(h);
  if (!h || !path)
  {
    return 0;
  }
  return toFunding(h)->loadTape(std::string(path)) ? 1 : 0;
  FLOX_CAPI_LEAVE;
}
extern "C" int flox_funding_schedule_load_profile(FloxFundingScheduleHandle h,
                                                  const char* name)
{
  FLOX_CAPI_ENTER(h);
  if (!h || !name)
  {
    return 0;
  }
  std::string n = name;
  if (n == "binance_um_futures")
  {
    *toFunding(h) = flox::FundingSchedule::binance_um_futures();
  }
  else if (n == "bybit_linear")
  {
    *toFunding(h) = flox::FundingSchedule::bybit_linear();
  }
  else if (n == "okx_swap")
  {
    *toFunding(h) = flox::FundingSchedule::okx_swap();
  }
  else if (n == "bitget_hourly")
  {
    *toFunding(h) = flox::FundingSchedule::bitget_hourly();
  }
  else
  {
    return 0;
  }
  return 1;
  FLOX_CAPI_LEAVE;
}
extern "C" void flox_funding_schedule_set_constant_rate(FloxFundingScheduleHandle h,
                                                        double rate)
{
  FLOX_CAPI_ENTER_VOID(h);
  toFunding(h)->setConstantRate(rate);
  FLOX_CAPI_LEAVE_VOID;
}
extern "C" void flox_funding_schedule_reset(FloxFundingScheduleHandle h)
{
  FLOX_CAPI_ENTER_VOID(h);
  toFunding(h)->reset();
  FLOX_CAPI_LEAVE_VOID;
}
extern "C" uint32_t flox_funding_schedule_tick(FloxFundingScheduleHandle h, int64_t now_ns,
                                               const uint32_t* symbols,
                                               const double* positions,
                                               const double* mark_prices,
                                               uint32_t n_symbols, double* out_buf,
                                               uint32_t max_events)
{
  FLOX_CAPI_ENTER(h);
  std::vector<flox::SymbolId> syms(n_symbols);
  std::vector<double> pos(n_symbols);
  std::vector<double> mark(n_symbols);
  for (uint32_t i = 0; i < n_symbols; ++i)
  {
    syms[i] = symbols ? symbols[i] : 0;
    pos[i] = positions ? positions[i] : 0.0;
    mark[i] = mark_prices ? mark_prices[i] : 0.0;
  }
  auto events = toFunding(h)->tick(now_ns, syms, pos, mark);
  if (out_buf == nullptr || max_events == 0)
  {
    return static_cast<uint32_t>(events.size());
  }
  uint32_t n = std::min<uint32_t>(max_events, static_cast<uint32_t>(events.size()));
  for (uint32_t i = 0; i < n; ++i)
  {
    out_buf[i * 6 + 0] = static_cast<double>(events[i].timestampNs.raw());
    out_buf[i * 6 + 1] = static_cast<double>(events[i].symbol);
    out_buf[i * 6 + 2] = events[i].rate;
    out_buf[i * 6 + 3] = events[i].markPrice;
    out_buf[i * 6 + 4] = events[i].positionSigned;
    out_buf[i * 6 + 5] = events[i].amount;
  }
  return n;
  FLOX_CAPI_LEAVE;
}

extern "C" FloxLiveQueuePositionHandle flox_live_queue_position_create(void)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return new flox::LiveQueuePositionEstimator();
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_live_queue_position_destroy(FloxLiveQueuePositionHandle h)
{
  FLOX_CAPI_ENTER_DESTROY(h);
  delete toLiveQ(h);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_live_queue_position_set_confidence_half_life_ns(
    FloxLiveQueuePositionHandle h, int64_t half_life_ns)
{
  FLOX_CAPI_ENTER_VOID(h);
  toLiveQ(h)->setConfidenceHalfLifeNs(half_life_ns);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_live_queue_position_set_shrink_factor(
    FloxLiveQueuePositionHandle h, double factor)
{
  FLOX_CAPI_ENTER_VOID(h);
  toLiveQ(h)->setShrinkAttributionFactor(factor);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_live_queue_position_on_order_placed(
    FloxLiveQueuePositionHandle h, uint32_t symbol, uint8_t side, int64_t price_raw,
    uint64_t order_id, int64_t order_qty_raw, int64_t level_qty_raw, int64_t ts_ns)
{
  FLOX_CAPI_ENTER_VOID(h);
  toLiveQ(h)->onOrderPlaced(symbol, static_cast<flox::Side>(side),
                            flox::Price::fromRaw(price_raw), order_id,
                            flox::Quantity::fromRaw(order_qty_raw),
                            flox::Quantity::fromRaw(level_qty_raw), ts_ns);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_live_queue_position_on_order_cancelled(
    FloxLiveQueuePositionHandle h, uint64_t order_id, int64_t ts_ns)
{
  FLOX_CAPI_ENTER_VOID(h);
  toLiveQ(h)->onOrderCancelled(order_id, ts_ns);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_live_queue_position_on_order_filled(
    FloxLiveQueuePositionHandle h, uint64_t order_id, int64_t cumulative_fill_raw,
    int64_t ts_ns)
{
  FLOX_CAPI_ENTER_VOID(h);
  toLiveQ(h)->onOrderFilled(order_id, flox::Quantity::fromRaw(cumulative_fill_raw), ts_ns);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_live_queue_position_on_trade(FloxLiveQueuePositionHandle h,
                                                  uint32_t symbol, int64_t price_raw,
                                                  int64_t qty_raw, int64_t ts_ns)
{
  FLOX_CAPI_ENTER_VOID(h);
  toLiveQ(h)->onTrade(symbol, flox::Price::fromRaw(price_raw),
                      flox::Quantity::fromRaw(qty_raw), ts_ns);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_live_queue_position_on_trade_with_flag(
    FloxLiveQueuePositionHandle h, uint32_t symbol, int64_t price_raw, int64_t qty_raw,
    int64_t ts_ns, uint8_t is_hidden)
{
  FLOX_CAPI_ENTER_VOID(h);
  toLiveQ(h)->onTradeWithFlag(symbol, flox::Price::fromRaw(price_raw),
                              flox::Quantity::fromRaw(qty_raw), ts_ns, is_hidden != 0);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_live_queue_position_set_hidden_order_policy(
    FloxLiveQueuePositionHandle h, uint8_t policy)
{
  FLOX_CAPI_ENTER_VOID(h);
  toLiveQ(h)->setHiddenOrderPolicy(static_cast<flox::HiddenOrderPolicy>(policy));
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_live_queue_position_on_level_update(
    FloxLiveQueuePositionHandle h, uint32_t symbol, uint8_t side, int64_t price_raw,
    int64_t new_qty_raw, int64_t ts_ns)
{
  FLOX_CAPI_ENTER_VOID(h);
  toLiveQ(h)->onLevelUpdate(symbol, static_cast<flox::Side>(side),
                            flox::Price::fromRaw(price_raw),
                            flox::Quantity::fromRaw(new_qty_raw), ts_ns);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" uint8_t flox_live_queue_position_snapshot(FloxLiveQueuePositionHandle h,
                                                     uint64_t order_id, int64_t now_ns,
                                                     int64_t* out_slots)
{
  FLOX_CAPI_ENTER(h);
  auto snap = toLiveQ(h)->snapshot(order_id, now_ns);
  if (!snap.has_value() || out_slots == nullptr)
  {
    return 0;
  }
  out_slots[0] = static_cast<int64_t>(snap->orderId);
  out_slots[1] = snap->queueAheadEst.raw();
  out_slots[2] = snap->total.raw();
  out_slots[3] = snap->lastUpdateNs;
  double conf = snap->confidence;
  std::memcpy(&out_slots[4], &conf, sizeof(double));
  out_slots[5] = snap->hiddenVolumeSeen.raw();
  return 1;
  FLOX_CAPI_LEAVE;
}

extern "C" uint32_t flox_live_queue_position_tracked_count(FloxLiveQueuePositionHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toLiveQ(h)->trackedOrderCount());
  FLOX_CAPI_LEAVE;
}

// ============================================================
// Liquidation engine
// ============================================================

namespace
{
LiquidationEngine* toLiqEngine(FloxLiquidationEngineHandle h)
{
  return static_cast<LiquidationEngine*>(h);
}
}  // namespace

extern "C" FloxLiquidationEngineHandle flox_liquidation_engine_create(void)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return new LiquidationEngine();
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_liquidation_engine_destroy(FloxLiquidationEngineHandle h)
{
  FLOX_CAPI_ENTER_DESTROY(h);
  delete toLiqEngine(h);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_liquidation_engine_add_tier(FloxLiquidationEngineHandle h,
                                                 double min_notional, double mm_fraction)
{
  FLOX_CAPI_ENTER_VOID(h);
  toLiqEngine(h)->addTier(min_notional, mm_fraction);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_liquidation_engine_set_insurance_fund_capital(
    FloxLiquidationEngineHandle h, double capital)
{
  FLOX_CAPI_ENTER_VOID(h);
  toLiqEngine(h)->setInsuranceFundCapital(capital);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" double flox_liquidation_engine_insurance_fund_balance(FloxLiquidationEngineHandle h)
{
  FLOX_CAPI_ENTER(h);
  return toLiqEngine(h)->insuranceFundBalance();
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_liquidation_engine_set_adl_enabled(FloxLiquidationEngineHandle h,
                                                        uint8_t enabled)
{
  FLOX_CAPI_ENTER_VOID(h);
  toLiqEngine(h)->setAdlEnabled(enabled != 0);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_liquidation_engine_set_adl_ranking(FloxLiquidationEngineHandle h,
                                                        uint8_t ranking)
{
  FLOX_CAPI_ENTER_VOID(h);
  AdlRanking r = AdlRanking::PnlRatio;
  switch (ranking)
  {
    case 1:
      r = AdlRanking::Binance;
      break;
    case 2:
      r = AdlRanking::Bybit;
      break;
    case 3:
      r = AdlRanking::PositionSize;
      break;
    default:
      r = AdlRanking::PnlRatio;
      break;
  }
  toLiqEngine(h)->setAdlRanking(r);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" uint8_t flox_liquidation_engine_adl_ranking(FloxLiquidationEngineHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint8_t>(toLiqEngine(h)->adlRanking());
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_liquidation_engine_set_liquidation_slippage_bps(
    FloxLiquidationEngineHandle h, double bps)
{
  FLOX_CAPI_ENTER_VOID(h);
  toLiqEngine(h)->setLiquidationSlippageBps(bps);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_liquidation_engine_open_position(FloxLiquidationEngineHandle h,
                                                      uint64_t account_id, uint32_t symbol,
                                                      double quantity, double entry_price,
                                                      double equity)
{
  FLOX_CAPI_ENTER_VOID(h);
  toLiqEngine(h)->openPosition(LeveragedPosition{.accountId = account_id,
                                                 .symbol = symbol,
                                                 .quantity = Quantity::fromDouble(quantity),
                                                 .entryPrice = Price::fromDouble(entry_price),
                                                 .equity = Volume::fromDouble(equity)});
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_liquidation_engine_close_position(FloxLiquidationEngineHandle h,
                                                       uint64_t account_id, uint32_t symbol)
{
  FLOX_CAPI_ENTER_VOID(h);
  toLiqEngine(h)->closePosition(account_id, symbol);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" uint32_t flox_liquidation_engine_on_mark(FloxLiquidationEngineHandle h,
                                                    uint32_t symbol, double mark_price)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toLiqEngine(h)->onMark(symbol, mark_price).liquidationsCount);
  FLOX_CAPI_LEAVE;
}

extern "C" uint32_t flox_liquidation_engine_on_marks(FloxLiquidationEngineHandle h,
                                                     uint32_t n,
                                                     const uint32_t* symbols,
                                                     const double* prices,
                                                     int64_t ts_ns)
{
  FLOX_CAPI_ENTER(h);
  std::vector<std::pair<flox::SymbolId, double>> marks;
  marks.reserve(n);
  for (uint32_t i = 0; i < n; ++i)
  {
    marks.emplace_back(symbols[i], prices[i]);
  }
  return static_cast<uint32_t>(
      toLiqEngine(h)->onMarks(marks, ts_ns).liquidationsCount);
  FLOX_CAPI_LEAVE;
}

extern "C" uint64_t flox_liquidation_engine_liquidations_count(FloxLiquidationEngineHandle h)
{
  FLOX_CAPI_ENTER(h);
  return toLiqEngine(h)->liquidationsCount();
  FLOX_CAPI_LEAVE;
}
extern "C" uint64_t flox_liquidation_engine_insurance_payments_count(
    FloxLiquidationEngineHandle h)
{
  FLOX_CAPI_ENTER(h);
  return toLiqEngine(h)->insurancePaymentsCount();
  FLOX_CAPI_LEAVE;
}
extern "C" uint64_t flox_liquidation_engine_adl_closeouts_count(FloxLiquidationEngineHandle h)
{
  FLOX_CAPI_ENTER(h);
  return toLiqEngine(h)->adlCloseoutsCount();
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_liquidation_engine_load_profile(FloxLiquidationEngineHandle h,
                                                     uint8_t profile)
{
  FLOX_CAPI_ENTER_VOID(h);
  auto* eng = toLiqEngine(h);
  LiquidationEngine canned;
  switch (profile)
  {
    case 0:
      canned = LiquidationEngine::binance_um_futures();
      break;
    case 1:
      canned = LiquidationEngine::bybit_linear();
      break;
    case 2:
      canned = LiquidationEngine::okx_swap();
      break;
    default:
      return;
  }
  *eng = canned;
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_liquidation_engine_set_executor(FloxLiquidationEngineHandle h,
                                                     FloxSimulatedExecutorHandle exec_h)
{
  FLOX_CAPI_ENTER_VOID(h);
  SimulatedExecutor* ex = nullptr;
  if (exec_h != nullptr)
  {
    ex = &static_cast<FloxSimulatedExecutorImpl*>(exec_h)->executor;
  }
  toLiqEngine(h)->setExecutor(ex);
  FLOX_CAPI_LEAVE_VOID;
}

// T039: cascade statistics.
namespace
{
template <typename T>
uint32_t copyVec(const std::vector<T>& src, T* out, uint32_t max)
{
  if (out == nullptr || max == 0)
  {
    return 0;
  }
  const uint32_t n = std::min<uint32_t>(max, static_cast<uint32_t>(src.size()));
  std::copy(src.begin(), src.begin() + n, out);
  return n;
}
}  // namespace

extern "C" uint32_t flox_liquidation_engine_deficits_paid_by_fund_size(
    FloxLiquidationEngineHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toLiqEngine(h)->deficitsPaidByFund().size());
  FLOX_CAPI_LEAVE;
}
extern "C" uint32_t flox_liquidation_engine_deficits_paid_by_fund_copy(
    FloxLiquidationEngineHandle h, double* out, uint32_t max)
{
  FLOX_CAPI_ENTER(h);
  return copyVec(toLiqEngine(h)->deficitsPaidByFund(), out, max);
  FLOX_CAPI_LEAVE;
}
extern "C" uint32_t flox_liquidation_engine_deficits_paid_by_adl_size(
    FloxLiquidationEngineHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toLiqEngine(h)->deficitsPaidByAdl().size());
  FLOX_CAPI_LEAVE;
}
extern "C" uint32_t flox_liquidation_engine_deficits_paid_by_adl_copy(
    FloxLiquidationEngineHandle h, double* out, uint32_t max)
{
  FLOX_CAPI_ENTER(h);
  return copyVec(toLiqEngine(h)->deficitsPaidByAdl(), out, max);
  FLOX_CAPI_LEAVE;
}
extern "C" uint32_t flox_liquidation_engine_cascade_sizes_size(
    FloxLiquidationEngineHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toLiqEngine(h)->cascadeSizesPerTick().size());
  FLOX_CAPI_LEAVE;
}
extern "C" uint32_t flox_liquidation_engine_cascade_sizes_copy(
    FloxLiquidationEngineHandle h, uint32_t* out, uint32_t max)
{
  FLOX_CAPI_ENTER(h);
  return copyVec(toLiqEngine(h)->cascadeSizesPerTick(), out, max);
  FLOX_CAPI_LEAVE;
}
extern "C" uint32_t flox_liquidation_engine_fund_balance_history_size(
    FloxLiquidationEngineHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toLiqEngine(h)->fundBalanceHistory().size());
  FLOX_CAPI_LEAVE;
}
extern "C" uint32_t flox_liquidation_engine_fund_balance_history_copy(
    FloxLiquidationEngineHandle h, double* out, uint32_t max)
{
  FLOX_CAPI_ENTER(h);
  return copyVec(toLiqEngine(h)->fundBalanceHistory(), out, max);
  FLOX_CAPI_LEAVE;
}
extern "C" uint64_t flox_liquidation_engine_ticks_to_first_adl(
    FloxLiquidationEngineHandle h)
{
  FLOX_CAPI_ENTER(h);
  return toLiqEngine(h)->ticksToFirstAdl();
  FLOX_CAPI_LEAVE;
}
extern "C" void flox_liquidation_engine_reset_stats(FloxLiquidationEngineHandle h)
{
  FLOX_CAPI_ENTER_VOID(h);
  toLiqEngine(h)->resetStats();
  FLOX_CAPI_LEAVE_VOID;
}
extern "C" void flox_liquidation_engine_set_mark_impact_model(
    FloxLiquidationEngineHandle h, uint8_t model, double weight)
{
  FLOX_CAPI_ENTER_VOID(h);
  using M = flox::LiquidationEngine::MarkImpactModel;
  M m = M::None;
  if (model == 1)
  {
    m = M::BookAnchored;
  }
  else if (model == 2)
  {
    m = M::BookOnly;
  }
  toLiqEngine(h)->setMarkImpactModel(m, weight);
  FLOX_CAPI_LEAVE_VOID;
}
extern "C" uint8_t flox_liquidation_engine_mark_impact_model(
    FloxLiquidationEngineHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint8_t>(toLiqEngine(h)->markImpactModel());
  FLOX_CAPI_LEAVE;
}
extern "C" double flox_liquidation_engine_mark_impact_weight(
    FloxLiquidationEngineHandle h)
{
  FLOX_CAPI_ENTER(h);
  return toLiqEngine(h)->markImpactWeight();
  FLOX_CAPI_LEAVE;
}
extern "C" void flox_liquidation_engine_set_max_cascade_depth(
    FloxLiquidationEngineHandle h, uint32_t depth)
{
  FLOX_CAPI_ENTER_VOID(h);
  toLiqEngine(h)->setMaxCascadeDepth(depth);
  FLOX_CAPI_LEAVE_VOID;
}
extern "C" uint32_t flox_liquidation_engine_max_cascade_depth(
    FloxLiquidationEngineHandle h)
{
  FLOX_CAPI_ENTER(h);
  return toLiqEngine(h)->maxCascadeDepth();
  FLOX_CAPI_LEAVE;
}

// ============================================================
// T037: Account
// ============================================================

extern "C" FloxAccountHandle flox_account_create(uint64_t account_id, double equity)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return new flox::Account(account_id, equity);
  FLOX_CAPI_LEAVE;
}
extern "C" void flox_account_destroy(FloxAccountHandle h)
{
  FLOX_CAPI_ENTER_DESTROY(h);
  delete toAccount(h);
  FLOX_CAPI_LEAVE_VOID;
}
extern "C" uint64_t flox_account_id(FloxAccountHandle h)
{
  FLOX_CAPI_ENTER(h);
  return toAccount(h)->accountId();
  FLOX_CAPI_LEAVE;
}
extern "C" double flox_account_equity(FloxAccountHandle h)
{
  FLOX_CAPI_ENTER(h);
  // The C ABI is double-facing; Account keeps the value in fixed point.
  return toAccount(h)->equity().toDouble();
  FLOX_CAPI_LEAVE;
}
extern "C" void flox_account_set_equity(FloxAccountHandle h, double equity)
{
  FLOX_CAPI_ENTER_VOID(h);
  toAccount(h)->setEquity(equity);
  FLOX_CAPI_LEAVE_VOID;
}
extern "C" void flox_account_add_equity(FloxAccountHandle h, double delta)
{
  FLOX_CAPI_ENTER_VOID(h);
  toAccount(h)->addEquity(delta);
  FLOX_CAPI_LEAVE_VOID;
}
extern "C" uint8_t flox_account_margin_mode(FloxAccountHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint8_t>(toAccount(h)->marginMode());
  FLOX_CAPI_LEAVE;
}
extern "C" void flox_account_set_margin_mode(FloxAccountHandle h, uint8_t mode)
{
  FLOX_CAPI_ENTER_VOID(h);
  using M = flox::MarginMode;
  toAccount(h)->setMarginMode(mode == 1 ? M::Isolated : M::Cross);
  FLOX_CAPI_LEAVE_VOID;
}
extern "C" void flox_account_open_position(FloxAccountHandle h, uint32_t symbol,
                                           double quantity, double entry_price)
{
  FLOX_CAPI_ENTER_VOID(h);
  toAccount(h)->openPosition(symbol, quantity, entry_price);
  FLOX_CAPI_LEAVE_VOID;
}
extern "C" void flox_account_open_position_isolated(FloxAccountHandle h,
                                                    uint32_t symbol,
                                                    double quantity,
                                                    double entry_price,
                                                    double isolated_equity)
{
  FLOX_CAPI_ENTER_VOID(h);
  toAccount(h)->openPosition(symbol, quantity, entry_price, isolated_equity);
  FLOX_CAPI_LEAVE_VOID;
}
extern "C" void flox_account_close_position(FloxAccountHandle h, uint32_t symbol)
{
  FLOX_CAPI_ENTER_VOID(h);
  toAccount(h)->closePosition(symbol);
  FLOX_CAPI_LEAVE_VOID;
}
extern "C" uint32_t flox_account_position_count(FloxAccountHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toAccount(h)->positionCount());
  FLOX_CAPI_LEAVE;
}
extern "C" void flox_account_set_mark(FloxAccountHandle h, uint32_t symbol, double price)
{
  FLOX_CAPI_ENTER_VOID(h);
  toAccount(h)->setMark(symbol, price);
  FLOX_CAPI_LEAVE_VOID;
}
extern "C" void flox_account_set_mark_at(FloxAccountHandle h, uint32_t symbol,
                                         double price, int64_t ts_ns)
{
  FLOX_CAPI_ENTER_VOID(h);
  toAccount(h)->setMark(symbol, price, ts_ns);
  FLOX_CAPI_LEAVE_VOID;
}
extern "C" int64_t flox_account_mark_ts(FloxAccountHandle h, uint32_t symbol)
{
  FLOX_CAPI_ENTER(h);
  return toAccount(h)->markTsFor(symbol);
  FLOX_CAPI_LEAVE;
}
extern "C" uint8_t flox_account_has_stale_marks(FloxAccountHandle h,
                                                int64_t now_ns, int64_t budget_ns)
{
  FLOX_CAPI_ENTER(h);
  return toAccount(h)->hasStaleMarks(now_ns, budget_ns) ? 1 : 0;
  FLOX_CAPI_LEAVE;
}
extern "C" double flox_account_total_notional(FloxAccountHandle h)
{
  FLOX_CAPI_ENTER(h);
  return toAccount(h)->totalNotional().toDouble();
  FLOX_CAPI_LEAVE;
}
extern "C" double flox_account_total_unrealised_pnl(FloxAccountHandle h)
{
  FLOX_CAPI_ENTER(h);
  return toAccount(h)->totalUnrealisedPnl().toDouble();
  FLOX_CAPI_LEAVE;
}
extern "C" void flox_account_record_fill(FloxAccountHandle h, int64_t ts_ns,
                                         double notional)
{
  FLOX_CAPI_ENTER_VOID(h);
  toAccount(h)->recordFill(ts_ns, notional);
  FLOX_CAPI_LEAVE_VOID;
}
extern "C" void flox_account_record_fill_ex(FloxAccountHandle h, int64_t ts_ns,
                                            double notional, uint32_t symbol)
{
  FLOX_CAPI_ENTER_VOID(h);
  toAccount(h)->recordFill(ts_ns, notional, symbol);
  FLOX_CAPI_LEAVE_VOID;
}
extern "C" uint32_t flox_account_rolling_notional_by_symbol_size(FloxAccountHandle h)
{
  FLOX_CAPI_ENTER(h);
  return static_cast<uint32_t>(toAccount(h)->rollingNotionalBySymbol30d().size());
  FLOX_CAPI_LEAVE;
}
extern "C" uint32_t flox_account_rolling_notional_by_symbol_copy(
    FloxAccountHandle h, uint32_t* symbols_out, double* notionals_out,
    uint32_t max)
{
  FLOX_CAPI_ENTER(h);
  const auto pairs = toAccount(h)->rollingNotionalBySymbol30d();
  const uint32_t n = static_cast<uint32_t>(
      std::min<size_t>(pairs.size(), static_cast<size_t>(max)));
  for (uint32_t i = 0; i < n; ++i)
  {
    symbols_out[i] = pairs[i].first;
    notionals_out[i] = pairs[i].second.toDouble();
  }
  return n;
  FLOX_CAPI_LEAVE;
}
extern "C" double flox_account_rolling_notional_30d(FloxAccountHandle h)
{
  FLOX_CAPI_ENTER(h);
  return toAccount(h)->rollingNotional30d().toDouble();
  FLOX_CAPI_LEAVE;
}
extern "C" void flox_account_reset_rolling(FloxAccountHandle h)
{
  FLOX_CAPI_ENTER_VOID(h);
  toAccount(h)->resetRolling();
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_liquidation_engine_attach_account(
    FloxLiquidationEngineHandle h, FloxAccountHandle account)
{
  FLOX_CAPI_ENTER_VOID(h);
  toLiqEngine(h)->attachAccount(toAccount(account));
  FLOX_CAPI_LEAVE_VOID;
}
extern "C" void flox_liquidation_engine_detach_account(
    FloxLiquidationEngineHandle h, uint64_t account_id)
{
  FLOX_CAPI_ENTER_VOID(h);
  toLiqEngine(h)->detachAccount(account_id);
  FLOX_CAPI_LEAVE_VOID;
}
extern "C" void flox_fee_schedule_bind_account(FloxFeeScheduleHandle h,
                                               FloxAccountHandle account)
{
  FLOX_CAPI_ENTER_VOID(h);
  toFee(h)->bindAccount(toAccount(account));
  FLOX_CAPI_LEAVE_VOID;
}
extern "C" void flox_fee_schedule_clear_account_binding(FloxFeeScheduleHandle h)
{
  FLOX_CAPI_ENTER_VOID(h);
  toFee(h)->clearAccountBinding();
  FLOX_CAPI_LEAVE_VOID;
}

// ============================================================
// T052: VenueStack
// ============================================================

#include "flox/backtest/venue_stack.h"

namespace
{

// The handle points at this wrapper, not at the VenueStack: the executor
// accessor has to hand back a FloxSimulatedExecutorImpl, and that wrapper
// needs an owner with the same lifetime as the stack. Registering the six
// borrowed addresses here, once, is also what lets _destroy recognise them.
struct FloxVenueStackImpl
{
  flox::VenueStack stack;
  FloxSimulatedExecutorImpl executorView;

  explicit FloxVenueStackImpl(flox::VenueStack&& s)
      : stack(std::move(s)), executorView(stack.clock(), stack.executor())
  {
    for (const void* p : borrowed())
    {
      FloxBorrowedHandles::add(p);
    }
  }

  ~FloxVenueStackImpl()
  {
    for (const void* p : borrowed())
    {
      FloxBorrowedHandles::remove(p);
    }
  }

  FloxVenueStackImpl(const FloxVenueStackImpl&) = delete;
  FloxVenueStackImpl& operator=(const FloxVenueStackImpl&) = delete;

 private:
  std::array<const void*, 6> borrowed()
  {
    return {&executorView, &stack.account(), &stack.liquidation(),
            &stack.fees(), &stack.funding(), &stack.venue()};
  }
};

inline FloxVenueStackImpl* toVenueStackImpl(FloxVenueStackHandle h)
{
  return static_cast<FloxVenueStackImpl*>(h);
}

inline flox::VenueStack* toVenueStack(FloxVenueStackHandle h)
{
  return &toVenueStackImpl(h)->stack;
}

}  // namespace

extern "C" FloxVenueStackHandle flox_venue_stack_create(uint8_t venue,
                                                        uint64_t account_id,
                                                        double equity)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  using flox::VenueStack;
  switch (venue)
  {
    case 1:
      return new FloxVenueStackImpl(VenueStack::bybit_linear(account_id, equity));
    case 2:
      return new FloxVenueStackImpl(VenueStack::okx_swap(account_id, equity));
    case 3:
      return new FloxVenueStackImpl(VenueStack::deribit(account_id, equity));
    case 0:
    default:
      return new FloxVenueStackImpl(VenueStack::binance_um_futures(account_id, equity));
  }
  FLOX_CAPI_LEAVE;
}
extern "C" void flox_venue_stack_destroy(FloxVenueStackHandle h)
{
  FLOX_CAPI_ENTER_DESTROY(h);
  delete toVenueStackImpl(h);
  FLOX_CAPI_LEAVE_VOID;
}
// Borrowed: valid while the stack lives, and flox_simulated_executor_destroy
// on it does nothing. See the ownership paragraph in flox_capi.h.
extern "C" FloxSimulatedExecutorHandle flox_venue_stack_executor(
    FloxVenueStackHandle h)
{
  FLOX_CAPI_ENTER(h);
  return &toVenueStackImpl(h)->executorView;
  FLOX_CAPI_LEAVE;
}
extern "C" FloxAccountHandle flox_venue_stack_account(FloxVenueStackHandle h)
{
  FLOX_CAPI_ENTER(h);
  return &toVenueStack(h)->account();
  FLOX_CAPI_LEAVE;
}
extern "C" FloxLiquidationEngineHandle flox_venue_stack_liquidation(
    FloxVenueStackHandle h)
{
  FLOX_CAPI_ENTER(h);
  return &toVenueStack(h)->liquidation();
  FLOX_CAPI_LEAVE;
}
extern "C" FloxFeeScheduleHandle flox_venue_stack_fees(FloxVenueStackHandle h)
{
  FLOX_CAPI_ENTER(h);
  return &toVenueStack(h)->fees();
  FLOX_CAPI_LEAVE;
}
extern "C" FloxFundingScheduleHandle flox_venue_stack_funding(
    FloxVenueStackHandle h)
{
  FLOX_CAPI_ENTER(h);
  return &toVenueStack(h)->funding();
  FLOX_CAPI_LEAVE;
}
extern "C" FloxVenueAvailabilityHandle flox_venue_stack_venue(
    FloxVenueStackHandle h)
{
  FLOX_CAPI_ENTER(h);
  return &toVenueStack(h)->venue();
  FLOX_CAPI_LEAVE;
}
extern "C" const char* flox_venue_stack_venue_name(FloxVenueStackHandle h)
{
  FLOX_CAPI_ENTER(h);
  return toVenueStack(h)->venueName().c_str();
  FLOX_CAPI_LEAVE;
}

// ============================================================
// T039: DEX amounts (u256 / i256) at the C boundary
// ============================================================

namespace
{
// Write a NUL-terminated string into out; return 1 if it fit, else 0.
inline uint8_t writeOut(const std::string& s, char* out, size_t out_len)
{
  if (out == nullptr || out_len == 0 || s.size() + 1 > out_len)
  {
    return 0;
  }
  std::memcpy(out, s.c_str(), s.size() + 1);
  return 1;
}
}  // namespace

extern "C" uint8_t flox_u256_roundtrip(const char* dec, char* out, size_t out_len)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  if (dec == nullptr)
  {
    return 0;
  }
  try
  {
    return writeOut(flox::u256::fromDec(dec).toDec(), out, out_len);
  }
  catch (...)
  {
    return 0;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" uint8_t flox_i256_roundtrip(const char* dec, char* out, size_t out_len)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  if (dec == nullptr)
  {
    return 0;
  }
  try
  {
    const flox::i256 v = flox::i256::fromDec(dec);
    const std::string mag = v.magnitude().toDec();
    const std::string s = (v.neg && !v.magnitude().isZero()) ? "-" + mag : mag;
    return writeOut(s, out, out_len);
  }
  catch (...)
  {
    return 0;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" uint8_t flox_u256_from_hex(const char* hex, char* out, size_t out_len)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  if (hex == nullptr)
  {
    return 0;
  }
  try
  {
    return writeOut(flox::u256::fromHex(hex).toDec(), out, out_len);
  }
  catch (...)
  {
    return 0;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" uint8_t flox_u256_to_words(const char* dec, uint64_t* words)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  if (dec == nullptr || words == nullptr)
  {
    return 0;
  }
  try
  {
    const flox::u256 v = flox::u256::fromDec(dec);
    for (int i = 0; i < 4; ++i)
    {
      words[i] = v.w[static_cast<std::size_t>(i)];  // little-endian, w[0] is the LSB limb
    }
    return 1;
  }
  catch (...)
  {
    return 0;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" uint8_t flox_u256_from_words(const uint64_t* words, char* out, size_t out_len)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  if (words == nullptr)
  {
    return 0;
  }
  flox::u256 v;
  for (int i = 0; i < 4; ++i)
  {
    v.w[static_cast<std::size_t>(i)] = words[i];
  }
  return writeOut(v.toDec(), out, out_len);
  FLOX_CAPI_LEAVE;
}

// ============================================================
// T040: AMM curves
// ============================================================

namespace
{
inline flox::INTokenCurve* toCurve(FloxCurveHandle h)
{
  return static_cast<flox::INTokenCurve*>(h);
}
}  // namespace

extern "C" FloxCurveHandle flox_curve_constant_product(const char* reserve0, const char* reserve1,
                                                       uint64_t fee_num, uint64_t fee_den)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  if (reserve0 == nullptr || reserve1 == nullptr)
  {
    return nullptr;
  }
  try
  {
    return new flox::ConstantProductCurve(flox::u256::fromDec(reserve0),
                                          flox::u256::fromDec(reserve1), fee_num, fee_den);
  }
  catch (...)
  {
    return nullptr;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" FloxCurveHandle flox_curve_raydium_cp(const char* reserve0, const char* reserve1,
                                                 uint64_t trade_fee_rate, uint64_t creator_fee_rate,
                                                 uint8_t creator_fee_on_input)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  if (reserve0 == nullptr || reserve1 == nullptr)
  {
    return nullptr;
  }
  try
  {
    return new flox::RaydiumCpCurve(flox::u256::fromDec(reserve0), flox::u256::fromDec(reserve1),
                                    trade_fee_rate, creator_fee_rate, creator_fee_on_input != 0);
  }
  catch (...)
  {
    return nullptr;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" FloxCurveHandle flox_curve_uniswap_v3(const char* sqrt_price_x96, const char* liquidity,
                                                 uint32_t fee_pips,
                                                 const char* const* tick_sqrt_ratio,
                                                 const char* const* tick_liquidity_net,
                                                 size_t n_ticks)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  if (sqrt_price_x96 == nullptr || liquidity == nullptr)
  {
    return nullptr;
  }
  try
  {
    std::vector<flox::ClTick> ticks;
    ticks.reserve(n_ticks);
    for (size_t k = 0; k < n_ticks; ++k)
    {
      ticks.push_back({flox::u256::fromDec(tick_sqrt_ratio[k]),
                       flox::i256::fromDec(tick_liquidity_net[k])});
    }
    return new flox::ConcentratedLiquidityCurve(flox::u256::fromDec(sqrt_price_x96),
                                                flox::u256::fromDec(liquidity), fee_pips,
                                                std::move(ticks));
  }
  catch (...)
  {
    return nullptr;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" size_t flox_curve_token_count(FloxCurveHandle curve)
{
  FLOX_CAPI_ENTER(curve);
  return curve == nullptr ? 0 : toCurve(curve)->tokenCount();
  FLOX_CAPI_LEAVE;
}

extern "C" uint8_t flox_curve_amount_out(FloxCurveHandle curve, size_t i, size_t j,
                                         const char* amount_in, char* out, size_t out_len)
{
  FLOX_CAPI_ENTER(curve);
  if (curve == nullptr || amount_in == nullptr)
  {
    return 0;
  }
  try
  {
    return writeOut(toCurve(curve)->amountOut(i, j, flox::u256::fromDec(amount_in)).toDec(), out,
                    out_len);
  }
  catch (...)
  {
    return 0;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" uint8_t flox_curve_apply_swap(FloxCurveHandle curve, size_t i, size_t j,
                                         const char* amount_in, char* out, size_t out_len)
{
  FLOX_CAPI_ENTER(curve);
  if (curve == nullptr || amount_in == nullptr)
  {
    return 0;
  }
  try
  {
    return writeOut(toCurve(curve)->applySwap(i, j, flox::u256::fromDec(amount_in)).toDec(), out,
                    out_len);
  }
  catch (...)
  {
    return 0;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" uint8_t flox_curve_balance(FloxCurveHandle curve, size_t i, char* out, size_t out_len)
{
  FLOX_CAPI_ENTER(curve);
  if (curve == nullptr)
  {
    return 0;
  }
  try
  {
    const std::vector<flox::u256>& b = toCurve(curve)->balances();
    if (i >= b.size())
    {
      return 0;
    }
    return writeOut(b[i].toDec(), out, out_len);
  }
  catch (...)
  {
    return 0;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" uint8_t flox_curve_sqrt_price(FloxCurveHandle curve, char* out, size_t out_len)
{
  FLOX_CAPI_ENTER(curve);
  const auto* cl = curve == nullptr
                       ? nullptr
                       : dynamic_cast<const flox::ConcentratedLiquidityCurve*>(toCurve(curve));
  if (cl == nullptr)
  {
    writeOut("0", out, out_len);
    return 0;
  }
  return writeOut(cl->sqrtPrice().toDec(), out, out_len);
  FLOX_CAPI_LEAVE;
}

extern "C" uint8_t flox_curve_liquidity(FloxCurveHandle curve, char* out, size_t out_len)
{
  FLOX_CAPI_ENTER(curve);
  const auto* cl = curve == nullptr
                       ? nullptr
                       : dynamic_cast<const flox::ConcentratedLiquidityCurve*>(toCurve(curve));
  if (cl == nullptr)
  {
    writeOut("0", out, out_len);
    return 0;
  }
  return writeOut(cl->liquidity().toDec(), out, out_len);
  FLOX_CAPI_LEAVE;
}

extern "C" FloxCurveHandle flox_curve_clone(FloxCurveHandle curve)
{
  FLOX_CAPI_ENTER(curve);
  return curve == nullptr ? nullptr : toCurve(curve)->clone().release();
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_curve_destroy(FloxCurveHandle curve)
{
  FLOX_CAPI_ENTER_DESTROY(curve);
  delete toCurve(curve);
  FLOX_CAPI_LEAVE_VOID;
}

// ============================================================
// T041: Pool-state tape replay
// ============================================================

namespace
{
struct PoolTapeBuilder
{
  std::vector<uint8_t> bytes;
};

struct PoolReplayResult
{
  std::unique_ptr<flox::INTokenCurve> seed;
  std::unique_ptr<flox::AmmDexConnector> conn;
  std::unique_ptr<flox::PoolStateReplay> replay;
  std::size_t trades{0};
};

inline PoolTapeBuilder* toTape(FloxPoolTapeHandle h) { return static_cast<PoolTapeBuilder*>(h); }
inline PoolReplayResult* toReplay(FloxPoolReplayHandle h)
{
  return static_cast<PoolReplayResult*>(h);
}
}  // namespace

extern "C" FloxPoolTapeHandle flox_pool_tape_create(void)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return new PoolTapeBuilder();
  FLOX_CAPI_LEAVE;
}
extern "C" void flox_pool_tape_destroy(FloxPoolTapeHandle tape)
{
  FLOX_CAPI_ENTER_DESTROY(tape);
  delete toTape(tape);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_pool_tape_descriptor_constant_product(FloxPoolTapeHandle tape,
                                                           uint64_t fee_num, uint64_t fee_den,
                                                           uint8_t base_dec, uint8_t quote_dec)
{
  FLOX_CAPI_ENTER_VOID(tape);
  flox::PoolStateWriter(toTape(tape)->bytes)
      .descriptorConstantProduct(fee_num, fee_den, base_dec, quote_dec);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_pool_tape_descriptor_raydium_cp(FloxPoolTapeHandle tape,
                                                     uint64_t trade_fee_rate,
                                                     uint64_t creator_fee_rate,
                                                     uint8_t creator_fee_on_input, uint8_t base_dec,
                                                     uint8_t quote_dec)
{
  FLOX_CAPI_ENTER_VOID(tape);
  flox::PoolStateWriter(toTape(tape)->bytes)
      .descriptorRaydiumCp(trade_fee_rate, creator_fee_rate, creator_fee_on_input != 0, base_dec,
                           quote_dec);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" void flox_pool_tape_descriptor_clmm(FloxPoolTapeHandle tape, uint8_t venue,
                                               uint32_t fee_pips, uint8_t base_dec,
                                               uint8_t quote_dec)
{
  FLOX_CAPI_ENTER_VOID(tape);
  flox::PoolStateWriter(toTape(tape)->bytes)
      .descriptorClmm(static_cast<flox::PoolVenue>(venue), fee_pips, base_dec, quote_dec);
  FLOX_CAPI_LEAVE_VOID;
}

extern "C" uint8_t flox_pool_tape_checkpoint(FloxPoolTapeHandle tape, int64_t ts_ns,
                                             const char* reserve0, const char* reserve1)
{
  FLOX_CAPI_ENTER(tape);
  if (reserve0 == nullptr || reserve1 == nullptr)
  {
    return 0;
  }
  try
  {
    flox::PoolStateWriter(toTape(tape)->bytes)
        .checkpoint(ts_ns, flox::u256::fromDec(reserve0), flox::u256::fromDec(reserve1));
    return 1;
  }
  catch (...)
  {
    return 0;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" uint8_t flox_pool_tape_checkpoint_clmm(FloxPoolTapeHandle tape, int64_t ts_ns,
                                                  const char* sqrt_price, const char* liquidity,
                                                  const char* const* tick_sqrt_ratio,
                                                  const char* const* tick_liquidity_net,
                                                  size_t n_ticks)
{
  FLOX_CAPI_ENTER(tape);
  if (sqrt_price == nullptr || liquidity == nullptr)
  {
    return 0;
  }
  try
  {
    std::vector<flox::ClTick> ticks;
    ticks.reserve(n_ticks);
    for (size_t k = 0; k < n_ticks; ++k)
    {
      ticks.push_back({flox::u256::fromDec(tick_sqrt_ratio[k]),
                       flox::i256::fromDec(tick_liquidity_net[k])});
    }
    flox::PoolStateWriter(toTape(tape)->bytes)
        .checkpointClmm(ts_ns, flox::u256::fromDec(sqrt_price), flox::u256::fromDec(liquidity),
                        ticks);
    return 1;
  }
  catch (...)
  {
    return 0;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" uint8_t flox_pool_tape_swap(FloxPoolTapeHandle tape, int64_t ts_ns,
                                       uint8_t base_for_quote, const char* amount_in)
{
  FLOX_CAPI_ENTER(tape);
  if (amount_in == nullptr)
  {
    return 0;
  }
  try
  {
    flox::PoolStateWriter(toTape(tape)->bytes)
        .swap(ts_ns, base_for_quote != 0, flox::u256::fromDec(amount_in));
    return 1;
  }
  catch (...)
  {
    return 0;
  }
  FLOX_CAPI_LEAVE;
}

extern "C" FloxPoolReplayHandle flox_pool_tape_replay(FloxPoolTapeHandle tape, size_t base_idx,
                                                      size_t quote_idx, uint8_t base_dec,
                                                      uint8_t quote_dec)
{
  FLOX_CAPI_ENTER(tape);
  auto* r = new PoolReplayResult();
  // A throwaway seed for the connector ctor; the replay re-points it at the curve it
  // rebuilds from the tape's first Checkpoint.
  r->seed = std::make_unique<flox::ConstantProductCurve>(flox::u256(1), flox::u256(1), 1, 1);
  r->conn = std::make_unique<flox::AmmDexConnector>("amm", flox::SymbolId{1}, *r->seed, base_idx,
                                                    quote_idx, base_dec, quote_dec, 1,
                                                    flox::u256(1));
  r->conn->setCallbacks([](const flox::BookUpdateEvent&) {},
                        [r](const flox::TradeEvent&)
                        { ++r->trades; });
  r->replay = std::make_unique<flox::PoolStateReplay>(*r->conn);
  r->replay->run(toTape(tape)->bytes);
  return r;
  FLOX_CAPI_LEAVE;
}

extern "C" size_t flox_pool_replay_drift_count(FloxPoolReplayHandle replay)
{
  FLOX_CAPI_ENTER(replay);
  return replay == nullptr ? 0 : toReplay(replay)->replay->driftCount();
  FLOX_CAPI_LEAVE;
}

extern "C" size_t flox_pool_replay_trade_count(FloxPoolReplayHandle replay)
{
  FLOX_CAPI_ENTER(replay);
  return replay == nullptr ? 0 : toReplay(replay)->trades;
  FLOX_CAPI_LEAVE;
}

extern "C" FloxCurveHandle flox_pool_replay_curve(FloxPoolReplayHandle replay)
{
  FLOX_CAPI_ENTER(replay);
  // Borrowed: the replay owns this curve. Registered on the way out rather
  // than at construction because the replay only has a curve once the tape
  // carried a checkpoint, and a caller cannot hold the pointer without
  // having come through here first.
  auto* curve = const_cast<flox::INTokenCurve*>(toReplay(replay)->replay->curve());
  FloxBorrowedHandles::add(curve);
  return curve;
  FLOX_CAPI_LEAVE;
}

extern "C" void flox_pool_replay_destroy(FloxPoolReplayHandle replay)
{
  FLOX_CAPI_ENTER_DESTROY(replay);
  auto* impl = toReplay(replay);
  FloxBorrowedHandles::remove(const_cast<flox::INTokenCurve*>(impl->replay->curve()));
  delete impl;
  FLOX_CAPI_LEAVE_VOID;
}

// ============================================================
// Diagnostics: ABI version and the last error on this thread
// ============================================================

// The shared library exported 729 symbols and not one of them said which ABI
// they belonged to, so a consumer loading it through dlopen or ctypes -- the
// way docs/bindings/capi.md suggests -- had no way to notice a mismatch. The
// structs on this boundary are solid, with no reserved tail, so a version
// skew shows up as silently wrong numbers rather than a failed load.
extern "C" uint32_t flox_capi_abi_version(void)
{
  FLOX_CAPI_ENTER_NOHANDLE;
  return FLOX_CAPI_ABI_VERSION;
  FLOX_CAPI_LEAVE;
}

// These three are the only exported functions without the FLOX_CAPI_ENTER /
// FLOX_CAPI_LEAVE pair, because the pair reports through them: catching here
// and returning {} would hand back a NULL message pointer and lose the very
// error the caller is asking about. They take no handle and touch nothing
// that throws once the thread-local slot exists, so each carries its own
// narrow catch instead.
extern "C" int flox_last_error_code(void)
{
  try
  {
    return floxCapiLastError().code;
  }
  catch (...)
  {
    return kFloxCapiErrException;
  }
}

extern "C" const char* flox_last_error_message(void)
{
  try
  {
    return floxCapiLastError().message.c_str();
  }
  catch (...)
  {
    return "";
  }
}

extern "C" void flox_clear_last_error(void)
{
  try
  {
    auto& e = floxCapiLastError();
    e.code = kFloxCapiOk;
    e.message.clear();
  }
  catch (...)
  {
  }
}
