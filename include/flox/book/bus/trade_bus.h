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
#include "flox/book/events/trade_event.h"
#include "flox/util/eventing/event_bus.h"

namespace flox
{

#ifdef USE_SYNC_MARKET_BUS
using TradeBus = EventBus<TradeEvent, SyncPolicy<TradeEvent>>;
#else
using TradeBus = EventBus<TradeEvent, AsyncPolicy<TradeEvent>>;
#endif

/**
 * @brief Create and configure a TradeBus with optimal isolated core settings
 * @param enablePerformanceOptimizations Enable CPU frequency scaling optimizations
 * @return Configured TradeBus instance
 */
inline std::unique_ptr<TradeBus> createOptimalTradeBus(bool enablePerformanceOptimizations = false)
{
  auto bus = std::make_unique<TradeBus>();
#if FLOX_CPU_AFFINITY_ENABLED
  [[maybe_unused]] bool success = bus->setupOptimalConfiguration(TradeBus::ComponentType::MARKET_DATA,
                                                                 enablePerformanceOptimizations);
#endif
  return bus;
}

/**
 * @brief Configure an existing TradeBus for optimal performance
 * @param bus TradeBus instance to configure
 * @param enablePerformanceOptimizations Enable CPU frequency scaling optimizations
 * @return true if configuration was successful
 */
inline bool configureTradeBusForPerformance(TradeBus& bus, bool enablePerformanceOptimizations = false)
{
#if FLOX_CPU_AFFINITY_ENABLED
  bool success = bus.setupOptimalConfiguration(TradeBus::ComponentType::MARKET_DATA,
                                               enablePerformanceOptimizations);
  return success;
#else
  return true;
#endif
}
}  // namespace flox
