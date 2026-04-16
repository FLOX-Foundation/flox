#include "js_strategy.h"
#include "js_bindings.h"

#include <iostream>
#include <stdexcept>

// Embedded JS stdlib
static const char* const STRATEGY_JS =
#include "js_stdlib_strategy.inc"
    ;

static const char* const INDICATORS_JS =
#include "js_stdlib_indicators.inc"
    ;

namespace flox
{

FloxJsStrategy::FloxJsStrategy(const std::string& scriptPath, SymbolRegistry& registry)
    : _registry(registry)
{
  registerFloxBindings(_engine.context());
  loadStdlib();
  try
  {
    loadScript(scriptPath);
  }
  catch (...)
  {
    if (!JS_IsUndefined(_strategyObj))
    {
      JS_FreeValue(_engine.context(), _strategyObj);
      _strategyObj = JS_UNDEFINED;
    }
    _engine.eval("__flox_registered_strategy = null; flox = null;", "<cleanup>");
    throw;
  }
}

FloxJsStrategy::~FloxJsStrategy()
{
  if (!JS_IsUndefined(_strategyObj))
  {
    JS_FreeValue(_engine.context(), _strategyObj);
    _strategyObj = JS_UNDEFINED;
  }
  // Clear globals that hold JS object references before runtime teardown
  _engine.eval("__flox_registered_strategy = null; flox = null;", "<cleanup>");
}

void FloxJsStrategy::loadStdlib()
{
  if (!_engine.eval(INDICATORS_JS, "flox/indicators.js"))
  {
    throw std::runtime_error("Failed to load indicators.js: " + _engine.getErrorMessage());
  }
  if (!_engine.eval(STRATEGY_JS, "flox/strategy.js"))
  {
    throw std::runtime_error("Failed to load strategy.js: " + _engine.getErrorMessage());
  }

  // Create flox global object with register() and batch indicators
  const char* registerCode = R"(
    var __flox_registered_strategy = null;

    class OrderBook {
      constructor(tickSize) { this._h = __flox_book_create(tickSize || 0.01); }
      destroy() { __flox_book_destroy(this._h); }
      applySnapshot(bidPrices, bidQtys, askPrices, askQtys) {
        __flox_book_apply_snapshot(this._h, bidPrices, bidQtys, askPrices, askQtys);
      }
      applyDelta(bidPrices, bidQtys, askPrices, askQtys) {
        __flox_book_apply_delta(this._h, bidPrices, bidQtys, askPrices, askQtys);
      }
      bestBid() { return __flox_book_best_bid(this._h); }
      bestAsk() { return __flox_book_best_ask(this._h); }
      mid() { return __flox_book_mid(this._h); }
      spread() { return __flox_book_spread(this._h); }
      getBids(maxLevels) { return __flox_book_get_bids(this._h, maxLevels || 20); }
      getAsks(maxLevels) { return __flox_book_get_asks(this._h, maxLevels || 20); }
      isCrossed() { return __flox_book_is_crossed(this._h); }
      clear() { __flox_book_clear(this._h); }
    }

    class SimulatedExecutor {
      constructor() { this._h = __flox_executor_create(); }
      destroy() { __flox_executor_destroy(this._h); }
      submitOrder(id, side, price, qty, type, symbol) {
        __flox_executor_submit(this._h, id, side === "buy" ? 0 : 1, price, qty, type || 0, symbol || 1);
      }
      onBar(symbol, closePrice) { __flox_executor_on_bar(this._h, symbol, closePrice); }
      onTrade(symbol, price, isBuy) { __flox_executor_on_trade(this._h, symbol, price, isBuy ? 1 : 0); }
      advanceClock(timestampNs) { __flox_executor_advance_clock(this._h, timestampNs); }
      get fillCount() { return __flox_executor_fill_count(this._h); }
    }

    class PositionTracker {
      constructor(costBasis) { this._h = __flox_pos_create(costBasis || 0); }
      destroy() { __flox_pos_destroy(this._h); }
      onFill(symbol, side, price, qty) {
        __flox_pos_on_fill(this._h, symbol, side === "buy" ? 0 : 1, price, qty);
      }
      position(symbol) { return __flox_pos_position(this._h, symbol); }
      avgEntryPrice(symbol) { return __flox_pos_avg_entry(this._h, symbol); }
      realizedPnl(symbol) { return __flox_pos_pnl(this._h, symbol); }
      totalRealizedPnl() { return __flox_pos_total_pnl(this._h); }
    }

    class VolumeProfile {
      constructor(tickSize) { this._h = __flox_vprofile_create(tickSize || 0.01); }
      destroy() { __flox_vprofile_destroy(this._h); }
      addTrade(price, qty, isBuy) { __flox_vprofile_add_trade(this._h, price, qty, isBuy ? 1 : 0); }
      poc() { return __flox_vprofile_poc(this._h); }
      valueAreaHigh() { return __flox_vprofile_vah(this._h); }
      valueAreaLow() { return __flox_vprofile_val(this._h); }
      totalVolume() { return __flox_vprofile_total_volume(this._h); }
      clear() { __flox_vprofile_clear(this._h); }
    }

    class L3Book {
      constructor() { this._h = __flox_l3_create(); }
      destroy() { __flox_l3_destroy(this._h); }
      addOrder(orderId, price, qty, side) {
        return __flox_l3_add_order(this._h, orderId, price, qty, side === "buy" ? 0 : 1);
      }
      removeOrder(orderId) { return __flox_l3_remove_order(this._h, orderId); }
      modifyOrder(orderId, newQty) { return __flox_l3_modify_order(this._h, orderId, newQty); }
      bestBid() { return __flox_l3_best_bid(this._h); }
      bestAsk() { return __flox_l3_best_ask(this._h); }
    }

    class FootprintBar {
      constructor(tickSize) { this._h = __flox_fp_create(tickSize || 0.01); }
      destroy() { __flox_fp_destroy(this._h); }
      addTrade(price, qty, isBuy) { __flox_fp_add_trade(this._h, price, qty, isBuy ? 1 : 0); }
      totalDelta() { return __flox_fp_total_delta(this._h); }
      totalVolume() { return __flox_fp_total_volume(this._h); }
      clear() { __flox_fp_clear(this._h); }
    }

    class MarketProfile {
      constructor(tickSize, periodMinutes, sessionStartNs) {
        this._h = __flox_mp_create(tickSize || 0.01, periodMinutes || 30, sessionStartNs || 0);
      }
      destroy() { __flox_mp_destroy(this._h); }
      addTrade(timestampNs, price, qty, isBuy) { __flox_mp_add_trade(this._h, timestampNs, price, qty, isBuy ? 1 : 0); }
      poc() { return __flox_mp_poc(this._h); }
      valueAreaHigh() { return __flox_mp_vah(this._h); }
      valueAreaLow() { return __flox_mp_val(this._h); }
      initialBalanceHigh() { return __flox_mp_ib_high(this._h); }
      initialBalanceLow() { return __flox_mp_ib_low(this._h); }
      isPoorHigh() { return __flox_mp_is_poor_high(this._h); }
      isPoorLow() { return __flox_mp_is_poor_low(this._h); }
      clear() { __flox_mp_clear(this._h); }
    }

    class CompositeBook {
      constructor() { this._h = __flox_cb_create(); }
      destroy() { __flox_cb_destroy(this._h); }
      bestBid(symbol) { return __flox_cb_best_bid(this._h, symbol); }
      bestAsk(symbol) { return __flox_cb_best_ask(this._h, symbol); }
      hasArbitrage(symbol) { return __flox_cb_has_arb(this._h, symbol); }
    }

    class OrderTracker {
      constructor() { this._h = __flox_ot_create(); }
      destroy() { __flox_ot_destroy(this._h); }
      onSubmitted(orderId, symbol, side, price, qty) {
        return __flox_ot_submit(this._h, orderId, symbol, side === "buy" ? 0 : 1, price, qty);
      }
      onFilled(orderId, fillQty) { return __flox_ot_filled(this._h, orderId, fillQty); }
      onCanceled(orderId) { return __flox_ot_canceled(this._h, orderId); }
      isActive(orderId) { return __flox_ot_is_active(this._h, orderId); }
      get activeCount() { return __flox_ot_active_count(this._h); }
      prune() { __flox_ot_prune(this._h); }
    }

    class PositionGroupTracker {
      constructor() { this._h = __flox_pg_create(); }
      destroy() { __flox_pg_destroy(this._h); }
      openPosition(orderId, symbol, side, price, qty) {
        return __flox_pg_open(this._h, orderId, symbol, side === "buy" ? 0 : 1, price, qty);
      }
      closePosition(positionId, exitPrice) { __flox_pg_close(this._h, positionId, exitPrice); }
      netPosition(symbol) { return __flox_pg_net(this._h, symbol); }
      realizedPnl(symbol) { return __flox_pg_pnl(this._h, symbol); }
      totalRealizedPnl() { return __flox_pg_total_pnl(this._h); }
      openPositionCount(symbol) { return __flox_pg_open_count(this._h, symbol); }
      prune() { __flox_pg_prune(this._h); }
    }

    var flox = {
      register: function(strategy) {
        __flox_registered_strategy = strategy;
      },
      correlation: function(x, y) { return __flox_stat_correlation(x, y); },
      profitFactor: function(pnl) { return __flox_stat_profit_factor(pnl); },
      winRate: function(pnl) { return __flox_stat_win_rate(pnl); },
      bootstrapCI: function(data, confidence, samples) { return __flox_stat_bootstrap_ci(data, confidence, samples); },
      permutationTest: function(g1, g2, n) { return __flox_stat_permutation_test(g1, g2, n); },
      validateSegment: function(path) { return __flox_segment_validate(path); },
      mergeSegments: function(inputDir, outputPath) { return __flox_segment_merge(inputDir, outputPath); }
    };
  )";
  if (!_engine.eval(registerCode, "<flox_register>"))
  {
    throw std::runtime_error("Failed to set up flox.register: " + _engine.getErrorMessage());
  }
}

