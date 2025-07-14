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
#include "flox/log/log.h"

#include <map>
#include <memory>

namespace flox
{

class ConnectorManager
{
 public:
  void registerConnector(std::shared_ptr<IExchangeConnector> connector)
  {
    const auto& id = connector->exchangeId();
    connectors[id] = connector;
  }

  void startAll(IExchangeConnector::BookUpdateCallback onBookUpdate,
                IExchangeConnector::TradeCallback onTrade)
  {
    for (auto& [symbol, connector] : connectors)
    {
      FLOX_LOG("[ConnectorManager] starting: " << symbol);
      connector->setCallbacks([onBookUpdate = std::move(onBookUpdate)](const BookUpdateEvent& update) mutable
                              { onBookUpdate(update); },
                              [onTrade = std::move(onTrade)](const TradeEvent& trade) mutable
                              { onTrade(trade); });
      connector->start();
    }
  }

 private:
  std::map<std::string, std::shared_ptr<IExchangeConnector>> connectors;
};

}  // namespace flox