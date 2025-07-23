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

#include "flox/aggregator/events/candle_event.h"
#include "flox/util/eventing/event_bus.h"

namespace flox
{

#ifdef USE_SYNC_CANDLE_BUS
using CandleBus = EventBus<CandleEvent, SyncPolicy<CandleEvent>>;
#else
using CandleBus = EventBus<CandleEvent, AsyncPolicy<CandleEvent>>;
#endif

/**
 * @brief Create and configure a CandleBus with optimal isolated core settings
 * @param enablePerformanceOptimizations Enable CPU frequency scaling optimizations
 * @return Configured CandleBus instance
 */
inline std::unique_ptr<CandleBus> createOptimalCandleBus(bool enablePerformanceOptimizations = false)
{
  auto bus = std::make_unique<CandleBus>();
#if FLOX_CPU_AFFINITY_ENABLED
  bool success = bus->setupOptimalConfiguration(CandleBus::ComponentType::MARKET_DATA,
                                                enablePerformanceOptimizations);
  if (!success)
  {
    FLOX_LOG_WARN("CandleBus affinity setup failed, continuing with default configuration");
  }

#endif
  return bus;
}

/**
 * @brief Configure an existing CandleBus for optimal performance
 * @param bus CandleBus instance to configure
 * @param enablePerformanceOptimizations Enable CPU frequency scaling optimizations
 * @return true if configuration was successful
 */
inline bool configureCandleBusForPerformance(CandleBus& bus, bool enablePerformanceOptimizations = false)
{
#if FLOX_CPU_AFFINITY_ENABLED
  return bus.setupOptimalConfiguration(CandleBus::ComponentType::MARKET_DATA,
                                       enablePerformanceOptimizations);
#else
  return true;
#endif
}
}  // namespace flox