void FloxJsStrategy::loadScript(const std::string& path)
{
  if (!_engine.loadFile(path))
  {
    throw std::runtime_error("Failed to load script " + path + ": " + _engine.getErrorMessage());
  }

  // Get the registered strategy
  _strategyObj = _engine.getGlobalProperty("__flox_registered_strategy");
  if (JS_IsNull(_strategyObj) || JS_IsUndefined(_strategyObj))
  {
    throw std::runtime_error(
        "No strategy registered. Call flox.register(new YourStrategy()) in "
        "your script.");
  }

  resolveSymbols();
}

void FloxJsStrategy::resolveSymbols()
{
  auto* ctx = _engine.context();

  // Read _exchange
  JSValue exchangeVal = JS_GetPropertyStr(ctx, _strategyObj, "_exchange");
  std::string defaultExchange;
  if (JS_IsString(exchangeVal))
  {
    const char* s = JS_ToCString(ctx, exchangeVal);
    if (s)
    {
      defaultExchange = s;
      JS_FreeCString(ctx, s);
    }
  }
  JS_FreeValue(ctx, exchangeVal);

  // Read _symbolNames array
  JSValue namesVal = JS_GetPropertyStr(ctx, _strategyObj, "_symbolNames");
  JSValue lengthVal = JS_GetPropertyStr(ctx, namesVal, "length");
  uint32_t len = 0;
  JS_ToUint32(ctx, &len, lengthVal);
  JS_FreeValue(ctx, lengthVal);

  _symbolIds.clear();
  _symbolNames.clear();

  for (uint32_t i = 0; i < len; i++)
  {
    JSValue elem = JS_GetPropertyUint32(ctx, namesVal, i);
    const char* nameStr = JS_ToCString(ctx, elem);
    std::string symName = nameStr ? nameStr : "";
    JS_FreeCString(ctx, nameStr);
    JS_FreeValue(ctx, elem);

    // Parse "Exchange:SYMBOL" or use default exchange
    std::string exchange = defaultExchange;
    std::string symbol = symName;
    auto colon = symName.find(':');
    if (colon != std::string::npos)
    {
      exchange = symName.substr(0, colon);
      symbol = symName.substr(colon + 1);
    }

    if (exchange.empty())
    {
      JS_FreeValue(ctx, namesVal);
      throw std::runtime_error("No exchange specified for symbol: " + symName);
    }

    // Resolve or register the symbol
    uint32_t symId = 0;
    FloxRegistryHandle regHandle = static_cast<FloxRegistryHandle>(&_registry);
    if (!flox_registry_get_symbol_id(regHandle, exchange.c_str(), symbol.c_str(), &symId))
    {
      symId = flox_registry_add_symbol(regHandle, exchange.c_str(), symbol.c_str(), 0.01);
    }

    _symbolIds.push_back(symId);
    _symbolNames.push_back(symName);
  }
  JS_FreeValue(ctx, namesVal);

  // Inject _symbolMap and _reverseMap into JS strategy object
  JSValue symbolMap = JS_NewObject(ctx);
  JSValue reverseMap = JS_NewObject(ctx);
  for (size_t i = 0; i < _symbolIds.size(); i++)
  {
    JS_SetPropertyStr(ctx, symbolMap, _symbolNames[i].c_str(),
                      JS_NewUint32(ctx, _symbolIds[i]));
    char idStr[16];
    snprintf(idStr, sizeof(idStr), "%u", _symbolIds[i]);
    JS_SetPropertyStr(ctx, reverseMap, idStr,
                      JS_NewString(ctx, _symbolNames[i].c_str()));
  }
  JS_SetPropertyStr(ctx, _strategyObj, "_symbolMap", symbolMap);
  JS_SetPropertyStr(ctx, _strategyObj, "_reverseMap", reverseMap);
}

