/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "demo/demo_builder.h"
#include "demo/demo_connector.h"
#include "demo/demo_strategy.h"
#include "demo/simple_components.h"

#include <chrono>
#include <memory>

#include "flox/aggregator/bus/candle_bus.h"
#include "flox/aggregator/candle_aggregator.h"
#include "flox/book/bus/book_update_bus.h"
#include "flox/book/bus/trade_bus.h"
#include "flox/execution/bus/order_execution_bus.h"
#include "flox/execution/execution_tracker_adapter.h"
#include "flox/strategy/abstract_strategy.h"

namespace demo
{

DemoBuilder::DemoBuilder(const EngineConfig& cfg) : _config(cfg) {}

std::unique_ptr<Engine> DemoBuilder::build()
{
  auto bookUpdateBus = std::make_unique<BookUpdateBus>();
  auto tradeBus = std::make_unique<TradeBus>();
  auto execBus = std::make_unique<OrderExecutionBus>();
  auto candleBus = std::make_unique<CandleBus>();

  auto execTracker = std::make_unique<ConsoleExecutionTracker>();
  auto trackerAdapter = std::make_shared<ExecutionTrackerAdapter>(1, execTracker.get());
  execBus->subscribe(trackerAdapter);

  auto candleAggregator = std::make_shared<CandleAggregator>(std::chrono::seconds{60}, candleBus.get());
  tradeBus->subscribe(candleAggregator);

#if FLOX_CPU_AFFINITY_ENABLED
  // Configure CPU affinity for optimal performance
  using namespace performance;

  // Set up optimal performance configuration with isolated cores
  performance::CriticalComponentConfig config;
  config.preferIsolatedCores = true;
  config.exclusiveIsolatedCores = true;

  auto cpuAffinity = createCpuAffinity();
  auto assignment = cpuAffinity->getNumaAwareCoreAssignment(config);

  std::cout << "[DemoBuilder] ✓ CPU affinity configured for high-performance workload:" << std::endl;
  std::cout << "  - Market Data cores: " << assignment.marketDataCores.size() << std::endl;
  std::cout << "  - Execution cores: " << assignment.executionCores.size() << std::endl;
  std::cout << "  - Strategy cores: " << assignment.strategyCores.size() << std::endl;
  std::cout << "  - Risk cores: " << assignment.riskCores.size() << std::endl;
  std::cout << "  - Using isolated cores: " << (assignment.hasIsolatedCores ? "Yes" : "No") << std::endl;
#else
  std::cout << "[DemoBuilder] ✓ CPU affinity disabled (ENABLE_CPU_AFFINITY=OFF)" << std::endl;
#endif

  std::vector<std::shared_ptr<IStrategy>> strategies;
  std::vector<std::unique_ptr<ISubsystem>> subsystems;

  for (SymbolId sym = 0; sym < 8; ++sym)
  {
    auto strat = std::make_shared<DemoStrategy>(sym, *execBus);

    bookUpdateBus->subscribe(strat);
    tradeBus->subscribe(strat);
  }

  std::vector<std::shared_ptr<ExchangeConnector>> connectors;
  for (SymbolId sym = 0; sym < 3; ++sym)
  {
    auto conn = std::make_shared<DemoConnector>(std::string("demo") + char('A' + sym), sym, *bookUpdateBus, *tradeBus);
    connectors.push_back(conn);
  }

  subsystems.push_back(std::move(bookUpdateBus));
  subsystems.push_back(std::move(tradeBus));
  subsystems.push_back(std::move(candleBus));
  subsystems.push_back(std::move(execBus));
  subsystems.push_back(std::move(execTracker));

  return std::make_unique<Engine>(_config, std::move(subsystems), std::move(connectors));
}

}  // namespace demo
