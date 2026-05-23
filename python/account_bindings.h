/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "flox/backtest/account.h"

namespace py = pybind11;

namespace flox_py
{

inline void bindAccount(py::module_& m)
{
  py::enum_<flox::MarginMode>(m, "MarginMode")
      .value("Cross", flox::MarginMode::Cross)
      .value("Isolated", flox::MarginMode::Isolated)
      .export_values();

  py::class_<flox::Account>(m, "Account")
      .def(py::init<uint64_t, double>(),
           py::arg("account_id") = 0,
           py::arg("equity") = 0.0)
      .def("account_id", &flox::Account::accountId)
      .def("equity", &flox::Account::equity)
      .def("set_equity", &flox::Account::setEquity, py::arg("equity"))
      .def("add_equity", &flox::Account::addEquity, py::arg("delta"))
      .def("margin_mode", &flox::Account::marginMode)
      .def("set_margin_mode", [](flox::Account& self, py::object mode)
           {
             if (py::isinstance<py::str>(mode))
             {
               self.setMarginModeByName(mode.cast<std::string>());
             }
             else
             {
               self.setMarginMode(mode.cast<flox::MarginMode>());
             } }, "Accepts a MarginMode enum or a string name (cross, isolated).", py::arg("mode"))
      .def("open_position", &flox::Account::openPosition, py::arg("symbol"), py::arg("quantity"), py::arg("entry_price"), py::arg("isolated_equity") = 0.0)
      .def("close_position", &flox::Account::closePosition, py::arg("symbol"))
      .def("position_count", &flox::Account::positionCount)
      .def("set_mark", &flox::Account::setMark, py::arg("symbol"), py::arg("price"), py::arg("ts_ns") = int64_t{0})
      .def("mark_for", &flox::Account::markFor, py::arg("symbol"))
      .def("mark_ts_for", &flox::Account::markTsFor, py::arg("symbol"))
      .def("has_stale_marks", &flox::Account::hasStaleMarks, py::arg("now_ns"), py::arg("budget_ns"))
      .def("total_notional", &flox::Account::totalNotional)
      .def("total_unrealised_pnl", &flox::Account::totalUnrealisedPnl)
      .def("record_fill", &flox::Account::recordFill, py::arg("ts_ns"), py::arg("notional"), py::arg("symbol") = flox::SymbolId{0})
      .def("rolling_notional_by_symbol_30d", &flox::Account::rollingNotionalBySymbol30d)
      .def("rolling_notional_30d", &flox::Account::rollingNotional30d)
      .def("reset_rolling", &flox::Account::resetRolling);
}

}  // namespace flox_py
