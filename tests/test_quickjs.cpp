#include "js_bindings.h"
#include "js_engine.h"
#include "js_strategy.h"

#include "flox/capi/bridge_strategy.h"

#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace flox;

// ============================================================
// Unit tests — engine basics
// ============================================================

TEST(JsEngineTest, CreateAndEval)
{
  FloxJsEngine engine;
  EXPECT_TRUE(engine.eval("var x = 1 + 2;"));
}

TEST(JsEngineTest, EvalError)
{
  FloxJsEngine engine;
  EXPECT_FALSE(engine.eval("throw new Error('test error');"));
  auto msg = engine.getErrorMessage();
  EXPECT_TRUE(msg.find("test error") != std::string::npos);
}

TEST(JsEngineTest, GlobalProperty)
{
  FloxJsEngine engine;
  EXPECT_TRUE(engine.eval("var testVal = 42;"));

  JSValue val = engine.getGlobalProperty("testVal");
  int32_t result = 0;
  JS_ToInt32(engine.context(), &result, val);
  EXPECT_EQ(result, 42);
  JS_FreeValue(engine.context(), val);
}

TEST(JsEngineTest, BindingsRegister)
{
  FloxJsEngine engine;
  registerFloxBindings(engine.context());

  JSValue fn = engine.getGlobalProperty("__flox_emit_market_buy");
  EXPECT_TRUE(JS_IsFunction(engine.context(), fn));
  JS_FreeValue(engine.context(), fn);

  // console.log should exist
  JSValue console = engine.getGlobalProperty("console");
  EXPECT_FALSE(JS_IsUndefined(console));
  JS_FreeValue(engine.context(), console);
}

