/*
 * Flox Engine
 * Developed by Evgenii Makarov (https://github.com/eeiaao)
 *
 * Copyright (c) 2025 Evgenii Makarov
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <string>
#include <vector>

namespace flox
{

struct SymbolConfig
{
  std::string symbol;
  double tickSize;
};

struct ExchangeConfig
{
  std::string name;
  std::string type;
  std::vector<SymbolConfig> symbols;
};

struct KillSwitchConfig
{
  double maxOrderQty = 10'000.0;
  double maxLoss = -1e6;
  int maxOrdersPerSecond = -1;
};

struct EngineConfig
{
  std::vector<ExchangeConfig> exchanges;
  KillSwitchConfig killSwitchConfig;

  std::string logLevel = "info";
  std::string logFile;
};

#ifndef FLOX_DEFAULT_EVENTBUS_QUEUE_SIZE
#define FLOX_DEFAULT_EVENTBUS_QUEUE_SIZE 4096
#endif

namespace config
{

inline constexpr size_t DEFAULT_EVENTBUS_QUEUE_SIZE = FLOX_DEFAULT_EVENTBUS_QUEUE_SIZE;

}

}  // namespace flox