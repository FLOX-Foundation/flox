/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "demo/latency_collector.h"

#include "flox/common.h"
#include "flox/engine/abstract_subscriber.h"
#include "flox/execution/abstract_executor.h"
#include "flox/execution/bus/order_execution_bus.h"
#include "flox/killswitch/abstract_killswitch.h"
#include "flox/metrics/abstract_execution_tracker.h"
#include "flox/metrics/abstract_pnl_tracker.h"
#include "flox/position/abstract_position_manager.h"
#include "flox/risk/abstract_risk_manager.h"
#include "flox/sink/abstract_storage_sink.h"
#include "flox/validation/abstract_order_validator.h"

#include <chrono>
#include <random>
#include <string>

namespace demo
{
using namespace flox;

class ConsoleExecutionTracker final : public IExecutionTracker
{
 public:
  void start() override {}
  void stop() override {}

  void onOrderSubmitted(const Order& order, TimePoint ts) override
  {
    FLOX_LOG("[tracker] submitted " << order.id << " at " << ts.time_since_epoch().count());
  }
  void onOrderAccepted(const Order& order, TimePoint ts) override
  {
    FLOX_LOG("[tracker] accepted " << order.id << " at " << ts.time_since_epoch().count());
  }

  void onOrderPartiallyFilled(const Order& order, Quantity qty, TimePoint ts) override
  {
    FLOX_LOG("[tracker] partial fill " << order.id << " qty=" << qty.toDouble()
                                       << " at " << ts.time_since_epoch().count());
  }
  void onOrderFilled(const Order& order, TimePoint ts) override
  {
    FLOX_LOG("[tracker] filled " << order.id << " after " << ts.time_since_epoch().count());
  }
  void onOrderCanceled(const Order& order, TimePoint ts) override
  {
    FLOX_LOG("[tracker] canceled " << order.id << " at " << ts.time_since_epoch().count());
  }

  void onOrderExpired(const Order& order, TimePoint ts) override
  {
    FLOX_LOG("[tracker] expired " << order.id << " at " << ts.time_since_epoch().count());
  }
  void onOrderRejected(const Order& order, const std::string& reason, TimePoint) override
  {
    FLOX_LOG("[tracker] rejected " << order.id << " reason=" << reason);
  }

  void onOrderReplaced(const Order& oldOrder, const Order& newOrder, TimePoint ts) override
  {
    FLOX_LOG("[tracker] replaced old=" << oldOrder.id << " new=" << newOrder.id
                                       << " at " << ts.time_since_epoch().count());
  }
};

class SimplePnLTracker final : public IPnLTracker
{
 public:
  void start() override {}
  void stop() override {}

  void onOrderFilled(const Order& order) override
  {
    double value = order.price.toDouble() * order.quantity.toDouble();
    _pnl += (order.side == Side::BUY ? -value : value);
    FLOX_LOG("[pnl] " << _pnl);
  }

 private:
  double _pnl = 0.0;
};

class StdoutStorageSink final : public IStorageSink
{
 public:
  void start() override {}
  void stop() override {}

  void store(const Order& order) override { FLOX_LOG("[storage] order " << order.id); }
};

class SimpleOrderValidator final : public IOrderValidator
{
 public:
  void start() override {}
  void stop() override {}

  bool validate(const Order& order, std::string& reason) const override
  {
    static thread_local std::mt19937 rng(std::random_device{}());
    static std::uniform_int_distribution<int> dist(0, 19);

    if (dist(rng) == 0)
    {
      reason = "random rejection";
      return false;
    }
    return true;
  }
};

class SimpleKillSwitch final : public IKillSwitch
{
 public:
  void start() override {}
  void stop() override {}

  void check(const Order& order) override
  {
  }

  void trigger(const std::string& r) override
  {
    _triggered = true;
    _reason = r;
    _since = std::chrono::steady_clock::now();
  }

  bool isTriggered() const override { return _triggered; }

  std::string reason() const override { return _reason; }

 private:
  void reset()
  {
    _triggered = false;
    _reason.clear();
  }

  bool _triggered = false;
  std::string _reason;
  TimePoint _since{};
};

class SimpleRiskManager final : public IRiskManager
{
 public:
  explicit SimpleRiskManager(SimpleKillSwitch* ks) : _ks(ks) {}

  void start() override {}
  void stop() override {}

