/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "demo/demo_strategy.h"
#include "demo/latency_collector.h"

#include <chrono>
#include <iostream>

namespace demo
{

DemoStrategy::DemoStrategy(SymbolId symbol, OrderExecutionBus& execBus)
    : _riskManager(&_killSwitch), _executor(execBus), _symbol(symbol)
{
}

void DemoStrategy::start()
{
  std::cout << "[strategy " << _symbol << "] start" << std::endl;
}

void DemoStrategy::stop()
{
  std::cout << "[strategy " << _symbol << "] stop" << std::endl;
}

void DemoStrategy::onTrade(const TradeEvent& ev)
{
  if (ev.trade.symbol != _symbol)
    return;

  Order order{};

  {
    MEASURE_LATENCY(LatencyCollector::StrategyOnTrade);

    order.id = ++_nextId;
    order.side = (_nextId % 2 == 0 ? Side::BUY : Side::SELL);
    order.price = (_nextId % 2 == 0 ? ev.trade.price - Price::fromDouble(0.01)
                                    : ev.trade.price + Price::fromDouble(0.01));
    order.quantity = Quantity::fromDouble(1.0);
    order.type = OrderType::LIMIT;
    order.symbol = _symbol;
    order.createdAt = std::chrono::steady_clock::now();

    _killSwitch.check(order);

    if (_killSwitch.isTriggered())
    {
      std::cout << "[kill] strategy " << _symbol
                << " blocked by kill switch"
                << ", reason: " << _killSwitch.reason() << "\n";
      return;
    }

    std::string reason;
    if (!_validator.validate(order, reason))
    {
      std::cout << "[strategy " << _symbol << "] order rejected: " << reason << '\n';
      return;
    }

    if (!_riskManager.allow(order))
    {
      std::cout << "[risk] strategy " << _symbol << " rejected order id=" << order.id << '\n';
      return;
    }
  }

  _executor.submitOrder(order);
}

void DemoStrategy::onBookUpdate(const BookUpdateEvent& ev)
{
  if (ev.update.symbol == _symbol)
  {
    _book.applyBookUpdate(ev);
  }
}

}  // namespace demo
