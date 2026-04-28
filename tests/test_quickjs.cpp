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
