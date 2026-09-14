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

#include <exception>
#include <map>
#include <memory>

namespace flox
{

class ConnectorManager
{
 public:
  ConnectorManager() = default;

  // Registered connectors are owned via shared_ptr; destroying a running
  // connector without stop() first is a guaranteed abort for implementations
  // that join a worker thread in their destructor. stopAll()
  // makes sure every connector this manager started is stopped before its
  // last owning reference goes away here.
  ~ConnectorManager() { stopAll(); }

  ConnectorManager(const ConnectorManager&) = delete;
  ConnectorManager& operator=(const ConnectorManager&) = delete;

  void registerConnector(std::shared_ptr<IExchangeConnector> connector)
  {
    const auto& id = connector->exchangeId();
    connectors[id] = connector;
  }

  void startAll(IExchangeConnector::BookUpdateCallback onBookUpdate,
                IExchangeConnector::TradeCallback onTrade)
  {
    // onBookUpdate/onTrade are MoveOnlyFunction: they cannot be copied, only
    // moved. Moving them into a fresh lambda on every loop iteration (the old
    // code) leaves every connector after the first one wired to a moved-from,
    // empty callback -- the guard in emitTrade/emitBookUpdate doesn't catch
    // this because the outer wrapper is non-empty, only the callable inside
    // it is gone. Move each callback ONCE into a shared owner, then give
    // every connector's lambda a copy of the (copyable) shared_ptr, so they
    // all end up calling the one callback the caller actually passed in.
    auto sharedBookUpdate =
        std::make_shared<IExchangeConnector::BookUpdateCallback>(std::move(onBookUpdate));
    auto sharedTrade = std::make_shared<IExchangeConnector::TradeCallback>(std::move(onTrade));

    for (auto& [id, connector] : connectors)
    {
      FLOX_LOG("[ConnectorManager] starting: " << id);
      connector->setCallbacks(
          [sharedBookUpdate](const BookUpdateEvent& update)
          { (*sharedBookUpdate)(update); },
          [sharedTrade](const TradeEvent& trade)
          { (*sharedTrade)(trade); });
      connector->start();
    }
  }

  // Stops every registered connector. Safe to call more than once and safe
  // to call on connectors that were never started (stop() implementations in
  // this codebase are idempotent). There was previously no supported way to
  // stop everything a ConnectorManager had started.
  void stopAll()
  {
    for (auto& [id, connector] : connectors)
    {
      try
      {
        FLOX_LOG("[ConnectorManager] stopping: " << id);
        connector->stop();
      }
      catch (const std::exception& e)
      {
        FLOX_LOG("[ConnectorManager] stop() threw for " << id << ": " << e.what());
      }
    }
  }

 private:
  std::map<std::string, std::shared_ptr<IExchangeConnector>> connectors;
};

}  // namespace flox