TEST(JsEngineTest, SMA)
{
  FloxJsEngine engine;
  EXPECT_TRUE(engine.eval(R"(
    class SMA {
      constructor(period) { this._period = period; this._count = 0; this._sum = 0; this._value = 0; this._buffer = []; this._index = 0; }
      update(v) { if (this._count < this._period) { this._buffer.push(v); this._sum += v; this._count++; } else { this._sum -= this._buffer[this._index]; this._buffer[this._index] = v; this._sum += v; this._index = (this._index + 1) % this._period; } this._value = this._sum / this._count; return this._value; }
      get value() { return this._value; }
      get ready() { return this._count >= this._period; }
    }
    var sma = new SMA(3);
    sma.update(10); sma.update(20); sma.update(30);
    var ready = sma.ready;
    var avg = sma.value;
  )"));

  JSValue avg = engine.getGlobalProperty("avg");
  double avgVal = 0;
  JS_ToFloat64(engine.context(), &avgVal, avg);
  EXPECT_DOUBLE_EQ(avgVal, 20.0);
  JS_FreeValue(engine.context(), avg);

  JSValue ready = engine.getGlobalProperty("ready");
  EXPECT_TRUE(JS_ToBool(engine.context(), ready));
  JS_FreeValue(engine.context(), ready);
}

// ============================================================
// Helper: write a temp JS file
// ============================================================

class TempJsFile
{
 public:
  TempJsFile(const std::string& content)
  {
    _path = std::filesystem::temp_directory_path() / ("flox_test_" + std::to_string(counter_++) + ".js");
    std::ofstream f(_path);
    f << content;
  }
  ~TempJsFile() { std::filesystem::remove(_path); }
  std::string path() const { return _path.string(); }
  std::filesystem::path _path;
  static int counter_;
};
int TempJsFile::counter_ = 0;

// ============================================================
// Integration tests — full strategy lifecycle
// ============================================================

TEST(JsIntegrationTest, TargetsBindings)
{
  TempJsFile script(R"(
    var fr = flox.targets.future_return([100.0, 101.0, 99.0, 105.0, 110.0], 2);
    var fr0 = fr[0];
    var fr2 = fr[2];
    var fr_tail_nan = isNaN(fr[3]) && isNaN(fr[4]);

    var constClose = [];
    for (var i = 0; i < 20; i++) constClose.push(100.0);
    var vol = flox.targets.future_ctc_volatility(constClose, 5);
    var vol0 = vol[0];

    var lin = [];
    for (var i = 0; i < 20; i++) lin.push(100.0 + 0.5 * i);
    var sl = flox.targets.future_linear_slope(lin, 4);
    var sl0 = sl[0];
  )");

  SymbolRegistry registry;
  FloxJsStrategy jsStrat(script.path(), registry);

  auto getNum = [&](const char* name)
  {
    JSValue v = jsStrat.engine().getGlobalProperty(name);
    double d = 0;
    JS_ToFloat64(jsStrat.engine().context(), &d, v);
    JS_FreeValue(jsStrat.engine().context(), v);
    return d;
  };

  EXPECT_NEAR(getNum("fr0"), 99.0 / 100.0 - 1.0, 1e-12);
  EXPECT_NEAR(getNum("fr2"), 110.0 / 99.0 - 1.0, 1e-12);
  EXPECT_NEAR(getNum("vol0"), 0.0, 1e-12);
  EXPECT_NEAR(getNum("sl0"), 0.5, 1e-12);

  JSValue tail = jsStrat.engine().getGlobalProperty("fr_tail_nan");
  EXPECT_TRUE(JS_ToBool(jsStrat.engine().context(), tail));
  JS_FreeValue(jsStrat.engine().context(), tail);
}

TEST(JsEngineTest, AdfBindingExposed)
{
  FloxJsEngine engine;
  registerFloxBindings(engine.context());

  // Build a deterministic random walk and call __flox_indicator_adf.
  EXPECT_TRUE(engine.eval(R"(
    var n = 200;
    var seed = 42;
    function rand() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return ((seed + 1) / 0x80000000); }
    function gauss() {
      var u1 = rand();
      var u2 = rand();
      return Math.sqrt(-2 * Math.log(u1)) * Math.cos(2 * Math.PI * u2);
    }
    var walk = [0];
    for (var i = 1; i < n; ++i) walk.push(walk[i-1] + gauss());
    var r = __flox_indicator_adf(walk, 4, "c");
    var test_stat = r.test_stat;
    var p_value = r.p_value;
    var used_lag = r.used_lag;
  )"));

  JSValue ts = engine.getGlobalProperty("test_stat");
  double tsVal = 0;
  JS_ToFloat64(engine.context(), &tsVal, ts);
  JS_FreeValue(engine.context(), ts);
  EXPECT_TRUE(std::isfinite(tsVal));

  JSValue ul = engine.getGlobalProperty("used_lag");
  uint32_t ulVal = 0;
  JS_ToUint32(engine.context(), &ulVal, ul);
  JS_FreeValue(engine.context(), ul);
  EXPECT_LE(ulVal, 4u);
}

TEST(JsIntegrationTest, AutoCorrelationBindings)
{
  TempJsFile script(R"(
    var linear = [];
    for (var i = 0; i < 50; ++i) linear.push(5.0 + 0.7 * i);

    // Batch: AutoCorrelation.compute / __flox_indicator_autocorrelation.
    var ac = AutoCorrelation.compute(linear, 10, 1);
    var batch_at_10 = ac[10];
    var warmup_nan = isNaN(ac[9]);

    // Streaming class.
    var stream = new AutoCorrelation(10, 1);
    var lastStream = NaN;
    for (var j = 0; j < linear.length; ++j) {
      lastStream = stream.update(linear[j]);
    }
    var stream_eq_batch = Math.abs(lastStream - ac[ac.length - 1]) < 1e-9;
  )");

  SymbolRegistry registry;
  FloxJsStrategy jsStrat(script.path(), registry);

  auto* ctx = jsStrat.engine().context();

  JSValue v = jsStrat.engine().getGlobalProperty("batch_at_10");
  double d = 0;
  JS_ToFloat64(ctx, &d, v);
  JS_FreeValue(ctx, v);
  EXPECT_NEAR(d, 1.0, 1e-10);

  JSValue w = jsStrat.engine().getGlobalProperty("warmup_nan");
  EXPECT_TRUE(JS_ToBool(ctx, w));
  JS_FreeValue(ctx, w);

  JSValue eq = jsStrat.engine().getGlobalProperty("stream_eq_batch");
  EXPECT_TRUE(JS_ToBool(ctx, eq));
  JS_FreeValue(ctx, eq);
}

TEST(JsIntegrationTest, IndicatorGraphBindings)
{
  TempJsFile script(R"(
    var ramp = [];
    for (var i = 0; i < 50; ++i) ramp.push(i);

    var g = new flox.IndicatorGraph();
    g.setBars(0, ramp);

    g.addNode("ema5", [], function(graph, sym) {
      return SMA.compute(graph.close(sym), 5);
    });
    g.addNode("sma5", [], function(graph, sym) {
      return SMA.compute(graph.close(sym), 5);
    });
    g.addNode("diff", ["ema5", "sma5"], function(graph, sym) {
      var a = graph.get(sym, "ema5");
      var b = graph.get(sym, "sma5");
      var out = [];
      for (var i = 0; i < a.length; ++i) out.push(a[i] - b[i]);
      return out;
    });

    var ema5 = g.require(0, "ema5");
    var diff = g.require(0, "diff");
    var ema5_len = ema5.length;
    var diff_len = diff.length;
    var sma5_cached = g.get(0, "sma5");
    var sma5_not_null = sma5_cached !== null;

    var threw = false;
    try { g.require(0, "missing"); } catch (e) { threw = true; }
    var require_throws = threw;

    g.destroy();
  )");

  SymbolRegistry registry;
  FloxJsStrategy jsStrat(script.path(), registry);

  auto* ctx = jsStrat.engine().context();

  auto getInt = [&](const char* name)
  {
    JSValue v = jsStrat.engine().getGlobalProperty(name);
    int32_t i = 0;
    JS_ToInt32(ctx, &i, v);
    JS_FreeValue(ctx, v);
    return i;
  };
  auto getBool = [&](const char* name)
  {
    JSValue v = jsStrat.engine().getGlobalProperty(name);
    bool b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
  };

  EXPECT_EQ(getInt("ema5_len"), 50);
  EXPECT_EQ(getInt("diff_len"), 50);
  EXPECT_TRUE(getBool("sma5_not_null"));
  EXPECT_TRUE(getBool("require_throws"));
}

TEST(JsIntegrationTest, StreamingIndicatorGraph)
{
  TempJsFile script(R"(
    var sg = new flox.StreamingIndicatorGraph();
    sg.addNode("double_close", [], function(graph, sym) {
      var c = graph.close(sym);
      var out = [];
      for (var i = 0; i < c.length; ++i) out.push(c[i] * 2.0);
      return out;
    });

    var closes = [10.0, 20.0, 30.0, 40.0, 50.0];
    var lastDouble = 0;
    var barCounts = [];
    for (var i = 0; i < closes.length; ++i) {
      sg.step(0, closes[i]);
      lastDouble = sg.current(0, "double_close");
      barCounts.push(sg.barCount(0));
    }

    // After 5 steps: current == last close * 2, bar counts 1..5.
    var ok_last = Math.abs(lastDouble - 100.0) < 1e-9;
    var ok_counts = barCounts[0] === 1 && barCounts[4] === 5;

    // Parity: batch on same data should match.
    var bg = new flox.IndicatorGraph();
    bg.setBars(0, new Float64Array(closes));
    bg.addNode("double_close", [], function(graph, sym) {
      var c = graph.close(sym);
      var out = [];
      for (var i = 0; i < c.length; ++i) out.push(c[i] * 2.0);
      return out;
    });
    var batchOut = bg.require(0, "double_close");
    var parity = Math.abs(batchOut[batchOut.length - 1] - lastDouble) < 1e-9;
    bg.destroy();

    // Reset and verify bar count resets.
    sg.reset(0);
    var after_reset_count = sg.barCount(0);
    var after_reset_nan = isNaN(sg.current(0, "double_close"));
    sg.destroy();
  )");

  SymbolRegistry registry;
  FloxJsStrategy jsStrat(script.path(), registry);
  auto* ctx = jsStrat.engine().context();

  auto getBool = [&](const char* name)
  {
    JSValue v = jsStrat.engine().getGlobalProperty(name);
    bool b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
  };
  auto getInt = [&](const char* name)
  {
    JSValue v = jsStrat.engine().getGlobalProperty(name);
    int32_t i = 0;
    JS_ToInt32(ctx, &i, v);
    JS_FreeValue(ctx, v);
    return i;
  };

  EXPECT_TRUE(getBool("ok_last"));
  EXPECT_TRUE(getBool("ok_counts"));
  EXPECT_TRUE(getBool("parity"));
  EXPECT_EQ(getInt("after_reset_count"), 0);
  EXPECT_TRUE(getBool("after_reset_nan"));
}

TEST(JsIntegrationTest, LoadStrategyAndResolveSymbols)
{
  TempJsFile script(R"(
    class TestStrat extends Strategy {
      constructor() {
        super({ exchange: "Binance", symbols: ["BTCUSDT", "ETHUSDT"] });
      }
    }
    flox.register(new TestStrat());
  )");

  SymbolRegistry registry;
  FloxJsStrategy jsStrat(script.path(), registry);

  EXPECT_EQ(jsStrat.symbolIds().size(), 2u);
  // Symbols should have been registered in the registry
  EXPECT_EQ(registry.size(), 2u);

  auto btcId = registry.getSymbolId("Binance", "BTCUSDT");
  ASSERT_TRUE(btcId.has_value());
  EXPECT_EQ(jsStrat.symbolIds()[0], btcId.value());

  auto ethId = registry.getSymbolId("Binance", "ETHUSDT");
  ASSERT_TRUE(ethId.has_value());
  EXPECT_EQ(jsStrat.symbolIds()[1], ethId.value());
}

TEST(JsIntegrationTest, OnStartOnStopCalled)
{
  TempJsFile script(R"(
    var startCalled = false;
    var stopCalled = false;
    class TestStrat extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["SYM1"] }); }
      onStart() { startCalled = true; }
      onStop()  { stopCalled = true; }
    }
    flox.register(new TestStrat());
  )");

  SymbolRegistry registry;
  FloxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<FloxStrategyHandle>(bridge.get()));

  bridge->start();
  bridge->stop();

  JSValue started = jsStrat.engine().getGlobalProperty("startCalled");
  EXPECT_TRUE(JS_ToBool(jsStrat.engine().context(), started));
  JS_FreeValue(jsStrat.engine().context(), started);

  JSValue stopped = jsStrat.engine().getGlobalProperty("stopCalled");
  EXPECT_TRUE(JS_ToBool(jsStrat.engine().context(), stopped));
  JS_FreeValue(jsStrat.engine().context(), stopped);
}

