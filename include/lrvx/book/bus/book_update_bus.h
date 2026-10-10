/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/book/events/book_update_event.h"
#include "lrvx/util/eventing/event_bus.h"
#include "lrvx/util/memory/pool.h"

namespace lrvx
{

using BookUpdateBus = EventBus<pool::Handle<BookUpdateEvent>>;

/**
 * @brief Create a BookUpdateBus with optimal performance configuration
 * @param enablePerformanceOptimizations Enable CPU frequency scaling optimizations
 * @return Unique pointer to configured BookUpdateBus
 */
inline std::unique_ptr<BookUpdateBus>
createOptimalBookUpdateBus(bool enablePerformanceOptimizations = false)
{
  auto bus = std::make_unique<BookUpdateBus>();
#if LRVX_CPU_AFFINITY_ENABLED
  bool success = bus->setupOptimalConfiguration(BookUpdateBus::ComponentType::MARKET_DATA,
                                                enablePerformanceOptimizations);
  if (!success)
  {
    LRVX_LOG_WARN("BookUpdateBus affinity setup failed, continuing with default configuration");
  }
#endif
  return bus;
}

/**
 * @brief Configure an existing BookUpdateBus for optimal performance
 * @param bus BookUpdateBus instance to configure
 * @param enablePerformanceOptimizations Enable CPU frequency scaling optimizations
 * @return true if configuration was successful
 */
inline bool configureBookUpdateBusForPerformance(BookUpdateBus& bus, bool enablePerformanceOptimizations = false)
{
#if LRVX_CPU_AFFINITY_ENABLED
  return bus.setupOptimalConfiguration(BookUpdateBus::ComponentType::MARKET_DATA,
                                       enablePerformanceOptimizations);
#else
  return true;
#endif
}

}  // namespace lrvx
