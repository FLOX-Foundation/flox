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
#include "flox/execution/events/order_event.h"
#include "flox/util/eventing/event_bus.h"

namespace flox
{

#ifdef FLOX_USE_SYNC_ORDER_BUS
using OrderExecutionBus = EventBus<OrderEvent, SyncPolicy<OrderEvent> >;
#else
using OrderExecutionBus = EventBus<OrderEvent, AsyncPolicy<OrderEvent> >;
#endif

/**
 * @brief Create and configure an OrderExecutionBus with optimal isolated core settings
 * @param enablePerformanceOptimizations Enable CPU frequency scaling optimizations
 * @return Configured OrderExecutionBus instance
 */
inline std::unique_ptr<OrderExecutionBus> createOptimalOrderExecutionBus(bool enablePerformanceOptimizations = false)
{
  auto bus = std::make_unique<OrderExecutionBus>();
#if FLOX_CPU_AFFINITY_ENABLED
  bool success = bus->setupOptimalConfiguration(OrderExecutionBus::ComponentType::EXECUTION,
                                                enablePerformanceOptimizations);
  if (!success)
  {
    FLOX_LOG_WARN("OrderExecutionBus affinity setup failed, continuing with default configuration");
  }
#endif
  return bus;
}

/**
 * @brief Configure an existing OrderExecutionBus for optimal performance
 * @param bus OrderExecutionBus instance to configure
 * @param enablePerformanceOptimizations Enable CPU frequency scaling optimizations
 * @return true if configuration was successful
 */
inline bool configureOrderExecutionBusForPerformance(OrderExecutionBus& bus, bool enablePerformanceOptimizations = false)
{
#if FLOX_CPU_AFFINITY_ENABLED
  return bus.setupOptimalConfiguration(OrderExecutionBus::ComponentType::EXECUTION,
                                       enablePerformanceOptimizations);
#else
  return true;
#endif
}
}  // namespace flox