TEST(JsIntegrationTest, OnTradeReceivesCorrectData)
{
  TempJsFile script(R"(
    var lastSymbol = "";
    var lastPrice = 0;
    var lastSide = "";
    var tradeCount = 0;

    class TestStrat extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["BTCUSDT"] }); }
      onTrade(ctx, trade) {
        lastSymbol = trade.symbol;
        lastPrice = trade.price;
        lastSide = trade.side;
        tradeCount++;
      }
    }
    flox.register(new TestStrat());
  )");

  SymbolRegistry registry;
  FloxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<FloxStrategyHandle>(bridge.get()));

  // Simulate a trade event through the C callback
  FloxSymbolContext fctx{};
  fctx.symbol_id = symIds[0];

  FloxTradeData ftrade{};
  ftrade.symbol = symIds[0];
  ftrade.price_raw = flox_price_from_double(50123.45);
  ftrade.quantity_raw = flox_quantity_from_double(1.5);
  ftrade.is_buy = 1;
  ftrade.exchange_ts_ns = 1000000000;

  callbacks.on_trade(callbacks.user_data, &fctx, &ftrade);

  auto* ctx = jsStrat.engine().context();

  JSValue count = jsStrat.engine().getGlobalProperty("tradeCount");
  int32_t countVal = 0;
  JS_ToInt32(ctx, &countVal, count);
  EXPECT_EQ(countVal, 1);
  JS_FreeValue(ctx, count);

  JSValue sym = jsStrat.engine().getGlobalProperty("lastSymbol");
  const char* symStr = JS_ToCString(ctx, sym);
  EXPECT_STREQ(symStr, "BTCUSDT");
  JS_FreeCString(ctx, symStr);
  JS_FreeValue(ctx, sym);

  JSValue price = jsStrat.engine().getGlobalProperty("lastPrice");
  double priceVal = 0;
  JS_ToFloat64(ctx, &priceVal, price);
  EXPECT_NEAR(priceVal, 50123.45, 0.01);
  JS_FreeValue(ctx, price);

  JSValue side = jsStrat.engine().getGlobalProperty("lastSide");
  const char* sideStr = JS_ToCString(ctx, side);
  EXPECT_STREQ(sideStr, "buy");
  JS_FreeCString(ctx, sideStr);
  JS_FreeValue(ctx, side);
}