void FloxJsStrategy::injectHandle(FloxStrategyHandle handle)
{
  auto* ctx = _engine.context();
  JSValue handleVal = createHandleObject(ctx, handle);
  JS_SetPropertyStr(ctx, _strategyObj, "_handle", handleVal);
}

FloxStrategyCallbacks FloxJsStrategy::getCallbacks()
{
  FloxStrategyCallbacks cb{};
  cb.on_trade = FloxJsStrategy::onTrade;
  cb.on_book = FloxJsStrategy::onBook;
  cb.on_start = FloxJsStrategy::onStart;
  cb.on_stop = FloxJsStrategy::onStop;
  cb.user_data = this;
  return cb;
}

// ============================================================
// C callback implementations
// ============================================================

void FloxJsStrategy::onTrade(void* userData, const FloxSymbolContext* ctx,
                             const FloxTradeData* trade)
{
  auto* self = static_cast<FloxJsStrategy*>(userData);
  auto* jsCtx = self->_engine.context();

  JSValue ctxObj = self->makeCtxObject(ctx);
  JSValue tradeObj = self->makeTradeObject(trade);

  JSValue method = JS_GetPropertyStr(jsCtx, self->_strategyObj, "_dispatchTrade");
  if (JS_IsFunction(jsCtx, method))
  {
    JSValue args[2] = {ctxObj, tradeObj};
    JSValue ret = JS_Call(jsCtx, method, self->_strategyObj, 2, args);
    if (JS_IsException(ret))
    {
      std::cerr << "[flox-js] Error in onTrade: " << self->_engine.getErrorMessage() << std::endl;
    }
    JS_FreeValue(jsCtx, ret);
  }
  JS_FreeValue(jsCtx, method);
  JS_FreeValue(jsCtx, ctxObj);
  JS_FreeValue(jsCtx, tradeObj);
}

