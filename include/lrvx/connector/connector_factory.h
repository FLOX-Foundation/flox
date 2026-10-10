/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/connector/abstract_exchange_connector.h"
#include "lrvx/util/base/move_only_function.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace lrvx
{

class ConnectorFactory
{
 public:
  using CreatorFunc = MoveOnlyFunction<std::shared_ptr<IExchangeConnector>(const std::string&)>;

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

}  // namespace lrvx