  bool allow(const Order& order) const override
  {
    static thread_local std::mt19937 rng(std::random_device{}());
    static std::uniform_real_distribution<> dist(0.0, 1.0);

    if (dist(rng) < 0.05)
    {
      FLOX_LOG("[risk] rejected order id=" << order.id << " (random)");
      return false;
    }

    return true;
  }

 private:
  SimpleKillSwitch* _ks;
};

class SimplePositionManager : public IPositionManager
{
 public:
  static constexpr size_t MAX_SYMBOLS = 65'536;

  explicit SimplePositionManager(SubscriberId id) : IPositionManager(id) {}

  void start() override {}
  void stop() override {}

  void onOrderSubmitted(const Order& order) override
  {
    FLOX_LOG("[position] order submitted: id=" << order.id);
  }

  void onOrderAccepted(const Order& order) override
  {
    FLOX_LOG("[position] order accepted: id=" << order.id);
  }

  void onOrderPartiallyFilled(const Order& order, Quantity qty) override
  {
    FLOX_LOG("[position] order partially filled: id=" << order.id
                                                      << ", qty=" << qty.toDouble());
    update(order, qty);
  }

  void onOrderFilled(const Order& order) override
  {
    FLOX_LOG("[position] order filled: id=" << order.id
                                            << ", qty=" << order.quantity.toDouble());

    update(order, order.quantity);
  }

  void onOrderCanceled(const Order& order) override
  {
    FLOX_LOG("[position] order canceled: id=" << order.id);
  }

  void onOrderExpired(const Order& order) override
  {
    FLOX_LOG("[position] order expired: id=" << order.id);
  }

  void onOrderRejected(const Order& order, const std::string& reason) override
  {
    FLOX_LOG("[position] order rejected: id=" << order.id << " reason: " << reason);
  }

  void onOrderReplaced(const Order& oldOrder, const Order& newOrder) override
  {
    FLOX_LOG("[position] order replaced: old_id=" << oldOrder.id
                                                  << ", new_id=" << newOrder.id);
  }

  Quantity getPosition(SymbolId symbol) const override
  {
    return _positions[symbol];
  }

  void printPositions() const
  {
    for (SymbolId i = 0; i < MAX_SYMBOLS; ++i)
    {
      if (!_positions[i].isZero())
      {
        FLOX_LOG("Symbol " << i << ": " << _positions[i].toDouble() << "\n");
      }
    }
  }

 private:
  void update(const Order& order, Quantity qty)
  {
    if (order.side == Side::BUY)
    {
      _positions[order.symbol] += qty;
    }
    else
    {
      _positions[order.symbol] -= qty;
    }
  }

  Quantity _positions[MAX_SYMBOLS]{};
};

class SimpleOrderExecutor final : public IOrderExecutor
{
 public:
  explicit SimpleOrderExecutor(OrderExecutionBus& bus) : _bus(bus), _posMgr(387) {}

  void start() override { _bus.start(); }
  void stop() override { _bus.stop(); }

  void submitOrder(const Order& order) override
  {
    // accepted
    OrderEvent ev{OrderEventType::ACCEPTED};
    ev.order = order;
    _bus.publish(ev);

    // simulate partial fill
    Quantity half = Quantity::fromRaw(order.quantity.raw() / 2);
    ev = {};
    ev.type = OrderEventType::PARTIALLY_FILLED;
    ev.order = order;
    ev.fillQty = half;

    _bus.publish(ev);

    Order part = order;
    part.quantity = half;

    _pnlTracker.onOrderFilled(part);
    _posMgr.onOrderFilled(part);

    // simulate replace
    Order newOrder = order;
    newOrder.price += Price::fromDouble(0.1);
    ev = {};
    ev.type = OrderEventType::REPLACED;
    ev.order = order;
    ev.newOrder = newOrder;
    _bus.publish(ev);

    // final fill of remaining quantity
    ev = {};
    ev.type = OrderEventType::FILLED;
    ev.order = newOrder;
    ev.fillQty = order.quantity - half;
    _bus.publish(ev);

    Order rest = newOrder;
    rest.quantity = order.quantity - half;

    _sink.store(newOrder);

    _pnlTracker.onOrderFilled(rest);
    _posMgr.onOrderFilled(rest);

    collector.record(LatencyCollector::EndToEnd,
                     std::chrono::steady_clock::now() - order.createdAt);
  }

 private:
  OrderExecutionBus& _bus;
  SimplePnLTracker _pnlTracker;
  StdoutStorageSink _sink;
  SimplePositionManager _posMgr;
};

}  // namespace demo