TEST(JsIntegrationTest, MultipleTrades)
{
  TempJsFile script(R"(
    var tradeCount = 0;

    class TestStrat extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["SYM1"] }); }
      onTrade(ctx, trade) { tradeCount++; }
    }
    flox.register(new TestStrat());
  )");

  SymbolRegistry registry;
  FloxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<FloxStrategyHandle>(bridge.get()));

  // Feed 100 trades
  for (int i = 0; i < 100; i++)
  {
    FloxSymbolContext fctx{};
    fctx.symbol_id = symIds[0];
    FloxTradeData ftrade{};
    ftrade.symbol = symIds[0];
    ftrade.price_raw = flox_price_from_double(100.0 + i);
    ftrade.quantity_raw = flox_quantity_from_double(1.0);
    ftrade.is_buy = (i % 2 == 0) ? 1 : 0;
    callbacks.on_trade(callbacks.user_data, &fctx, &ftrade);
  }

  JSValue count = jsStrat.engine().getGlobalProperty("tradeCount");
  int32_t countVal = 0;
  JS_ToInt32(jsStrat.engine().context(), &countVal, count);
  EXPECT_EQ(countVal, 100);
  JS_FreeValue(jsStrat.engine().context(), count);
}

TEST(JsIntegrationTest, QualifiedSymbolFormat)
{
  TempJsFile script(R"(
    class TestStrat extends Strategy {
      constructor() {
        super({ symbols: ["Binance:BTCUSDT", "Bybit:ETHUSDT"] });
      }
    }
    flox.register(new TestStrat());
  )");

  SymbolRegistry registry;
  FloxJsStrategy jsStrat(script.path(), registry);

  EXPECT_EQ(jsStrat.symbolIds().size(), 2u);
  auto btcId = registry.getSymbolId("Binance", "BTCUSDT");
  ASSERT_TRUE(btcId.has_value());
  auto ethId = registry.getSymbolId("Bybit", "ETHUSDT");
  ASSERT_TRUE(ethId.has_value());
}

TEST(JsIntegrationTest, NoExchangeThrows)
{
  TempJsFile script(R"(
    class TestStrat extends Strategy {
      constructor() { super({ symbols: ["BTCUSDT"] }); }
    }
    flox.register(new TestStrat());
  )");

  SymbolRegistry registry;
  EXPECT_THROW(FloxJsStrategy(script.path(), registry), std::runtime_error);
}

TEST(JsIntegrationTest, NoRegisterOk)
{
  // Plain scripts without flox.register() are valid (standalone script use case)
  TempJsFile script("var x = 1;");

  SymbolRegistry registry;
  EXPECT_NO_THROW(FloxJsStrategy(script.path(), registry));
}

// ============================================================
// Composite-condition DSL (W1-T028)
//
// `when(strategy, sym, barType, param).ema(50).gt(when(...).ema(200))`
// builds a tree out of indicator nodes + comparison/logical wrappers.
// Pure JS sugar over `lastNClosedBars`; no engine state.
// ============================================================

