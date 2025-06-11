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

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace flox
{

class ConnectorFactory
{
 public:
  using CreatorFunc =
      std::move_only_function<std::shared_ptr<IExchangeConnector>(const std::string&)>;

  static ConnectorFactory& instance()
  {
    static ConnectorFactory factory;
    return factory;
  }

  void registerConnector(const std::string& type, CreatorFunc creator)
  {
    _creators[type] = std::move(creator);
  }

  std::shared_ptr<IExchangeConnector> createConnector(const std::string& type,
                                                      const std::string& symbol)
  {
    auto it = _creators.find(type);
    if (it != _creators.end())
    {
      return it->second(symbol);
    }
    return nullptr;
  }

 private:
  ConnectorFactory() = default;
  std::unordered_map<std::string, CreatorFunc> _creators;
};

}  // namespace flox
