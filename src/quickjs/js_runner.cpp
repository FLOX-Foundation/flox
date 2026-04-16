#include "js_strategy.h"

#include "flox/capi/bridge_strategy.h"

#include <iostream>
#include <memory>

int main(int argc, char* argv[])
{
  if (argc < 2)
  {
    std::cerr << "Usage: flox_js_runner <script.js>" << std::endl;
    return 1;
  }

  try
  {
    flox::SymbolRegistry registry;
    flox::FloxJsStrategy jsStrategy(argv[1], registry);

    auto callbacks = jsStrategy.getCallbacks();
    auto symbolIds = jsStrategy.symbolIds();

    // Create BridgeStrategy to connect JS strategy to the engine
    auto bridge = std::make_unique<flox::BridgeStrategy>(
        1, std::vector<flox::SymbolId>(symbolIds.begin(), symbolIds.end()), registry, callbacks);

    // Inject the bridge handle so JS can call emit_* and query methods
    jsStrategy.injectHandle(static_cast<FloxStrategyHandle>(bridge.get()));

    std::cout << "Loaded JS strategy with " << symbolIds.size() << " symbol(s)" << std::endl;

    bridge->start();
    bridge->stop();
  }
  catch (const std::exception& e)
  {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