TEST(JsIntegrationTest, CompositeDslCrossover)
{
  TempJsFile script(R"(
    var crossUpReady = false;
    var crossUpValue = false;
    var rsiReady = false;
    var rsiValue = 0;

    class TestStrat extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["BTCUSDT"] }); }
      onBar(ctx, bar) {
        var TIME = 0;
        var M1 = 60 * 1000000000;
        var fast = when(this, "BTCUSDT", TIME, M1).ema(3);
        var slow = when(this, "BTCUSDT", TIME, M1).ema(6);
        var cross = fast.gt(slow);
        crossUpReady = cross.isReady();
        crossUpValue = cross.isReady() ? cross.value() : false;

        var rsi = when(this, "BTCUSDT", TIME, M1).rsi(3);
        rsiReady = rsi.isReady();
        rsiValue = rsi.isReady() ? rsi.value() : 0;
      }
    }
    flox.register(new TestStrat());
  )");

  SymbolRegistry registry;
  FloxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<FloxStrategyHandle>(bridge.get()));

  const uint64_t M1_NS = 60ull * 1'000'000'000ull;
  // Climb from 100 → 110 over 8 bars so the fast EMA(3) ends well above
  // the slow EMA(6).
  for (int i = 0; i < 8; ++i)
  {
    BarEvent ev{};
    ev.symbol = symIds[0];
    ev.barType = BarType::Time;
    ev.barTypeParam = M1_NS;
    double price = 100.0 + i * 1.5;
    ev.bar.open = Price::fromDouble(price);
    ev.bar.close = Price::fromDouble(price + 0.5);
    ev.bar.high = Price::fromDouble(price + 1.0);
    ev.bar.low = Price::fromDouble(price - 0.5);
    ev.bar.startTime = TimePoint{std::chrono::nanoseconds{static_cast<int64_t>(M1_NS) * i}};
    ev.bar.endTime = TimePoint{std::chrono::nanoseconds{static_cast<int64_t>(M1_NS) * (i + 1)}};
    bridge->onBar(ev);
  }

  auto* ctx = jsStrat.engine().context();

  JSValue ready = jsStrat.engine().getGlobalProperty("crossUpReady");
  EXPECT_TRUE(JS_ToBool(ctx, ready));
  JS_FreeValue(ctx, ready);

  JSValue val = jsStrat.engine().getGlobalProperty("crossUpValue");
  EXPECT_TRUE(JS_ToBool(ctx, val));
  JS_FreeValue(ctx, val);

  JSValue rready = jsStrat.engine().getGlobalProperty("rsiReady");
  EXPECT_TRUE(JS_ToBool(ctx, rready));
  JS_FreeValue(ctx, rready);

  JSValue rval = jsStrat.engine().getGlobalProperty("rsiValue");
  double rsiV = 0;
  JS_ToFloat64(ctx, &rsiV, rval);
  // Climbing series → RSI well above 50, ideally near 100.
  EXPECT_GT(rsiV, 80.0);
  JS_FreeValue(ctx, rval);
}

TEST(JsIntegrationTest, CompositeDslLogicalOps)
{
  TempJsFile script(R"(
    var andReady = false;
    var andValue = false;
    var orValue = false;
    var notValue = false;

    class TestStrat extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["BTCUSDT"] }); }
      onBar(ctx, bar) {
        var M1 = 60 * 1000000000;
        var fast = when(this, "BTCUSDT", 0, M1).ema(3);
        var slow = when(this, "BTCUSDT", 0, M1).ema(6);
        var crossUp = fast.gt(slow);
        var aboveHundred = fast.gt(99);

        var both = crossUp.and(aboveHundred);
        var either = crossUp.or(aboveHundred);
        var notCross = crossUp.not();

        andReady = both.isReady();
        andValue = both.isReady() ? both.value() : false;
        orValue = either.isReady() ? either.value() : false;
        notValue = notCross.isReady() ? notCross.value() : false;
      }
    }
    flox.register(new TestStrat());
  )");

  SymbolRegistry registry;
  FloxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<FloxStrategyHandle>(bridge.get()));

  const uint64_t M1_NS = 60ull * 1'000'000'000ull;
  for (int i = 0; i < 8; ++i)
  {
    BarEvent ev{};
    ev.symbol = symIds[0];
    ev.barType = BarType::Time;
    ev.barTypeParam = M1_NS;
    double price = 100.0 + i * 1.5;
    ev.bar.open = Price::fromDouble(price);
    ev.bar.close = Price::fromDouble(price + 0.5);
    ev.bar.high = Price::fromDouble(price + 1.0);
    ev.bar.low = Price::fromDouble(price - 0.5);
    ev.bar.startTime = TimePoint{std::chrono::nanoseconds{static_cast<int64_t>(M1_NS) * i}};
    ev.bar.endTime = TimePoint{std::chrono::nanoseconds{static_cast<int64_t>(M1_NS) * (i + 1)}};
    bridge->onBar(ev);
  }

  auto* ctx = jsStrat.engine().context();

  JSValue ar = jsStrat.engine().getGlobalProperty("andReady");
  EXPECT_TRUE(JS_ToBool(ctx, ar));
  JS_FreeValue(ctx, ar);

  JSValue av = jsStrat.engine().getGlobalProperty("andValue");
  EXPECT_TRUE(JS_ToBool(ctx, av));
  JS_FreeValue(ctx, av);

  JSValue ov = jsStrat.engine().getGlobalProperty("orValue");
  EXPECT_TRUE(JS_ToBool(ctx, ov));
  JS_FreeValue(ctx, ov);

  JSValue nv = jsStrat.engine().getGlobalProperty("notValue");
  EXPECT_FALSE(JS_ToBool(ctx, nv));
  JS_FreeValue(ctx, nv);
}

