/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "demo/demo_builder.h"
#include "demo/demo_connector.h"
#include "demo/demo_strategy.h"
#include "demo/simple_components.h"

#include <chrono>
#include <memory>

#include "lrvx/aggregator/bar_aggregator.h"
#include "lrvx/aggregator/bus/bar_bus.h"
#include "lrvx/book/bus/book_update_bus.h"
#include "lrvx/book/bus/trade_bus.h"
#include "lrvx/execution/bus/order_execution_bus.h"
#include "lrvx/execution/execution_tracker_adapter.h"
#include "lrvx/strategy/abstract_strategy.h"

namespace demo
{

DemoBuilder::DemoBuilder(const EngineConfig& cfg) : _config(cfg) {}

std::unique_ptr<Engine> DemoBuilder::build()
{
  auto bookUpdateBus = std::make_unique<BookUpdateBus>();
  auto tradeBus = std::make_unique<TradeBus>();
  auto execBus = std::make_unique<OrderExecutionBus>();
  auto barBus = std::make_unique<BarBus>();

  auto execTracker = std::make_unique<ConsoleExecutionTracker>();
  auto trackerAdapter = std::make_unique<ExecutionTrackerAdapter>(1, execTracker.get());
  execBus->subscribe(trackerAdapter.get());

  auto barAggregator = std::make_unique<TimeBarAggregator>(TimeBarPolicy(std::chrono::seconds{60}), barBus.get());
  tradeBus->subscribe(barAggregator.get());

#if LRVX_CPU_AFFINITY_ENABLED
  // Configure CPU affinity for optimal performance
  using namespace performance;

  // Set up optimal performance configuration with isolated cores
  performance::CriticalComponentConfig config;
  config.preferIsolatedCores = true;
  config.exclusiveIsolatedCores = true;

  auto cpuAffinity = createCpuAffinity();
  auto assignment = cpuAffinity->getNumaAwareCoreAssignment(config);

  LRVX_LOG("[DemoBuilder] ✓ CPU affinity configured for high-performance workload:");
  LRVX_LOG("  - Market Data cores: " << assignment.marketDataCores.size());
  LRVX_LOG("  - Execution cores: " << assignment.executionCores.size());
  LRVX_LOG("  - Strategy cores: " << assignment.strategyCores.size());
  LRVX_LOG("  - Risk cores: " << assignment.riskCores.size());
  LRVX_LOG("  - Using isolated cores: " << (assignment.hasIsolatedCores ? "Yes" : "No"));
#else
  LRVX_LOG("[DemoBuilder] ✓ CPU affinity disabled (LRVX_ENABLE_CPU_AFFINITY=OFF)");
#endif

  std::vector<std::shared_ptr<IStrategy>> strategies;
  std::vector<std::unique_ptr<ISubsystem>> subsystems;

  for (SymbolId sym = 0; sym < 8; ++sym)
  {
    auto strat = std::make_unique<DemoStrategy>(sym, *execBus);

    bookUpdateBus->subscribe(strat.get());
    tradeBus->subscribe(strat.get());

    subsystems.push_back(std::move(strat));
  }

  std::vector<std::shared_ptr<IExchangeConnector>> connectors;
  for (SymbolId sym = 0; sym < 3; ++sym)
  {
    auto conn = std::make_shared<DemoConnector>(std::string("demo") + char('A' + sym), sym, *bookUpdateBus, *tradeBus);
    connectors.push_back(conn);
  }

  // Engine::stop() stops subsystems in reverse registration order, so
  // registration must run consumers-first: a subscriber that is registered
  // after its bus would be stopped while the bus is still delivering to it
  // (the bar aggregator's stop() flushes and clears state the trade-bus
  // consumers were concurrently writing).
  subsystems.push_back(std::move(execTracker));
  subsystems.push_back(std::move(trackerAdapter));
  subsystems.push_back(std::move(execBus));
  subsystems.push_back(std::move(barBus));
  subsystems.push_back(std::move(barAggregator));
  subsystems.push_back(std::move(tradeBus));
  subsystems.push_back(std::move(bookUpdateBus));

  return std::make_unique<Engine>(_config, std::move(subsystems), std::move(connectors));
}

}  // namespace demo
