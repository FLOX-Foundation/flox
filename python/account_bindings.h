/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <utility>
#include <vector>

#include "lrvx/backtest/account.h"

namespace py = pybind11;

namespace lrvx
{

inline void bindAccount(py::module_& m)
{
  py::enum_<lrvx::MarginMode>(m, "MarginMode")
      .value("Cross", lrvx::MarginMode::Cross)
      .value("Isolated", lrvx::MarginMode::Isolated)
      .export_values();

  // Account stores equity, marks, notionals and the rolling window in the
  // engine's fixed-point types; this surface is double-facing, so each
  // accessor converts at the boundary and each mutator takes the Account
  // overload that quantises there. The member pointers the bindings used
  // before are gone for the overloaded ones: `&Account::setEquity` no longer
  // names a single function.
  py::class_<lrvx::Account>(m, "Account")
      .def(py::init<uint64_t, double>(),
           py::arg("account_id") = 0,
           py::arg("equity") = 0.0)
      .def("account_id", &lrvx::Account::accountId)
      .def("equity", [](const lrvx::Account& self)
           { return self.equity().toDouble(); })
      .def("set_equity", [](lrvx::Account& self, double equity)
           { self.setEquity(equity); }, py::arg("equity"))
      .def("add_equity", [](lrvx::Account& self, double delta)
           { self.addEquity(delta); }, py::arg("delta"))
      .def("margin_mode", &lrvx::Account::marginMode)
      .def("set_margin_mode", [](lrvx::Account& self, py::object mode)
           {
             if (py::isinstance<py::str>(mode))
             {
               self.setMarginModeByName(mode.cast<std::string>());
             }
             else
             {
               self.setMarginMode(mode.cast<lrvx::MarginMode>());
             } }, "Accepts a MarginMode enum or a string name (cross, isolated).", py::arg("mode"))
      .def("open_position", [](lrvx::Account& self, lrvx::SymbolId symbol, double quantity, double entryPrice, double isolatedEquity, double contractMultiplier, bool isLongOption)
           { self.openPosition(symbol, quantity, entryPrice, isolatedEquity, contractMultiplier, isLongOption); }, py::arg("symbol"), py::arg("quantity"), py::arg("entry_price"), py::arg("isolated_equity") = 0.0, py::arg("contract_multiplier") = 1.0, py::arg("is_long_option") = false)
      .def("close_position", &lrvx::Account::closePosition, py::arg("symbol"))
      .def("position_count", &lrvx::Account::positionCount)
      .def("set_mark", [](lrvx::Account& self, lrvx::SymbolId symbol, double price, int64_t tsNs)
           { self.setMark(symbol, price, tsNs); }, py::arg("symbol"), py::arg("price"), py::arg("ts_ns") = int64_t{0})
      .def("mark_for", [](const lrvx::Account& self, lrvx::SymbolId symbol)
           { return self.markFor(symbol).toDouble(); }, py::arg("symbol"))
      .def("mark_ts_for", &lrvx::Account::markTsFor, py::arg("symbol"))
      .def("has_stale_marks", &lrvx::Account::hasStaleMarks, py::arg("now_ns"), py::arg("budget_ns"))
      .def("total_notional", [](const lrvx::Account& self)
           { return self.totalNotional().toDouble(); })
      .def("total_unrealised_pnl", [](const lrvx::Account& self)
           { return self.totalUnrealisedPnl().toDouble(); })
      .def("margin_notional", [](const lrvx::Account& self)
           { return self.marginNotional().toDouble(); })
      .def("margin_unrealised_pnl", [](const lrvx::Account& self)
           { return self.marginUnrealisedPnl().toDouble(); })
      .def("cross_headroom", [](const lrvx::Account& self, double tierFraction)
           { return self.crossHeadroom(tierFraction).toDouble(); }, py::arg("tier_fraction"))
      .def("record_fill", [](lrvx::Account& self, int64_t tsNs, double notional, lrvx::SymbolId symbol)
           { self.recordFill(tsNs, notional, symbol); }, py::arg("ts_ns"), py::arg("notional"), py::arg("symbol") = lrvx::SymbolId{0})
      .def("rolling_notional_by_symbol_30d", [](const lrvx::Account& self)
           {
             std::vector<std::pair<lrvx::SymbolId, double>> out;
             for (const auto& [symbol, notional] : self.rollingNotionalBySymbol30d())
             {
               out.emplace_back(symbol, notional.toDouble());
             }
             return out; })
      .def("rolling_notional_30d", [](const lrvx::Account& self)
           { return self.rollingNotional30d().toDouble(); })
      .def("reset_rolling", &lrvx::Account::resetRolling)
      .def("reset", [](lrvx::Account& self, double equity)
           { self.reset(equity); }, py::arg("equity"),
           "Clear every open position and mark, reset the 30-day rolling "
           "notional window, and set equity to `equity`. Use this to reuse "
           "one Account across repeated backtest or RL-training episodes "
           "on the same tape -- without it, a position and its equity "
           "delta from one episode survive into the next.");
}

}  // namespace lrvx
