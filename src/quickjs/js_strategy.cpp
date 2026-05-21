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

static const char* const TARGETS_JS =
#include "js_stdlib_targets.inc"
    ;

static const char* const LATENCY_JS =
#include "js_stdlib_latency.inc"
    ;

static const char* const COMPOSITE_JS =
#include "js_stdlib_composite.inc"
    ;

static const char* const ORDER_GROUP_JS =
#include "js_stdlib_order_group.inc"
    ;

static const char* const FEED_CLOCK_JS =
#include "js_stdlib_feed_clock.inc"
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
  if (!_engine.eval(TARGETS_JS, "flox/targets.js"))
  {
    throw std::runtime_error("Failed to load targets.js: " + _engine.getErrorMessage());
  }
  if (!_engine.eval(LATENCY_JS, "flox/latency.js"))
  {
    throw std::runtime_error("Failed to load latency.js: " + _engine.getErrorMessage());
  }
  if (!_engine.eval(COMPOSITE_JS, "flox/composite.js"))
  {
    throw std::runtime_error("Failed to load composite.js: " + _engine.getErrorMessage());
  }
  if (!_engine.eval(ORDER_GROUP_JS, "flox/order_group.js"))
  {
    throw std::runtime_error("Failed to load order_group.js: " + _engine.getErrorMessage());
  }
  if (!_engine.eval(FEED_CLOCK_JS, "flox/feed_clock.js"))
  {
    throw std::runtime_error("Failed to load feed_clock.js: " + _engine.getErrorMessage());
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
      constructor() { this._h = __flox_simulated_executor_create(); }
      destroy() { __flox_simulated_executor_destroy(this._h); }
      submitOrder(id, side, price, qty, type, symbol) {
        // JS convention: type 0 = market (default), 1 = limit.
        // C API convention: LIMIT=0, MARKET=1.
        var cType = (type === 1) ? 0 : 1;
        __flox_simulated_executor_submit(this._h, id, side === "buy" ? 0 : 1, price, qty, cType, symbol || 1);
      }
      onBar(symbol, closePrice) { __flox_simulated_executor_on_bar(this._h, symbol, closePrice); }
      onTrade(symbol, price, isBuy) { __flox_simulated_executor_on_trade(this._h, symbol, price, isBuy ? 1 : 0); }
      onTradeQty(symbol, price, quantity, isBuy) {
        __flox_simulated_executor_on_trade_qty(this._h, symbol, price, quantity, isBuy ? 1 : 0);
      }
      onBestLevels(symbol, bidPrice, bidQty, askPrice, askQty) {
        __flox_simulated_executor_on_best_levels(this._h, symbol, bidPrice, bidQty, askPrice, askQty);
      }
      advanceClock(timestampNs) { __flox_simulated_executor_advance_clock(this._h, timestampNs); }
      setDefaultSlippage(model, ticks, tickSize, bps, impactCoeff) {
        __flox_simulated_executor_set_default_slippage(this._h, _slippageMap[model] || 0,
                                             ticks || 0, tickSize || 0,
                                             bps || 0, impactCoeff || 0);
      }
      setSymbolSlippage(symbol, model, ticks, tickSize, bps, impactCoeff) {
        __flox_simulated_executor_set_symbol_slippage(this._h, symbol, _slippageMap[model] || 0,
                                            ticks || 0, tickSize || 0,
                                            bps || 0, impactCoeff || 0);
      }
      setQueueModel(model, depth) {
        __flox_simulated_executor_set_queue_model(this._h, _queueMap[model] || 0, depth || 1);
      }
      get fillCount() { return __flox_simulated_executor_fill_count(this._h); }
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
      // C ABI order: (exchange_ts_ns, recv_ts_ns, price, qty, trade_id,
      // symbol_id, side). The JS surface keeps the same shape.
      writeTrade(timestampNs, exchangeNs, price, qty, tradeId, symbolId, isBuy) {
        __flox_dw_write_trade(this._h, timestampNs, exchangeNs,
                              price, qty, tradeId || 0,
                              symbolId, isBuy ? 0 : 1);
      }
      // bidsBuf / asksBuf are BigInt64Array buffers laid out [price_raw, qty_raw, ...].
      writeBook(timestampNs, exchangeNs, seqNs, symbolId, isSnapshot, bidsBuf, asksBuf) {
        var nBids = bidsBuf ? (bidsBuf.length >> 1) : 0;
        var nAsks = asksBuf ? (asksBuf.length >> 1) : 0;
        return __flox_dw_write_book(this._h, timestampNs, exchangeNs, seqNs || 0,
                                    symbolId, isSnapshot ? 1 : 0,
                                    bidsBuf || null, nBids,
                                    asksBuf || null, nAsks);
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
      readTradesFrom(startTsNs, maxTrades) { return __flox_dr_read_trades_from(this._h, startTsNs, maxTrades || 0); }
      readBBO(maxEvents) { return __flox_dr_read_bbo(this._h, maxEvents || 0); }
      readBBOFrom(startTsNs, maxEvents) { return __flox_dr_read_bbo_from(this._h, startTsNs, maxEvents || 0); }
      readBookUpdates() { return __flox_dr_read_book_updates(this._h); }
      readBookUpdatesFrom(startTsNs) { return __flox_dr_read_book_updates_from(this._h, startTsNs); }
    }

    class MergedTapeReader {
      // pathsOrOpts: string[] OR { paths: string[], fromNs?: bigint|number, toNs?: bigint|number, symbols?: number[] }
      constructor(pathsOrOpts, opts) {
        var paths, o;
        if (Array.isArray(pathsOrOpts)) {
          paths = pathsOrOpts;
          o = opts || {};
        } else {
          o = pathsOrOpts || {};
          paths = o.paths || [];
        }
        var from = (o.fromNs === undefined || o.fromNs === null) ? -1 : o.fromNs;
        var to   = (o.toNs   === undefined || o.toNs   === null) ? -1 : o.toNs;
        this._h = __flox_mtr_create(paths, from, to, o.symbols || []);
        if (!this._h) {
          throw new Error("MergedTapeReader: failed to open tapes - bad input or overlapping book streams");
        }
      }
      destroy() {
        if (this._h) { __flox_mtr_destroy(this._h); this._h = null; }
      }
      // Unified view of symbols across all tapes, keyed by (exchange, name).
      symbolTable() { return __flox_mtr_get_symbols(this._h); }
      get symbolCount() { return __flox_mtr_symbol_count(this._h); }
      // Per-tape stats: { firstEventNs, lastEventNs, trades, books, path }
      perTapeStats() { return __flox_mtr_get_tape_stats(this._h); }
      get tapeCount() { return __flox_mtr_tape_count(this._h); }
      // { minFirstNs, maxLastNs } — outer time range across all tapes.
      timeRange() { return __flox_mtr_time_range(this._h); }
      countTrades() { return __flox_mtr_count_trades(this._h); }
      readTrades(maxTrades) { return __flox_mtr_read_trades(this._h, maxTrades || 0); }
      // { events, levels } two-phase count for book updates.
      countBooks() { return __flox_mtr_count_books(this._h); }
      readBooks() { return __flox_mtr_read_books(this._h); }
    }

    class BinaryLogRecorderHook {
      // (outputDir, maxSegmentMb, exchangeId, compression, exchangeName?, instrumentType?)
      // exchangeName / instrumentType are stamped into metadata.json so
      // MergedTapeReader can key tapes by (exchange, name).
      constructor(outputDir, maxSegmentMb, exchangeId, compression,
                  exchangeName, instrumentType) {
        var compMap = { none: 0, lz4: 1 };
        var comp = 0;
        if (typeof compression === "number") {
          comp = compression | 0;
        } else if (typeof compression === "string") {
          if (compMap[compression] === undefined) {
            throw new Error("BinaryLogRecorderHook: unknown compression '" + compression + "'. Use 'none' or 'lz4'.");
          }
          comp = compMap[compression];
        }
        this._h = __flox_blrh_create(outputDir, maxSegmentMb === undefined ? 256 : maxSegmentMb,
                                     exchangeId || 0, comp,
                                     exchangeName || "", instrumentType || "");
        this.__floxIsBinaryLogRecorderHook = true;
      }
      destroy() {
        if (this._h) { __flox_blrh_destroy(this._h); this._h = null; }
      }
      addSymbol(symbolId, name, base, quote, pricePrecision, qtyPrecision) {
        __flox_blrh_add_symbol(this._h, symbolId, name || "", base || "", quote || "",
                               pricePrecision === undefined ? 8 : pricePrecision,
                               qtyPrecision === undefined ? 8 : qtyPrecision);
      }
      flush() { __flox_blrh_flush(this._h); }
      stats() { return __flox_blrh_stats(this._h); }
      _asRecorderHandle() { return __flox_blrh_as_recorder(this._h); }
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

      // Forward-looking labels (research-only). See flox/targets.js.
      targets: __floxTargets,

      // Latency models (Phase 1 sampling primitive). See flox/latency.js.
      ConstantLatency: ConstantLatency,
      GaussianLatency: GaussianLatency,
      ExponentialLatency: ExponentialLatency,
      EmpiricalLatency: EmpiricalLatency,

      // Tape diff: replay-equivalence localization. Returns an object
      // matching the Python TapeDiff dataclass shape.
      tapeDiff: function(left, right, opts) {
        return __flox_tape_diff(left, right, opts || {});
      },

      // Delta book compression: encode L2 snapshots as anchor +
      // delta events, replay them back into reconstructed snapshots.
      DeltaBookEncoder: class {
        constructor(opts) {
          var o = opts || {};
          this._h = __flox_delta_book_encoder_create(o.anchorEvery || 100);
        }
        destroy() {
          if (this._h) { __flox_delta_book_encoder_destroy(this._h); this._h = null; }
        }
        encode(symbolId, bids, asks) {
          return __flox_delta_book_encoder_encode(this._h, symbolId, bids || [], asks || []);
        }
      },
      DeltaBookReplayer: class {
        constructor() { this._h = __flox_delta_book_replayer_create(); }
        destroy() {
          if (this._h) { __flox_delta_book_replayer_destroy(this._h); this._h = null; }
        }
        apply(type, symbolId, bids, asks) {
          return __flox_delta_book_replayer_apply(this._h, type, symbolId, bids || [], asks || []);
        }
      },

      // .floxrun strategy-trace recorder + reader.
      TraceRecorder: class {
        constructor(opts) {
          this._h = __flox_run_recorder_create(opts || {});
        }
        destroy() {
          if (this._h) { __flox_run_recorder_destroy(this._h); this._h = null; }
        }
        addTapeRef(opts) { __flox_run_recorder_add_tape_ref(this._h, opts || {}); }
        setRunEndedNs(ns) { __flox_run_recorder_set_run_ended_ns(this._h, ns); }
        writeSignal(opts) { __flox_run_recorder_write_signal(this._h, opts); }
        writeOrderEvent(opts) { __flox_run_recorder_write_order_event(this._h, opts); }
        writeFill(opts) { __flox_run_recorder_write_fill(this._h, opts); }
        close() { __flox_run_recorder_close(this._h); }
      },
      TraceReader: class {
        constructor(path) { this._h = __flox_run_reader_open(path); }
        destroy() {
          if (this._h) { __flox_run_reader_close(this._h); this._h = null; }
        }
        strategyId() { return __flox_run_reader_strategy_id(this._h); }
        runStartedNs() { return __flox_run_reader_run_started_ns(this._h); }
        runEndedNs() { return __flox_run_reader_run_ended_ns(this._h); }
        readAllSignals() { return __flox_run_reader_signals(this._h); }
        readAllOrderEvents() { return __flox_run_reader_orders(this._h); }
        readAllFills() { return __flox_run_reader_fills(this._h); }
      },

      // Cross-binding parity test fixture for bar-close dispatch
      // ordering. Drives a MultiTimeframeAggregator + BarBus through
      // the C ABI; matches the pybind11 / NAPI / Codon surfaces.
      BarDispatchRecorder: class {
        constructor() { this._h = __flox_bar_dispatch_recorder_create(); }
        destroy() {
          if (this._h) { __flox_bar_dispatch_recorder_destroy(this._h); this._h = null; }
        }
        addTimeIntervalSeconds(seconds) {
          return __flox_bar_dispatch_recorder_add_time_seconds(this._h, seconds);
        }
        onTrade(symbol, price, qty, tsNs) {
          __flox_bar_dispatch_recorder_on_trade(this._h, symbol, price, qty, tsNs);
        }
        finalize() { __flox_bar_dispatch_recorder_finalize(this._h); }
        count() { return __flox_bar_dispatch_recorder_count(this._h); }
        typeAt(i) { return __flox_bar_dispatch_recorder_type_at(this._h, i); }
        paramAt(i) { return __flox_bar_dispatch_recorder_param_at(this._h, i); }
      },

      // Execution algos: TWAP / VWAP / Iceberg / POV. The user
      // drives `step(nowNs)` and dispatches the returned child
      // orders to its own executor.
      TWAPExecutor: class {
        constructor(opts) {
          var o = opts || {};
          this._h = __flox_exec_twap_create(
            o.targetQty, (o.side === 'sell') ? 1 : 0, o.symbol || 0,
            (o.type === 'limit') ? 1 : 0, o.limitPrice || 0,
            o.durationNs, o.sliceCount, o.startTimeNs);
        }
        destroy() { if (this._h) { __flox_exec_destroy(this._h); this._h = null; } }
        step(nowNs) { return __flox_exec_step(this._h, nowNs); }
        reportFill(qty) { __flox_exec_report_fill(this._h, qty); }
        submittedQty() { return __flox_exec_submitted_qty(this._h); }
        filledQty() { return __flox_exec_filled_qty(this._h); }
        remainingQty() { return __flox_exec_remaining_qty(this._h); }
        isDone() { return __flox_exec_is_done(this._h); }
      },
      VWAPExecutor: class {
        constructor(opts) {
          var o = opts || {};
          this._h = __flox_exec_vwap_create(
            o.targetQty, (o.side === 'sell') ? 1 : 0, o.symbol || 0,
            (o.type === 'limit') ? 1 : 0, o.limitPrice || 0,
            o.volumeCurve || []);
        }
        destroy() { if (this._h) { __flox_exec_destroy(this._h); this._h = null; } }
        step(nowNs) { return __flox_exec_step(this._h, nowNs); }
        reportFill(qty) { __flox_exec_report_fill(this._h, qty); }
        submittedQty() { return __flox_exec_submitted_qty(this._h); }
        filledQty() { return __flox_exec_filled_qty(this._h); }
        remainingQty() { return __flox_exec_remaining_qty(this._h); }
        isDone() { return __flox_exec_is_done(this._h); }
      },
      IcebergExecutor: class {
        constructor(opts) {
          var o = opts || {};
          this._h = __flox_exec_iceberg_create(
            o.targetQty, (o.side === 'sell') ? 1 : 0, o.symbol || 0,
            (o.type === 'limit') ? 1 : 0, o.limitPrice || 0, o.visibleQty);
        }
        destroy() { if (this._h) { __flox_exec_destroy(this._h); this._h = null; } }
        step(nowNs) { return __flox_exec_step(this._h, nowNs); }
        reportFill(qty) { __flox_exec_report_fill(this._h, qty); }
        submittedQty() { return __flox_exec_submitted_qty(this._h); }
        filledQty() { return __flox_exec_filled_qty(this._h); }
        remainingQty() { return __flox_exec_remaining_qty(this._h); }
        isDone() { return __flox_exec_is_done(this._h); }
      },
      POVExecutor: class {
        constructor(opts) {
          var o = opts || {};
          this._h = __flox_exec_pov_create(
            o.targetQty, (o.side === 'sell') ? 1 : 0, o.symbol || 0,
            (o.type === 'limit') ? 1 : 0, o.limitPrice || 0,
            o.participationRate, o.minSliceQty || 0);
        }
        destroy() { if (this._h) { __flox_exec_destroy(this._h); this._h = null; } }
        step(nowNs) { return __flox_exec_step(this._h, nowNs); }
        reportFill(qty) { __flox_exec_report_fill(this._h, qty); }
        observeVolume(qty) { __flox_exec_observe_volume(this._h, qty); }
        submittedQty() { return __flox_exec_submitted_qty(this._h); }
        filledQty() { return __flox_exec_filled_qty(this._h); }
        remainingQty() { return __flox_exec_remaining_qty(this._h); }
        isDone() { return __flox_exec_is_done(this._h); }
      },

      // Portfolio risk aggregator. C ABI handle-based; JS class
      // wraps the lifecycle.
      PortfolioRiskAggregator: class {
        constructor(opts) {
          var o = opts || {};
          this._h = __flox_portfolio_risk_create(o.rules || {}, o.initialEquity || 0);
        }
        destroy() {
          if (this._h) { __flox_portfolio_risk_destroy(this._h); this._h = null; }
        }
        update(name, fields) {
          __flox_portfolio_risk_update(this._h, name, fields || {});
        }
        remove(name) { __flox_portfolio_risk_remove(this._h, name); }
        resetKillSwitch() { __flox_portfolio_risk_reset(this._h); }
        checkOrder(strategy, notional, side) {
          return __flox_portfolio_risk_check_order(this._h, strategy, notional, side);
        }
        snapshot() { return __flox_portfolio_risk_snapshot(this._h); }
      },

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
      },

      StreamingIndicatorGraph: class {
        constructor() {
          this._h = __flox_streaming_create();
        }
        destroy() {
          if (this._h) { __flox_streaming_destroy(this._h); this._h = null; }
        }
        addNode(name, deps, fn) {
          __flox_streaming_add_node(this._h, name, deps || [], fn, this);
        }
        step(symbol, close, high, low, volume) {
          __flox_streaming_step(this._h, symbol, close,
                                high !== undefined ? high : null,
                                low !== undefined ? low : null,
                                volume !== undefined ? volume : null);
        }
        current(symbol, name) { return __flox_streaming_current(this._h, symbol, name); }
        barCount(symbol) { return __flox_streaming_bar_count(this._h, symbol); }
        reset(symbol) { __flox_streaming_reset(this._h, symbol); }
        resetAll() { __flox_streaming_reset_all(this._h); }
        close(symbol) { return __flox_streaming_close(this._h, symbol); }
        high(symbol) { return __flox_streaming_high(this._h, symbol); }
        low(symbol) { return __flox_streaming_low(this._h, symbol); }
        volume(symbol) { return __flox_streaming_volume(this._h, symbol); }
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
  cb.on_bar = FloxJsStrategy::onBar;
  cb.on_start = FloxJsStrategy::onStart;
  cb.on_stop = FloxJsStrategy::onStop;
  cb.on_fill = FloxJsStrategy::onFill;
  cb.on_order_update = FloxJsStrategy::onOrderUpdate;
  cb.on_queue_position_change = FloxJsStrategy::onQueuePositionChange;
  cb.on_market_position_change = FloxJsStrategy::onMarketPositionChange;
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

void FloxJsStrategy::onBar(void* userData, const FloxSymbolContext* ctx,
                           const FloxBarData* bar)
{
  auto* self = static_cast<FloxJsStrategy*>(userData);
  auto* jsCtx = self->_engine.context();

  JSValue ctxObj = self->makeCtxObject(ctx);
  JSValue barObj = self->makeBarObject(bar);

  JSValue method = JS_GetPropertyStr(jsCtx, self->_strategyObj, "_dispatchBar");
  if (JS_IsFunction(jsCtx, method))
  {
    JSValue args[2] = {ctxObj, barObj};
    JSValue ret = JS_Call(jsCtx, method, self->_strategyObj, 2, args);
    if (JS_IsException(ret))
    {
      std::cerr << "[flox-js] Error in onBar: " << self->_engine.getErrorMessage() << std::endl;
    }
    JS_FreeValue(jsCtx, ret);
  }
  JS_FreeValue(jsCtx, method);
  JS_FreeValue(jsCtx, ctxObj);
  JS_FreeValue(jsCtx, barObj);
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

JSValue FloxJsStrategy::makeBarObject(const FloxBarData* bar)
{
  auto* c = _engine.context();
  JSValue obj = JS_NewObject(c);
  JS_SetPropertyStr(c, obj, "symbolId", JS_NewUint32(c, bar->symbol));
  JS_SetPropertyStr(c, obj, "barType", JS_NewUint32(c, bar->bar_type));
  JS_SetPropertyStr(c, obj, "barTypeParam",
                    JS_NewFloat64(c, static_cast<double>(bar->bar_type_param)));
  JS_SetPropertyStr(c, obj, "open", JS_NewFloat64(c, flox_price_to_double(bar->open_raw)));
  JS_SetPropertyStr(c, obj, "high", JS_NewFloat64(c, flox_price_to_double(bar->high_raw)));
  JS_SetPropertyStr(c, obj, "low", JS_NewFloat64(c, flox_price_to_double(bar->low_raw)));
  JS_SetPropertyStr(c, obj, "close", JS_NewFloat64(c, flox_price_to_double(bar->close_raw)));
  JS_SetPropertyStr(c, obj, "volume",
                    JS_NewFloat64(c, flox_quantity_to_double(bar->volume_raw)));
  JS_SetPropertyStr(c, obj, "buyVolume",
                    JS_NewFloat64(c, flox_quantity_to_double(bar->buy_volume_raw)));
  JS_SetPropertyStr(c, obj, "startTimeNs",
                    JS_NewFloat64(c, static_cast<double>(bar->start_time_ns)));
  JS_SetPropertyStr(c, obj, "endTimeNs",
                    JS_NewFloat64(c, static_cast<double>(bar->end_time_ns)));
  JS_SetPropertyStr(c, obj, "closeReason", JS_NewUint32(c, bar->close_reason));
  return obj;
}

namespace
{
const char* jsOrderEventStatusName(uint8_t s)
{
  switch (s)
  {
    case 0:
      return "NEW";
    case 1:
      return "ACCEPTED";
    case 2:
      return "PENDING_NEW";
    case 3:
      return "PARTIALLY_FILLED";
    case 4:
      return "FILLED";
    case 5:
      return "PENDING_CANCEL";
    case 6:
      return "CANCELED";
    case 7:
      return "EXPIRED";
    case 8:
      return "REJECTED";
    case 9:
      return "REPLACED";
    case 10:
      return "PENDING_TRIGGER";
    case 11:
      return "TRIGGERED";
    case 12:
      return "TRAILING_UPDATED";
    case 13:
      return "QUEUE_POSITION_UPDATED";
    case 14:
      return "MARKET_POSITION_CHANGED";
    case 15:
      return "REPLACE_SUBMITTED";
    case 16:
      return "REPLACE_ACCEPTED";
    case 17:
      return "REPLACE_REJECTED";
    default:
      return "UNKNOWN";
  }
}
const char* jsMarketPositionName(uint8_t p)
{
  switch (p)
  {
    case 1:
      return "best";
    case 2:
      return "behind_best";
    case 3:
      return "mid_spread";
    case 4:
      return "level_empty";
    case 5:
      return "crossed";
    default:
      return "";
  }
}
const char* jsOrderTypeName(uint8_t t)
{
  switch (t)
  {
    case 0:
      return "LIMIT";
    case 1:
      return "MARKET";
    case 2:
      return "STOP_MARKET";
    case 3:
      return "STOP_LIMIT";
    case 4:
      return "TP_MARKET";
    case 5:
      return "TP_LIMIT";
    case 6:
      return "ICEBERG";
    default:
      return "UNKNOWN";
  }
}
}  // namespace

JSValue FloxJsStrategy::makeOrderEventObject(const FloxOrderEventData* ev)
{
  auto* c = _engine.context();
  JSValue obj = JS_NewObject(c);
  JS_SetPropertyStr(c, obj, "orderId",
                    JS_NewFloat64(c, static_cast<double>(ev->order_id)));
  JS_SetPropertyStr(c, obj, "symbolId", JS_NewUint32(c, ev->symbol_id));
  JS_SetPropertyStr(c, obj, "side",
                    JS_NewString(c, ev->side == 0 ? "buy" : "sell"));
  JS_SetPropertyStr(c, obj, "orderType",
                    JS_NewString(c, jsOrderTypeName(ev->order_type)));
  JS_SetPropertyStr(c, obj, "status",
                    JS_NewString(c, jsOrderEventStatusName(ev->status)));
  JS_SetPropertyStr(c, obj, "fillQty",
                    JS_NewFloat64(c, flox_quantity_to_double(ev->fill_qty_raw)));
  JS_SetPropertyStr(c, obj, "fillPrice",
                    JS_NewFloat64(c, flox_price_to_double(ev->fill_price_raw)));
  JS_SetPropertyStr(c, obj, "exchangeTsNs",
                    JS_NewFloat64(c, static_cast<double>(ev->exchange_ts_ns)));
  if (ev->reject_reason)
  {
    JS_SetPropertyStr(c, obj, "rejectReason", JS_NewString(c, ev->reject_reason));
  }
  else
  {
    JS_SetPropertyStr(c, obj, "rejectReason", JS_NULL);
  }
  JS_SetPropertyStr(c, obj, "queueAhead",
                    JS_NewFloat64(c, flox_quantity_to_double(ev->queue_ahead_raw)));
  JS_SetPropertyStr(c, obj, "queueTotal",
                    JS_NewFloat64(c, flox_quantity_to_double(ev->queue_total_raw)));
  const char* mpName = jsMarketPositionName(ev->market_position);
  if (mpName[0] != '\0')
  {
    JS_SetPropertyStr(c, obj, "marketPosition", JS_NewString(c, mpName));
  }
  else
  {
    JS_SetPropertyStr(c, obj, "marketPosition", JS_NULL);
  }
  JS_SetPropertyStr(c, obj, "distanceToBestTicks",
                    JS_NewInt32(c, ev->distance_to_best_ticks));
  JS_SetPropertyStr(c, obj, "submittedAtNs",
                    JS_NewFloat64(c, static_cast<double>(ev->submitted_at_ns)));
  JS_SetPropertyStr(c, obj, "acceptedAtNs",
                    JS_NewFloat64(c, static_cast<double>(ev->accepted_at_ns)));
  JS_SetPropertyStr(c, obj, "firstFillAtNs",
                    JS_NewFloat64(c, static_cast<double>(ev->first_fill_at_ns)));
  JS_SetPropertyStr(c, obj, "lastFillAtNs",
                    JS_NewFloat64(c, static_cast<double>(ev->last_fill_at_ns)));
  JS_SetPropertyStr(c, obj, "canceledAtNs",
                    JS_NewFloat64(c, static_cast<double>(ev->canceled_at_ns)));
  JS_SetPropertyStr(c, obj, "rejectedAtNs",
                    JS_NewFloat64(c, static_cast<double>(ev->rejected_at_ns)));
  JS_SetPropertyStr(c, obj, "triggeredAtNs",
                    JS_NewFloat64(c, static_cast<double>(ev->triggered_at_ns)));
  JS_SetPropertyStr(c, obj, "expiredAtNs",
                    JS_NewFloat64(c, static_cast<double>(ev->expired_at_ns)));
  const bool isMaker = ev->is_maker != 0;
  JS_SetPropertyStr(c, obj, "isMaker", JS_NewBool(c, isMaker));
  const bool isFill = (ev->status == 3 /* PARTIALLY_FILLED */ ||
                       ev->status == 4 /* FILLED */);
  if (isFill)
  {
    JS_SetPropertyStr(c, obj, "fillRole",
                      JS_NewString(c, isMaker ? "maker" : "taker"));
  }
  else
  {
    JS_SetPropertyStr(c, obj, "fillRole", JS_NULL);
  }
  return obj;
}

void FloxJsStrategy::onFill(void* userData, const FloxSymbolContext* ctx,
                            const FloxOrderEventData* ev)
{
  auto* self = static_cast<FloxJsStrategy*>(userData);
  auto* jsCtx = self->_engine.context();
  JSValue ctxObj = self->makeCtxObject(ctx);
  JSValue evObj = self->makeOrderEventObject(ev);
  JSValue method = JS_GetPropertyStr(jsCtx, self->_strategyObj, "_dispatchFill");
  if (JS_IsFunction(jsCtx, method))
  {
    JSValue args[2] = {ctxObj, evObj};
    JSValue ret = JS_Call(jsCtx, method, self->_strategyObj, 2, args);
    if (JS_IsException(ret))
    {
      std::cerr << "[flox-js] Error in onFill: "
                << self->_engine.getErrorMessage() << std::endl;
    }
    JS_FreeValue(jsCtx, ret);
  }
  JS_FreeValue(jsCtx, method);
  JS_FreeValue(jsCtx, ctxObj);
  JS_FreeValue(jsCtx, evObj);
}

void FloxJsStrategy::onOrderUpdate(void* userData, const FloxSymbolContext* ctx,
                                   const FloxOrderEventData* ev)
{
  auto* self = static_cast<FloxJsStrategy*>(userData);
  auto* jsCtx = self->_engine.context();
  JSValue ctxObj = self->makeCtxObject(ctx);
  JSValue evObj = self->makeOrderEventObject(ev);
  JSValue method = JS_GetPropertyStr(jsCtx, self->_strategyObj, "_dispatchOrderUpdate");
  if (JS_IsFunction(jsCtx, method))
  {
    JSValue args[2] = {ctxObj, evObj};
    JSValue ret = JS_Call(jsCtx, method, self->_strategyObj, 2, args);
    if (JS_IsException(ret))
    {
      std::cerr << "[flox-js] Error in onOrderUpdate: "
                << self->_engine.getErrorMessage() << std::endl;
    }
    JS_FreeValue(jsCtx, ret);
  }
  JS_FreeValue(jsCtx, method);
  JS_FreeValue(jsCtx, ctxObj);
  JS_FreeValue(jsCtx, evObj);
}

void FloxJsStrategy::onQueuePositionChange(void* userData, const FloxSymbolContext* ctx,
                                           const FloxOrderEventData* ev)
{
  auto* self = static_cast<FloxJsStrategy*>(userData);
  auto* jsCtx = self->_engine.context();
  JSValue ctxObj = self->makeCtxObject(ctx);
  JSValue evObj = self->makeOrderEventObject(ev);
  JSValue method =
      JS_GetPropertyStr(jsCtx, self->_strategyObj, "_dispatchQueuePositionChange");
  if (JS_IsFunction(jsCtx, method))
  {
    JSValue args[2] = {ctxObj, evObj};
    JSValue ret = JS_Call(jsCtx, method, self->_strategyObj, 2, args);
    if (JS_IsException(ret))
    {
      std::cerr << "[flox-js] Error in onQueuePositionChange: "
                << self->_engine.getErrorMessage() << std::endl;
    }
    JS_FreeValue(jsCtx, ret);
  }
  JS_FreeValue(jsCtx, method);
  JS_FreeValue(jsCtx, ctxObj);
  JS_FreeValue(jsCtx, evObj);
}

void FloxJsStrategy::onMarketPositionChange(void* userData,
                                            const FloxSymbolContext* ctx,
                                            const FloxOrderEventData* ev)
{
  auto* self = static_cast<FloxJsStrategy*>(userData);
  auto* jsCtx = self->_engine.context();
  JSValue ctxObj = self->makeCtxObject(ctx);
  JSValue evObj = self->makeOrderEventObject(ev);
  JSValue method =
      JS_GetPropertyStr(jsCtx, self->_strategyObj, "_dispatchMarketPositionChange");
  if (JS_IsFunction(jsCtx, method))
  {
    JSValue args[2] = {ctxObj, evObj};
    JSValue ret = JS_Call(jsCtx, method, self->_strategyObj, 2, args);
    if (JS_IsException(ret))
    {
      std::cerr << "[flox-js] Error in onMarketPositionChange: "
                << self->_engine.getErrorMessage() << std::endl;
    }
    JS_FreeValue(jsCtx, ret);
  }
  JS_FreeValue(jsCtx, method);
  JS_FreeValue(jsCtx, ctxObj);
  JS_FreeValue(jsCtx, evObj);
}

}  // namespace flox
