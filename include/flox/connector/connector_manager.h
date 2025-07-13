/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/connector/exchange_connector.h"

#include <iostream>
#include <map>
#include <memory>
#include <vector>

namespace flox
{

class ConnectorManager
{
 public:
  void registerConnector(std::shared_ptr<ExchangeConnector> connector)
  {
    const auto& id = connector->exchangeId();
    connectors[id] = connector;
  }

  void startAll(ExchangeConnector::BookUpdateCallback onBookUpdate,
                ExchangeConnector::TradeCallback onTrade)
  {
    for (auto& [symbol, connector] : connectors)
    {
      std::cout << "[ConnectorManager] starting: " << symbol << std::endl;
      connector->setCallbacks([onBookUpdate = std::move(onBookUpdate)](const BookUpdateEvent& update) mutable
                              { onBookUpdate(update); },
                              [onTrade = std::move(onTrade)](const TradeEvent& trade) mutable
                              { onTrade(trade); });
      connector->start();
    }
  }

 private:
  std::map<std::string, std::shared_ptr<ExchangeConnector>> connectors;
};

}  // namespace flox