// ============================================================
// Multi-feed clock (W6-T021)
// ============================================================

TEST(JsIntegrationTest, MultiFeedClockWaitForAll)
{
  TempJsFile script(R"(
    var fired1 = "";
    var fired2 = "";
    var fired3 = "";
    var staleAfter = 0;
    class TestStrat extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["BTCUSDT", "ETHUSDT"] }); }
      onStart() {
        var c = new MultiFeedClock({
          symbols: [1, 2],
          policy: FeedClockPolicy.WaitForAll,
          timeoutMs: 200,
        });
        var r1 = c.tick(1000000000, 1);
        fired1 = String(r1.fired);
        var r2 = c.tick(1100000000, 2);
        fired2 = String(r2.fired);
        var r3 = c.tick(1200000000, 1);
        fired3 = String(r3.fired);
        staleAfter = r2.stalenessNs[1];
      }
    }
    flox.register(new TestStrat());
  )");

  SymbolRegistry registry;
  FloxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<FloxStrategyHandle>(bridge.get()));
  bridge->start();

  auto* ctx = jsStrat.engine().context();

  auto getStr = [&](const char* name)
  {
    JSValue v = jsStrat.engine().getGlobalProperty(name);
    const char* s = JS_ToCString(ctx, v);
    std::string out = s ? s : "";
    if (s)
    {
      JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
    return out;
  };

  EXPECT_EQ(getStr("fired1"), "false");
  EXPECT_EQ(getStr("fired2"), "true");
  EXPECT_EQ(getStr("fired3"), "false");

  JSValue stale = jsStrat.engine().getGlobalProperty("staleAfter");
  int64_t staleVal = 0;
  JS_ToBigInt64(ctx, &staleVal, stale);
  EXPECT_EQ(staleVal, 100000000);
  JS_FreeValue(ctx, stale);
}

// ============================================================
// Multi-leg order group (W15-T004)
// ============================================================

TEST(JsIntegrationTest, OrderGroupAllOrNothingReverts)
{
  TempJsFile script(R"(
    var groupState = "";
    var actions = [];
    class TestStrat extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["BTCUSDT", "ETHUSDT"] }); }
      onStart() {
        var g = new OrderGroup({ parentSignalId: 7, policy: OrderGroupPolicy.AllOrNothing });
        g.addMarketLeg(1, 0, 0.1);
        g.addMarketLeg(2, 1, 2.0);
        g.recordSubmit(0, 100);
        g.recordSubmit(1, 101);
        g.recordFill(0, 0.1);
        g.recordFailure(1);
        groupState = g.state();
        actions = g.recommendedActions();
      }
    }
    flox.register(new TestStrat());
  )");

  SymbolRegistry registry;
  FloxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<FloxStrategyHandle>(bridge.get()));
  bridge->start();

  auto* ctx = jsStrat.engine().context();
  JSValue st = jsStrat.engine().getGlobalProperty("groupState");
  const char* stStr = JS_ToCString(ctx, st);
  EXPECT_STREQ(stStr, "Reverting") << "Reverting state expected (AllOrNothing + leg failure)";
  JS_FreeCString(ctx, stStr);
  JS_FreeValue(ctx, st);

  // actions = [{kind:'revert', legIndex:0, symbol:1, side:1, qty:0.1}]
  JSValue acts = jsStrat.engine().getGlobalProperty("actions");
  uint32_t len = 0;
  JS_ToUint32(ctx, &len, JS_GetPropertyStr(ctx, acts, "length"));
  EXPECT_EQ(len, 1u);
  JS_FreeValue(ctx, acts);
}

// ============================================================
// Indicator-grid sugar (W3-T017)
//
// `grid(strategy, [BTC, ETH], [H4, M5]).ema(50)` instantiates one
// indicator per (symbol, timeframe) cell. Lookup by
// `g.get(symbol, barType, param)`.
// ============================================================