void FloxJsStrategy::onBook(void* userData, const FloxSymbolContext* ctx,
                            const FloxBookData* book)
{
  auto* self = static_cast<FloxJsStrategy*>(userData);
  auto* jsCtx = self->_engine.context();

  JSValue ctxObj = self->makeCtxObject(ctx);
  JSValue bookObj = self->makeBookObject(book);

  JSValue method = JS_GetPropertyStr(jsCtx, self->_strategyObj, "_dispatchBook");
  if (JS_IsFunction(jsCtx, method))
  {
    JSValue args[2] = {ctxObj, bookObj};
    JSValue ret = JS_Call(jsCtx, method, self->_strategyObj, 2, args);
    if (JS_IsException(ret))
    {
      std::cerr << "[flox-js] Error in onBookUpdate: " << self->_engine.getErrorMessage()
                << std::endl;
    }
    JS_FreeValue(jsCtx, ret);
  }
  JS_FreeValue(jsCtx, method);
  JS_FreeValue(jsCtx, ctxObj);
  JS_FreeValue(jsCtx, bookObj);
}

void FloxJsStrategy::onStart(void* userData)
{
  auto* self = static_cast<FloxJsStrategy*>(userData);
  auto* jsCtx = self->_engine.context();

  JSValue method = JS_GetPropertyStr(jsCtx, self->_strategyObj, "onStart");
  if (JS_IsFunction(jsCtx, method))
  {
    JSValue ret = JS_Call(jsCtx, method, self->_strategyObj, 0, nullptr);
    if (JS_IsException(ret))
    {
      std::cerr << "[flox-js] Error in onStart: " << self->_engine.getErrorMessage() << std::endl;
    }
    JS_FreeValue(jsCtx, ret);
  }
  JS_FreeValue(jsCtx, method);
}

