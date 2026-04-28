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

    const _slippageMap = { none: 0, fixed_ticks: 1, fixed_bps: 2, volume_impact: 3 };
    const _queueMap = { none: 0, tob: 1, full: 2 };

    class SimulatedExecutor {
      constructor() { this._h = __flox_executor_create(); }
      destroy() { __flox_executor_destroy(this._h); }
      submitOrder(id, side, price, qty, type, symbol) {
        // JS convention: type 0 = market (default), 1 = limit.
        // C API convention: LIMIT=0, MARKET=1.
        var cType = (type === 1) ? 0 : 1;
        __flox_executor_submit(this._h, id, side === "buy" ? 0 : 1, price, qty, cType, symbol || 1);
      }
      onBar(symbol, closePrice) { __flox_executor_on_bar(this._h, symbol, closePrice); }
      onTrade(symbol, price, isBuy) { __flox_executor_on_trade(this._h, symbol, price, isBuy ? 1 : 0); }
      onTradeQty(symbol, price, quantity, isBuy) {
        __flox_executor_on_trade_qty(this._h, symbol, price, quantity, isBuy ? 1 : 0);
      }
      onBestLevels(symbol, bidPrice, bidQty, askPrice, askQty) {
        __flox_executor_on_best_levels(this._h, symbol, bidPrice, bidQty, askPrice, askQty);
      }
      advanceClock(timestampNs) { __flox_executor_advance_clock(this._h, timestampNs); }
      setDefaultSlippage(model, ticks, tickSize, bps, impactCoeff) {
        __flox_executor_set_default_slippage(this._h, _slippageMap[model] || 0,
                                             ticks || 0, tickSize || 0,
                                             bps || 0, impactCoeff || 0);
      }
      setSymbolSlippage(symbol, model, ticks, tickSize, bps, impactCoeff) {
        __flox_executor_set_symbol_slippage(this._h, symbol, _slippageMap[model] || 0,
                                            ticks || 0, tickSize || 0,
                                            bps || 0, impactCoeff || 0);
      }
      setQueueModel(model, depth) {
        __flox_executor_set_queue_model(this._h, _queueMap[model] || 0, depth || 1);
      }
      get fillCount() { return __flox_executor_fill_count(this._h); }
      get handle() { return this._h; }
    }

    class BacktestResult {
      constructor(initialCapital, feeRate, usePercentageFee, fixedFeePerTrade,
                  riskFreeRate, annualizationFactor) {
        this._h = __flox_backtest_result_create(
          initialCapital === undefined ? 100000.0 : initialCapital,
          feeRate === undefined ? 0.0001 : feeRate,
          usePercentageFee === undefined || usePercentageFee ? 1 : 0,
          fixedFeePerTrade || 0,
          riskFreeRate || 0,
          annualizationFactor || 252.0);
      }
      destroy() { __flox_backtest_result_destroy(this._h); }
      recordFill(orderId, symbol, side, price, qty, timestampNs) {
        __flox_backtest_result_record_fill(this._h, orderId, symbol,
                                           side === "buy" ? 0 : 1, price, qty, timestampNs);
      }
      ingestExecutor(executor) {
        __flox_backtest_result_ingest(this._h, executor.handle);
      }
      stats() { return __flox_backtest_result_stats(this._h); }
      equityCurve() { return __flox_backtest_result_equity_curve(this._h); }
      writeEquityCurveCsv(path) { return __flox_backtest_result_write_csv(this._h, path); }
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

    class DataWriter {
      constructor(dir, maxSegmentSize, compression) {
        this._h = __flox_dw_create(dir, maxSegmentSize || 0, compression || 0);
      }
      destroy() { __flox_dw_destroy(this._h); }
      writeTrade(symbolId, timestampNs, exchangeNs, price, qty, isBuy, tradeId, sequenceNo) {
        __flox_dw_write_trade(this._h, symbolId, timestampNs, exchangeNs,
                              price, qty, isBuy ? 1 : 0, tradeId || 0, sequenceNo || 0);
      }
      flush() { __flox_dw_flush(this._h); }
      close() { __flox_dw_close(this._h); }
      stats() { return __flox_dw_stats(this._h); }
    }

    class DataReader {
      constructor(dirOrOpts) {
        if (typeof dirOrOpts === "string") {
          this._h = __flox_dr_create(dirOrOpts);
        } else {
          var o = dirOrOpts || {};
          this._h = __flox_dr_create_filtered(
            o.dir || "",
            o.fromNs || 0,
            o.toNs || 0,
            o.symbols || []
          );
        }
      }
      destroy() { __flox_dr_destroy(this._h); }
      get count() { return __flox_dr_count(this._h); }
      summary() { return __flox_dr_summary(this._h); }
      stats() { return __flox_dr_stats(this._h); }
      readTrades(maxTrades) { return __flox_dr_read_trades(this._h, maxTrades || 0); }
    }

    class DataRecorder {
      constructor(dir, maxSegmentSize, compression) {
        this._h = __flox_recorder_create(dir, maxSegmentSize || 0, compression || 0);
      }
      destroy() { __flox_recorder_destroy(this._h); }
      addSymbol(symbolId, exchange, symbol, tickSize, lotSize, contractSize, takerFee) {
        __flox_recorder_add_symbol(this._h, symbolId, exchange || "", symbol || "",
                                   tickSize || 0.01, lotSize || 1.0,
                                   contractSize || 1.0, takerFee || 0.0);
      }
      start() { __flox_recorder_start(this._h); }
      stop() { __flox_recorder_stop(this._h); }
      flush() { __flox_recorder_flush(this._h); }
      get isRecording() { return __flox_recorder_is_recording(this._h) !== 0; }
    }

    class Partitioner {
      constructor(dataDir) { this._h = __flox_part_create(dataDir); }
      destroy() { __flox_part_destroy(this._h); }
      byTime(numPartitions, warmupNs) {
        return __flox_part_by_time(this._h, numPartitions || 2, warmupNs || 0);
      }
      byDuration(durationNs, warmupNs) {
        return __flox_part_by_duration(this._h, durationNs, warmupNs || 0);
      }
      byCalendar(unit, warmupNs) {
        return __flox_part_by_calendar(this._h, unit || 0, warmupNs || 0);
      }
      bySymbol(symbols) { return __flox_part_by_symbol(this._h, symbols || []); }
      perSymbol() { return __flox_part_per_symbol(this._h); }
      byEventCount(eventsPerPartition) {
        return __flox_part_by_event_count(this._h, eventsPerPartition);
      }
    }

    class SignalBuilder {
      constructor() { this._entries = []; }
      _add(tsMs, side, qty, price, orderType, symbol) {
        this._entries.push({ tsMs, side, qty, price: price || 0, orderType: orderType || 0, symbol: symbol || "" });
      }
      buy(tsMs, qty, symbol) { this._add(tsMs, 0, qty, 0, 0, symbol); }
      sell(tsMs, qty, symbol) { this._add(tsMs, 1, qty, 0, 0, symbol); }
      limitBuy(tsMs, price, qty, symbol) { this._add(tsMs, 0, qty, price, 1, symbol); }
      limitSell(tsMs, price, qty, symbol) { this._add(tsMs, 1, qty, price, 1, symbol); }
      get length() { return this._entries.length; }
      clear() { this._entries = []; }
      sorted() { return this._entries.slice().sort(function(a, b) { return a.tsMs - b.tsMs; }); }
    }

    class Engine {
      constructor(initialCapital, feeRate) {
        this._capital = initialCapital === undefined ? 100000.0 : initialCapital;
        this._feeRate = feeRate === undefined ? 0.0001 : feeRate;
        this._symbols = {};
        this._symbolOrder = [];
      }
      _canon(symbol) { return (!symbol || symbol === "") ? "__default__" : symbol; }
      loadCsv(path, symbol) {
        var key = this._canon(symbol);
        var bars = __flox_load_csv(path);
        if (!this._symbols[key]) { this._symbols[key] = bars; this._symbolOrder.push(key); }
        else { this._symbols[key] = this._symbols[key].concat(bars).sort(function(a,b){return a.ts-b.ts;}); }
      }
      get barCount() {
        var total = 0;
        for (var k in this._symbols) total += this._symbols[k].length;
        return total;
      }
      run(signals) {
        var executor = new SimulatedExecutor();
        var result = new BacktestResult(this._capital, this._feeRate, true);
        var symIds = {};
        var nextId = 1;
        var getSid = function(key) {
          if (!symIds[key]) { symIds[key] = nextId++; }
          return symIds[key];
        };
        var defaultKey = this._symbolOrder.length > 0 ? this._symbolOrder[0] : "__default__";

        // Build merged bar timeline. bar.ts is in ms (safe integer range).
        var merged = [];
        for (var i = 0; i < this._symbolOrder.length; i++) {
          var key = this._symbolOrder[i];
          var bars = this._symbols[key];
          for (var j = 0; j < bars.length; j++) {
            merged.push({ tsMs: bars[j].ts, key: key, bar: bars[j] });
          }
        }
        merged.sort(function(a, b) { return a.tsMs - b.tsMs; });

        var sorted = signals.sorted();
        var sigIdx = 0;
        var orderId = 1;

        for (var mi = 0; mi < merged.length; mi++) {
          var ref = merged[mi];
          var sid = getSid(ref.key);
          // Advance clock (ns) and fill pending orders at this bar's close.
          // Signals are submitted AFTER onBar to match Python Engine semantics.
          executor.advanceClock(ref.tsMs * 1000000);
          executor.onBar(sid, ref.bar.close);
          // Submit signals timestamped at or before this bar (ms comparison)
          while (sigIdx < sorted.length && sorted[sigIdx].tsMs <= ref.tsMs) {
            var sig = sorted[sigIdx];
            var sigKey = this._canon(sig.symbol) in this._symbols ? this._canon(sig.symbol) : defaultKey;
            var ssid = getSid(sigKey);
            executor.submitOrder(orderId, sig.side === 0 ? "buy" : "sell",
                                 sig.price, sig.qty, sig.orderType, ssid);
            orderId++;
            sigIdx++;
          }
        }
        while (sigIdx < sorted.length) {
          var sig = sorted[sigIdx];
          var sigKey = this._canon(sig.symbol) in this._symbols ? this._canon(sig.symbol) : defaultKey;
          var ssid = getSid(sigKey);
          executor.submitOrder(orderId, sig.side === 0 ? "buy" : "sell",
                               sig.price, sig.qty, sig.orderType, ssid);
          orderId++;
          sigIdx++;
        }
        result.ingestExecutor(executor);
        var stats = result.stats();
        result.destroy();
        executor.destroy();
        return stats;
      }
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
      mergeSegments: function(inputDir, outputPath) { return __flox_segment_merge(inputDir, outputPath); },

      // Aggregators — each takes (timestamps, prices, quantities, sides, param)
      timeBars: function(ts, px, qty, sides, intervalNs) {
        return __flox_agg_time(ts, px, qty, sides, intervalNs);
      },
      tickBars: function(ts, px, qty, sides, ticksPerBar) {
        return __flox_agg_tick(ts, px, qty, sides, ticksPerBar);
      },
      volumeBars: function(ts, px, qty, sides, volumePerBar) {
        return __flox_agg_volume(ts, px, qty, sides, volumePerBar);
      },
      rangeBars: function(ts, px, qty, sides, rangeSize) {
        return __flox_agg_range(ts, px, qty, sides, rangeSize);
      },
      renkoBars: function(ts, px, qty, sides, brickSize) {
        return __flox_agg_renko(ts, px, qty, sides, brickSize);
      },
      heikinBars: function(ts, px, qty, sides, intervalNs) {
        return __flox_agg_heikin(ts, px, qty, sides, intervalNs);
      },

      // Extended segment ops
      mergeDir: function(inputDir, outputDir) {
        return __flox_seg_merge_dir(inputDir, outputDir);
      },
      splitSegment: function(inputPath, outputDir, mode, timeIntervalNs, eventsPerFile) {
        return __flox_seg_split(inputPath, outputDir, mode || 0,
                                timeIntervalNs || 0, eventsPerFile || 0);
      },
      exportSegment: function(inputPath, outputPath, format, fromNs, toNs, symbols) {
        return __flox_seg_export(inputPath, outputPath, format || 0,
                                 fromNs || 0, toNs || 0, symbols || []);
      },
      validateSegmentFull: function(path, verifyCrc, verifyTimestamps) {
        return __flox_seg_validate_full(path, verifyCrc ? 1 : 0, verifyTimestamps ? 1 : 0);
      },
      validateDataset: function(dataDir) { return __flox_dataset_validate(dataDir); },
      recompressSegment: function(inputPath, outputPath, level) {
        return __flox_seg_recompress(inputPath, outputPath, level || 0);
      },
      extractSymbols: function(inputPath, outputDir, symbols) {
        return __flox_seg_extract_symbols(inputPath, outputDir, symbols || []);
      },
      extractTimeRange: function(inputPath, outputPath, fromNs, toNs) {
        return __flox_seg_extract_time(inputPath, outputPath, fromNs || 0, toNs || 0);
      },

      loadCsv: function(path) { return __flox_load_csv(path); },

      IndicatorGraph: class {
        constructor() {
          this._h = __flox_graph_create();
        }
        destroy() {
          if (this._h) { __flox_graph_destroy(this._h); this._h = null; }
        }
        setBars(symbol, close, high, low, volume) {
          __flox_graph_set_bars(this._h, symbol, close, high || null,
                                low || null, volume || null);
        }
        addNode(name, deps, fn) {
          __flox_graph_add_node(this._h, name, deps || [], fn, this);
        }
        require(symbol, name) { return __flox_graph_require(this._h, symbol, name); }
        get(symbol, name) { return __flox_graph_get(this._h, symbol, name); }
        close(symbol) { return __flox_graph_close(this._h, symbol); }
        high(symbol) { return __flox_graph_high(this._h, symbol); }
        low(symbol) { return __flox_graph_low(this._h, symbol); }
        volume(symbol) { return __flox_graph_volume(this._h, symbol); }
        invalidate(symbol) { __flox_graph_invalidate(this._h, symbol); }
        invalidateAll() { __flox_graph_invalidate_all(this._h); }
      }
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

  // Get the registered strategy (optional -- scripts can run without one)
  _strategyObj = _engine.getGlobalProperty("__flox_registered_strategy");
  if (!JS_IsNull(_strategyObj) && !JS_IsUndefined(_strategyObj))
  {
    resolveSymbols();
  }
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