TEST(JsIntegrationTest, IndicatorGridCrossProduct)
{
  TempJsFile script(R"(
    var gridSize = 0;
    var btcReady = false;
    var btcValue = 0;
    var ethReady = false;
    var keysShape = "";

    class TestStrat extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["BTCUSDT", "ETHUSDT"] }); }
      onBar(ctx, bar) {
        var BTC = this._symbolMap["BTCUSDT"];
        var ETH = this._symbolMap["ETHUSDT"];
        var M1 = 60 * 1000000000;
        var H1 = 3600 * 1000000000;
        var g = grid(this, [BTC, ETH], [M1, H1]).ema(3);
        gridSize = g.size();

        var btcEma = g.get(BTC, 0, M1);
        btcReady = btcEma.isReady();
        if (btcReady) btcValue = btcEma.value();

        var ethEma = g.get(ETH, 0, H1);
        ethReady = ethEma.isReady();

        var ks = g.keys();
        keysShape = ks.length + "/" + ks[0].symbol + ":" + ks[0].param;
      }
    }
    flox.register(new TestStrat());
  )");

  SymbolRegistry registry;
  FloxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<FloxStrategyHandle>(bridge.get()));

  // Drive 5 M1 bars on BTC so the M1 EMA(3) is ready; H1 cells stay cold.
  const uint64_t M1_NS = 60ull * 1'000'000'000ull;
  for (int i = 0; i < 5; ++i)
  {
    BarEvent ev{};
    ev.symbol = symIds[0];  // BTC
    ev.barType = BarType::Time;
    ev.barTypeParam = M1_NS;
    ev.bar.open = Price::fromDouble(100.0 + i);
    ev.bar.close = Price::fromDouble(101.0 + i);
    ev.bar.high = Price::fromDouble(102.0 + i);
    ev.bar.low = Price::fromDouble(99.0 + i);
    ev.bar.startTime = TimePoint{std::chrono::nanoseconds{static_cast<int64_t>(M1_NS) * i}};
    ev.bar.endTime = TimePoint{std::chrono::nanoseconds{static_cast<int64_t>(M1_NS) * (i + 1)}};
    bridge->onBar(ev);
  }

  auto* ctx = jsStrat.engine().context();

  JSValue size = jsStrat.engine().getGlobalProperty("gridSize");
  int32_t sizeVal = 0;
  JS_ToInt32(ctx, &sizeVal, size);
  EXPECT_EQ(sizeVal, 4) << "2 symbols x 2 timeframes";
  JS_FreeValue(ctx, size);

  JSValue btcReady = jsStrat.engine().getGlobalProperty("btcReady");
  EXPECT_TRUE(JS_ToBool(ctx, btcReady));
  JS_FreeValue(ctx, btcReady);

  JSValue ethReady = jsStrat.engine().getGlobalProperty("ethReady");
  EXPECT_FALSE(JS_ToBool(ctx, ethReady)) << "ETH/H1 cell stays cold";
  JS_FreeValue(ctx, ethReady);
}

// ============================================================
// Multi-TF alignment helpers (W1-T027)
//
// Parity with the pybind11 + NAPI surface added in T026: a JS strategy
// can read its per-(symbol, type, param) bar ring via lastClosedBar and
// lastNClosedBars without bookkeeping by hand.
// ============================================================

TEST(JsIntegrationTest, MultiTfHelpersExposeBarRing)
{
  TempJsFile script(R"(
    var firstClose = -1;   // close of the M5 bar after the first onBar
    var thirdClose = -1;   // close of the latest M5 bar after the third onBar
    var thirdCount = 0;    // size of lastNClosedBars(3) after the third onBar
    var thirdAllCloses = [];
    var BAR_TYPE_TIME = 0;
    var M5_NS = 5 * 60 * 1000000000;

    class TestStrat extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["BTCUSDT"] }); }
      onBar(ctx, bar) {
        var last = this.lastClosedBar("BTCUSDT", BAR_TYPE_TIME, M5_NS);
        if (firstClose < 0) {
          firstClose = last ? last.close : -1;
        }
        thirdClose = last ? last.close : -1;
        var nbars = this.lastNClosedBars("BTCUSDT", BAR_TYPE_TIME, M5_NS, 5);
        thirdCount = nbars.length;
        thirdAllCloses = nbars.map(function(b){ return b.close; });
      }
    }
    flox.register(new TestStrat());
  )");

  SymbolRegistry registry;
  FloxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<FloxStrategyHandle>(bridge.get()));

  const uint64_t M5_NS = 5ull * 60 * 1'000'000'000ull;
  auto pushBar = [&](double open, double close, int64_t startNs)
  {
    BarEvent ev{};
    ev.symbol = symIds[0];
    ev.barType = BarType::Time;
    ev.barTypeParam = M5_NS;
    ev.bar.open = Price::fromDouble(open);
    ev.bar.close = Price::fromDouble(close);
    ev.bar.high = Price::fromDouble(std::max(open, close));
    ev.bar.low = Price::fromDouble(std::min(open, close));
    ev.bar.startTime = TimePoint{std::chrono::nanoseconds{startNs}};
    ev.bar.endTime = TimePoint{std::chrono::nanoseconds{startNs + (int64_t)M5_NS}};
    bridge->onBar(ev);
  };

  pushBar(100.0, 101.0, 0);
  pushBar(101.0, 102.5, (int64_t)M5_NS);
  pushBar(102.5, 103.0, 2 * (int64_t)M5_NS);

  auto* ctx = jsStrat.engine().context();

  JSValue first = jsStrat.engine().getGlobalProperty("firstClose");
  double firstVal = 0;
  JS_ToFloat64(ctx, &firstVal, first);
  EXPECT_NEAR(firstVal, 101.0, 1e-9);
  JS_FreeValue(ctx, first);

  JSValue third = jsStrat.engine().getGlobalProperty("thirdClose");
  double thirdVal = 0;
  JS_ToFloat64(ctx, &thirdVal, third);
  EXPECT_NEAR(thirdVal, 103.0, 1e-9);
  JS_FreeValue(ctx, third);

  JSValue count = jsStrat.engine().getGlobalProperty("thirdCount");
  int32_t countVal = 0;
  JS_ToInt32(ctx, &countVal, count);
  EXPECT_EQ(countVal, 3);
  JS_FreeValue(ctx, count);
}

