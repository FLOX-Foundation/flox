/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <memory>

#include "flox/aggregator/events/bar_event.h"
#include "flox/util/eventing/event_bus.h"

namespace flox
{

using BarBus = EventBus<BarEvent>;

inline std::unique_ptr<BarBus> createOptimalBarBus(bool enablePerformanceOptimizations = false)
{
  auto bus = std::make_unique<BarBus>();
#if FLOX_CPU_AFFINITY_ENABLED
  bool success =
      bus->setupOptimalConfiguration(BarBus::ComponentType::MARKET_DATA, enablePerformanceOptimizations);
  if (!success)
  {
    FLOX_LOG_WARN("BarBus affinity setup failed, continuing with default configuration");
  }
#else
  (void)enablePerformanceOptimizations;
#endif
  return bus;
}

inline bool configureBarBusForPerformance(BarBus& bus, bool enablePerformanceOptimizations = false)
{
#if FLOX_CPU_AFFINITY_ENABLED
  return bus.setupOptimalConfiguration(BarBus::ComponentType::MARKET_DATA, enablePerformanceOptimizations);
#else
  (void)bus;
  (void)enablePerformanceOptimizations;
  return true;
#endif
}

}  // namespace flox