void FloxJsStrategy::onStop(void* userData)
{
  auto* self = static_cast<FloxJsStrategy*>(userData);
  auto* jsCtx = self->_engine.context();

  JSValue method = JS_GetPropertyStr(jsCtx, self->_strategyObj, "onStop");
  if (JS_IsFunction(jsCtx, method))
  {
    JSValue ret = JS_Call(jsCtx, method, self->_strategyObj, 0, nullptr);
    if (JS_IsException(ret))
    {
      std::cerr << "[flox-js] Error in onStop: " << self->_engine.getErrorMessage() << std::endl;
    }
    JS_FreeValue(jsCtx, ret);
  }
  JS_FreeValue(jsCtx, method);
}

// ============================================================
// JS object constructors
// ============================================================

JSValue FloxJsStrategy::makeCtxObject(const FloxSymbolContext* ctx)
{
  auto* c = _engine.context();
  JSValue obj = JS_NewObject(c);
  JS_SetPropertyStr(c, obj, "symbolId", JS_NewUint32(c, ctx->symbol_id));
  JS_SetPropertyStr(c, obj, "position", JS_NewFloat64(c, flox_quantity_to_double(ctx->position_raw)));
  JS_SetPropertyStr(c, obj, "avgEntryPrice",
                    JS_NewFloat64(c, flox_price_to_double(ctx->avg_entry_price_raw)));
  JS_SetPropertyStr(c, obj, "lastTradePrice",
                    JS_NewFloat64(c, flox_price_to_double(ctx->last_trade_price_raw)));
  JS_SetPropertyStr(c, obj, "lastUpdateNs",
                    JS_NewFloat64(c, static_cast<double>(ctx->last_update_ns)));

  JSValue bookObj = JS_NewObject(c);
  JS_SetPropertyStr(c, bookObj, "bidPrice",
                    JS_NewFloat64(c, flox_price_to_double(ctx->book.bid_price_raw)));
  JS_SetPropertyStr(c, bookObj, "askPrice",
                    JS_NewFloat64(c, flox_price_to_double(ctx->book.ask_price_raw)));
  JS_SetPropertyStr(c, bookObj, "midPrice",
                    JS_NewFloat64(c, flox_price_to_double(ctx->book.mid_raw)));
  JS_SetPropertyStr(c, bookObj, "spread",
                    JS_NewFloat64(c, flox_price_to_double(ctx->book.spread_raw)));
  JS_SetPropertyStr(c, obj, "book", bookObj);
  return obj;
}

JSValue FloxJsStrategy::makeTradeObject(const FloxTradeData* trade)
{
  auto* c = _engine.context();
  JSValue obj = JS_NewObject(c);
  JS_SetPropertyStr(c, obj, "symbolId", JS_NewUint32(c, trade->symbol));
  JS_SetPropertyStr(c, obj, "price", JS_NewFloat64(c, flox_price_to_double(trade->price_raw)));
  JS_SetPropertyStr(c, obj, "qty",
                    JS_NewFloat64(c, flox_quantity_to_double(trade->quantity_raw)));
  JS_SetPropertyStr(c, obj, "isBuy", JS_NewBool(c, trade->is_buy != 0));
  JS_SetPropertyStr(c, obj, "timestampNs",
                    JS_NewFloat64(c, static_cast<double>(trade->exchange_ts_ns)));
  return obj;
}

JSValue FloxJsStrategy::makeBookObject(const FloxBookData* book)
{
  auto* c = _engine.context();
  JSValue obj = JS_NewObject(c);
  JS_SetPropertyStr(c, obj, "symbolId", JS_NewUint32(c, book->symbol));
  JS_SetPropertyStr(c, obj, "timestampNs",
                    JS_NewFloat64(c, static_cast<double>(book->exchange_ts_ns)));

  JSValue snap = JS_NewObject(c);
  JS_SetPropertyStr(c, snap, "bidPrice",
                    JS_NewFloat64(c, flox_price_to_double(book->snapshot.bid_price_raw)));
  JS_SetPropertyStr(c, snap, "askPrice",
                    JS_NewFloat64(c, flox_price_to_double(book->snapshot.ask_price_raw)));
  JS_SetPropertyStr(c, snap, "midPrice",
                    JS_NewFloat64(c, flox_price_to_double(book->snapshot.mid_raw)));
  JS_SetPropertyStr(c, snap, "spread",
                    JS_NewFloat64(c, flox_price_to_double(book->snapshot.spread_raw)));
  JS_SetPropertyStr(c, obj, "snapshot", snap);
  return obj;
}

}  // namespace flox