TEST(JsIntegrationTest, MultiTfHelpersReturnNullBeforeAnyBar)
{
  TempJsFile script(R"(
    var beforeAnyBar = "unset";
    class TestStrat extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["BTCUSDT"] }); }
      onStart() {
        var bar = this.lastClosedBar("BTCUSDT", 0, 5*60*1000000000);
        beforeAnyBar = bar === null ? "null" : "non-null";
      }
    }
    flox.register(new TestStrat());
  )");

  SymbolRegistry registry;
  FloxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<FloxStrategyHandle>(bridge.get()));

  bridge->start();

  auto* ctx = jsStrat.engine().context();
  JSValue val = jsStrat.engine().getGlobalProperty("beforeAnyBar");
  const char* s = JS_ToCString(ctx, val);
  EXPECT_STREQ(s, "null");
  JS_FreeCString(ctx, s);
  JS_FreeValue(ctx, val);
}

TEST(JsIntegrationTest, MultiTfHelpersRingCapacityIsAdjustable)
{
  TempJsFile script(R"(
    var initialCap = 0;
    var afterSet = 0;
    var keptCount = 0;
    class TestStrat extends Strategy {
      constructor() { super({ exchange: "Test", symbols: ["BTCUSDT"] }); }
      onStart() {
        initialCap = this.barRingCapacity;
        this.setBarRingCapacity(3);
        afterSet = this.barRingCapacity;
      }
      onBar(ctx, bar) {
        var bars = this.lastNClosedBars("BTCUSDT", 0, 5*60*1000000000, 10);
        keptCount = bars.length;
      }
    }
    flox.register(new TestStrat());
  )");

  SymbolRegistry registry;
  FloxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<FloxStrategyHandle>(bridge.get()));

  bridge->start();

  const uint64_t M5_NS = 5ull * 60 * 1'000'000'000ull;
  for (int i = 0; i < 7; ++i)
  {
    BarEvent ev{};
    ev.symbol = symIds[0];
    ev.barType = BarType::Time;
    ev.barTypeParam = M5_NS;
    ev.bar.open = Price::fromDouble(100.0 + i);
    ev.bar.close = Price::fromDouble(100.5 + i);
    ev.bar.high = Price::fromDouble(101.0 + i);
    ev.bar.low = Price::fromDouble(99.5 + i);
    ev.bar.startTime = TimePoint{std::chrono::nanoseconds{static_cast<int64_t>(M5_NS) * i}};
    ev.bar.endTime = TimePoint{std::chrono::nanoseconds{static_cast<int64_t>(M5_NS) * (i + 1)}};
    bridge->onBar(ev);
  }

  auto* ctx = jsStrat.engine().context();
  JSValue cap = jsStrat.engine().getGlobalProperty("afterSet");
  int32_t capVal = 0;
  JS_ToInt32(ctx, &capVal, cap);
  EXPECT_EQ(capVal, 3);
  JS_FreeValue(ctx, cap);

  JSValue kept = jsStrat.engine().getGlobalProperty("keptCount");
  int32_t keptVal = 0;
  JS_ToInt32(ctx, &keptVal, kept);
  EXPECT_EQ(keptVal, 3);
  JS_FreeValue(ctx, kept);
}

TEST(JsIntegrationTest, JsExceptionInOnTradeDoesNotCrash)
{
  TempJsFile script(R"(
    var tradeCount = 0;
    class TestStrat extends Strategy {
      constructor() { super({ exchange: "T", symbols: ["S1"] }); }
      onTrade(ctx, trade) {
        tradeCount++;
        if (tradeCount === 2) throw new Error("intentional");
      }
    }
    flox.register(new TestStrat());
  )");

  SymbolRegistry registry;
  FloxJsStrategy jsStrat(script.path(), registry);
  auto callbacks = jsStrat.getCallbacks();
  auto symIds = jsStrat.symbolIds();

  auto bridge = std::make_unique<BridgeStrategy>(
      1, std::vector<SymbolId>(symIds.begin(), symIds.end()), registry, callbacks);
  jsStrat.injectHandle(static_cast<FloxStrategyHandle>(bridge.get()));

  // Trade 1: OK, Trade 2: throws, Trade 3: should still work
  for (int i = 0; i < 3; i++)
  {
    FloxSymbolContext fctx{};
    fctx.symbol_id = symIds[0];
    FloxTradeData ftrade{};
    ftrade.symbol = symIds[0];
    ftrade.price_raw = flox_price_from_double(100.0);
    ftrade.quantity_raw = flox_quantity_from_double(1.0);
    ftrade.is_buy = 1;
    callbacks.on_trade(callbacks.user_data, &fctx, &ftrade);
  }

  JSValue count = jsStrat.engine().getGlobalProperty("tradeCount");
  int32_t countVal = 0;
  JS_ToInt32(jsStrat.engine().context(), &countVal, count);
  EXPECT_EQ(countVal, 3);  // All 3 trades processed
  JS_FreeValue(jsStrat.engine().context(), count);
}
