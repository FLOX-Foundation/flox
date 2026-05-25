/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// python/bare_executor_bindings.h
//
// Direct py::class_<flox::SimulatedExecutor> bindings, so the executor
// instance owned by a VenueStack can be driven from Python via
// stack.executor(). The standalone PySimulatedExecutor wrapper in
// backtest_bindings.h owns its own SimulatedExecutor and is unrelated;
// VenueStack hands out a reference to its internal one, which without
// this binding has no Python-visible methods.
//
// Initial method surface is the slim subset needed by W6.T032 (RL env
// venue-stack backing), W15 broker plumbing (PaperBroker / CcxtBroker),
// and docs/examples/python_rate_limit_aware_market_maker.py:
// submit / cancel / book-and-trade updates / fills inspection / rate
// limit + venue availability setters. Iceberg, brackets, slippage, and
// queue-model accessors land in follow-up PRs as Phase 2 tasks need
// them.

#pragma once

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "flox/backtest/rate_limit_policy.h"
#include "flox/backtest/simulated_executor.h"
#include "flox/backtest/venue_availability.h"
#include "flox/common.h"
#include "flox/execution/order.h"

#include <cstdint>
#include <string>

namespace py = pybind11;

namespace flox_py
{

inline void bindBareExecutor(py::module_& m)
{
  using namespace flox;

  auto parseSide = [](const std::string& s) -> Side
  { return s == "buy" ? Side::BUY : Side::SELL; };

  auto parseOrderType = [](const std::string& s) -> OrderType
  {
    if (s == "limit")
    {
      return OrderType::LIMIT;
    }
    if (s == "stop_market")
    {
      return OrderType::STOP_MARKET;
    }
    if (s == "stop_limit")
    {
      return OrderType::STOP_LIMIT;
    }
    if (s == "take_profit_market")
    {
      return OrderType::TAKE_PROFIT_MARKET;
    }
    if (s == "take_profit_limit")
    {
      return OrderType::TAKE_PROFIT_LIMIT;
    }
    if (s == "trailing_stop")
    {
      return OrderType::TRAILING_STOP;
    }
    return OrderType::MARKET;
  };

  auto parseTif = [](const std::string& s) -> TimeInForce
  {
    if (s == "ioc")
    {
      return TimeInForce::IOC;
    }
    if (s == "fok")
    {
      return TimeInForce::FOK;
    }
    if (s == "gtd")
    {
      return TimeInForce::GTD;
    }
    if (s == "post_only")
    {
      return TimeInForce::POST_ONLY;
    }
    return TimeInForce::GTC;
  };

  py::class_<flox::SimulatedExecutor>(
      m, "VenueExecutor",
      "Executor handed out by VenueStack.executor(). Same simulated\n"
      "engine as standalone SimulatedExecutor, but the instance is\n"
      "owned by the venue stack and shares its account, fees, funding,\n"
      "liquidation, and rate-limit policy. Use this when you want to\n"
      "drive the venue-realistic stack from Python — RL envs, paper\n"
      "brokers, custom Strategy harnesses.")
      .def(
          "submit_order",
          [parseSide, parseOrderType, parseTif](
              flox::SimulatedExecutor& self, uint64_t id,
              const std::string& side, double price, double qty,
              const std::string& type, uint32_t symbol,
              const std::string& tif, bool reduce_only,
              int64_t expires_at_ns, uint64_t account_id)
          {
            Order order;
            order.id = id;
            order.side = parseSide(side);
            order.price = Price::fromDouble(price);
            order.quantity = Quantity::fromDouble(qty);
            order.symbol = symbol;
            order.type = parseOrderType(type);
            order.timeInForce = parseTif(tif);
            order.flags.reduceOnly = reduce_only ? 1 : 0;
            order.accountId = account_id;
            if (expires_at_ns > 0)
            {
              order.expiresAfter = TimePoint(
                  std::chrono::nanoseconds(expires_at_ns));
            }
            self.submitOrder(order);
          },
          "Submit an order. side: buy|sell. type: market|limit|stop_market|"
          "stop_limit|take_profit_market|take_profit_limit|trailing_stop. "
          "tif: gtc|ioc|fok|gtd|post_only.",
          py::arg("id"), py::arg("side"), py::arg("price"), py::arg("quantity"),
          py::arg("type") = "market", py::arg("symbol") = 1,
          py::arg("tif") = "gtc", py::arg("reduce_only") = false,
          py::arg("expires_at_ns") = 0, py::arg("account_id") = 0)
      .def(
          "cancel_order",
          [](flox::SimulatedExecutor& self, uint64_t order_id)
          { self.cancelOrder(order_id); },
          py::arg("order_id"))
      .def(
          "cancel_all",
          [](flox::SimulatedExecutor& self, uint32_t symbol)
          { self.cancelAllOrders(symbol); },
          py::arg("symbol"))
      .def(
          "on_trade",
          [](flox::SimulatedExecutor& self, uint32_t symbol, double price,
             bool is_buy)
          { self.onTrade(symbol, Price::fromDouble(price), is_buy); },
          "Feed a trade tick for order matching.",
          py::arg("symbol"), py::arg("price"), py::arg("is_buy"))
      .def(
          "on_trade_qty",
          [](flox::SimulatedExecutor& self, uint32_t symbol, double price,
             double quantity, bool is_buy)
          {
            self.onTrade(symbol, Price::fromDouble(price),
                         Quantity::fromDouble(quantity), is_buy);
          },
          "Feed a trade tick with quantity (enables queue-fill simulation).",
          py::arg("symbol"), py::arg("price"), py::arg("quantity"),
          py::arg("is_buy"))
      .def(
          "on_bar",
          [](flox::SimulatedExecutor& self, uint32_t symbol, double close_price)
          { self.onBar(symbol, Price::fromDouble(close_price)); },
          "Feed a bar close for order matching.",
          py::arg("symbol"), py::arg("close_price"))
      .def(
          "fills_list",
          [](const flox::SimulatedExecutor& self)
          {
            py::list out;
            for (const auto& f : self.fills())
            {
              py::dict d;
              d["order_id"] = f.orderId;
              d["symbol"] = f.symbol;
              d["side"] = (f.side == Side::BUY) ? "buy" : "sell";
              d["price"] = f.price.toDouble();
              d["quantity"] = f.quantity.toDouble();
              d["timestamp_ns"] = static_cast<int64_t>(f.timestampNs);
              out.append(d);
            }
            return out;
          },
          "Every fill recorded on this executor as a list of dicts.")
      .def_property_readonly(
          "fill_count",
          [](const flox::SimulatedExecutor& self)
          { return self.fills().size(); })
      .def(
          "set_rate_limit_policy",
          [](flox::SimulatedExecutor& self, const flox::RateLimitPolicy& p)
          { self.setRateLimitPolicy(p); },
          py::arg("policy"))
      .def(
          "clear_rate_limit_policy",
          [](flox::SimulatedExecutor& self)
          { self.clearRateLimitPolicy(); })
      .def(
          "set_venue_availability",
          [](flox::SimulatedExecutor& self, flox::VenueAvailability* va)
          { self.setVenueAvailability(va); },
          py::arg("venue_availability"));
}

}  // namespace flox_py
