/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "demo/simple_components.h"
#include "lrvx/book/nlevel_order_book.h"
#include "lrvx/engine/abstract_subscriber.h"
#include "lrvx/execution/bus/order_execution_bus.h"
#include "lrvx/strategy/abstract_strategy.h"

#include "lrvx/book/events/book_update_event.h"
#include "lrvx/book/events/trade_event.h"

namespace demo
{

using namespace lrvx;

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
