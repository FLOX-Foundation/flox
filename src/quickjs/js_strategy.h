#pragma once

#include "js_engine.h"

#include "flox/capi/bridge_strategy.h"
#include "flox/capi/flox_capi.h"
#include "flox/engine/symbol_registry.h"

#include <memory>
#include <string>
#include <vector>

namespace flox
{

class FloxJsStrategy
{
 public:
  FloxJsStrategy(const std::string& scriptPath, SymbolRegistry& registry);
  ~FloxJsStrategy();

  FloxStrategyCallbacks getCallbacks();
  const std::vector<uint32_t>& symbolIds() const { return _symbolIds; }
  FloxJsEngine& engine() { return _engine; }
  void injectHandle(FloxStrategyHandle handle);

 private:
  void loadStdlib();
  void loadScript(const std::string& path);
  void resolveSymbols();

  // C callbacks dispatched from BridgeStrategy
  static void onTrade(void* userData, const FloxSymbolContext* ctx, const FloxTradeData* trade);
  static void onBook(void* userData, const FloxSymbolContext* ctx, const FloxBookData* book);
  static void onStart(void* userData);
  static void onStop(void* userData);

  JSValue makeCtxObject(const FloxSymbolContext* ctx);
  JSValue makeTradeObject(const FloxTradeData* trade);
  JSValue makeBookObject(const FloxBookData* book);

  FloxJsEngine _engine;
  SymbolRegistry& _registry;
  JSValue _strategyObj = JS_UNDEFINED;
  std::vector<uint32_t> _symbolIds;
  std::vector<std::string> _symbolNames;
};

}  // namespace flox
