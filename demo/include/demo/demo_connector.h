/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/book/bus/book_update_bus.h"
#include "lrvx/book/bus/trade_bus.h"
#include "lrvx/book/events/book_update_event.h"
#include "lrvx/connector/abstract_exchange_connector.h"
#include "lrvx/engine/engine_config.h"
#include "lrvx/util/memory/pool.h"

#include <atomic>
#include <random>
#include <string>
#include <thread>

namespace demo
{
using namespace lrvx;

class DemoConnector : public IExchangeConnector
{
 public:
  DemoConnector(const std::string& id, SymbolId symbol, BookUpdateBus& bookUpdateBus, TradeBus& tradeBus);
  void start() override;
  void stop() override;
  std::string exchangeId() const override { return _id; }

 private:
  void run();

  std::string _id;
  SymbolId _symbol;
  BookUpdateBus& _bookUpdateBus;
  TradeBus& _tradeBus;
  std::atomic<bool> _running{false};
  std::thread _thread;
  std::mt19937 _rng{std::random_device{}()};
  bool _poolExhaustionReported{false};

  // A bus slot holds its Handle until that slot is overwritten, so a pool
  // smaller than the ring drains before the ring can wrap -- and once it is
  // empty there are no publishes left to wrap it, so the feed stops for
  // good. This pool was sized 7 against a 4096-slot ring, which meant the
  // demo emitted exactly seven book updates and then went quiet while still
  // looking alive (trades pass by value and need no pool). Use the same
  // constant the real connectors use; engine_config static_asserts that it
  // exceeds the ring.
  pool::Pool<BookUpdateEvent, config::DEFAULT_CONNECTOR_POOL_CAPACITY> _bookPool;
};

}  // namespace demo
