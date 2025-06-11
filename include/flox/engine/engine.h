/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/connector/abstract_exchange_connector.h"
#include "flox/engine/abstract_subsystem.h"
#include "flox/engine/engine_config.h"

#include <memory>
#include <string>
#include <vector>

namespace flox
{

struct ExchangeInstance
{
  std::string exchangeType;
  std::string name;
  std::string symbol;
  std::shared_ptr<IExchangeConnector> connector;
};

class Engine : public ISubsystem
{
 public:
  Engine(const EngineConfig& config, std::vector<std::unique_ptr<ISubsystem>> subsystems,
         std::vector<std::shared_ptr<IExchangeConnector>> connectors);

  void start() override;
  void stop() override;

 private:
  EngineConfig _config;

  std::vector<std::unique_ptr<ISubsystem>> _subsystems;
  std::vector<std::shared_ptr<IExchangeConnector>> _connectors;
};

}  // namespace flox
