#include "js_strategy.h"

#include "lrvx/capi/bridge_strategy.h"

#include <iostream>
#include <memory>

int main(int argc, char* argv[])
{
  if (argc < 2)
  {
    std::cerr << "Usage: lrvx_js_runner <script.js>\n";
    return 1;
  }

  try
  {
    lrvx::SymbolRegistry registry;
    lrvx::LrvxJsStrategy jsStrategy(argv[1], registry);

    auto symbolIds = jsStrategy.symbolIds();

    if (symbolIds.empty())
    {
      // Plain script, no strategy -- already executed, just exit
      return 0;
    }

    // Strategy mode
    auto callbacks = jsStrategy.getCallbacks();
    auto bridge = std::make_unique<lrvx::BridgeStrategy>(
        1, std::vector<lrvx::SymbolId>(symbolIds.begin(), symbolIds.end()), registry, callbacks);

    jsStrategy.injectHandle(static_cast<LrvxStrategyHandle>(bridge.get()));

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
