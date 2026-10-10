/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <memory>
#include "lrvx/execution/events/order_event.h"
#include "lrvx/util/eventing/event_bus.h"

namespace lrvx
{

using OrderExecutionBus = EventBus<OrderEvent>;

/**
 * @brief Create and configure an OrderExecutionBus with optimal isolated core settings
 * @param enablePerformanceOptimizations Enable CPU frequency scaling optimizations
 * @return Configured OrderExecutionBus instance
 */
inline std::unique_ptr<OrderExecutionBus> createOptimalOrderExecutionBus(bool enablePerformanceOptimizations = false)
{
  auto bus = std::make_unique<OrderExecutionBus>();
#if LRVX_CPU_AFFINITY_ENABLED
  bool success = bus->setupOptimalConfiguration(OrderExecutionBus::ComponentType::EXECUTION,
                                                enablePerformanceOptimizations);
  if (!success)
  {
    LRVX_LOG_WARN("OrderExecutionBus affinity setup failed, continuing with default configuration");
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
#if LRVX_CPU_AFFINITY_ENABLED
  return bus.setupOptimalConfiguration(OrderExecutionBus::ComponentType::EXECUTION,
                                       enablePerformanceOptimizations);
#else
  return true;
#endif
}

}  // namespace lrvx
