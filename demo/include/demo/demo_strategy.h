/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "demo/simple_components.h"
#include "flox/book/nlevel_order_book.h"
#include "flox/engine/abstract_subscriber.h"
#include "flox/execution/bus/order_execution_bus.h"
#include "flox/strategy/abstract_strategy.h"

#include "flox/book/events/book_update_event.h"
#include "flox/book/events/trade_event.h"

namespace demo
{

using namespace flox;

class DemoStrategy : public IStrategy
{
 public:
  DemoStrategy(SymbolId symbol, OrderExecutionBus& execBus);

  SubscriberId id() const override { return reinterpret_cast<SubscriberId>(this); }

  void start() override;
  void stop() override;

  void onTrade(const TradeEvent& trade) override;
  void onBookUpdate(const BookUpdateEvent& book) override;

 private:
  SimpleKillSwitch _killSwitch;
  SimpleOrderValidator _validator;
  SimpleRiskManager _riskManager;
  SimpleOrderExecutor _executor;

  SymbolId _symbol;
  NLevelOrderBook<> _book{Price::fromDouble(0.1)};
  OrderId _nextId{0};
};

}  // namespace demo
