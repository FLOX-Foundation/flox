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

#include "flox/backtest/funding_schedule.h"

namespace py = pybind11;

namespace flox_py
{

inline void bindFundingSchedule(py::module_& m)
{
  py::class_<flox::FundingPayment>(m, "FundingPayment")
      .def_readonly("timestamp_ns", &flox::FundingPayment::timestampNs)
      .def_readonly("symbol", &flox::FundingPayment::symbol)
      .def_readonly("rate", &flox::FundingPayment::rate)
      .def_readonly("mark_price", &flox::FundingPayment::markPrice)
      .def_readonly("position_signed", &flox::FundingPayment::positionSigned)
      .def_readonly("amount", &flox::FundingPayment::amount);

  py::class_<flox::FundingTapeEntry>(m, "FundingTapeEntry")
      .def(py::init<>())
      .def_readwrite("timestamp_ns", &flox::FundingTapeEntry::timestampNs)
      .def_readwrite("symbol", &flox::FundingTapeEntry::symbol)
      .def_readwrite("rate", &flox::FundingTapeEntry::rate);

  py::class_<flox::FundingSchedule>(m, "FundingSchedule")
      .def(py::init<>())
      .def_static("constant", &flox::FundingSchedule::constant, py::arg("interval_ns"),
                  py::arg("rate"))
      .def_static("tape", &flox::FundingSchedule::tape, py::arg("events"))
      .def_static("tape_by_symbol", &flox::FundingSchedule::tapeBySymbol,
                  py::arg("entries"))
      .def_static("binance_um_futures", &flox::FundingSchedule::binance_um_futures)
      .def_static("bybit_linear", &flox::FundingSchedule::bybit_linear)
      .def_static("okx_swap", &flox::FundingSchedule::okx_swap)
      .def_static("bitget_hourly", &flox::FundingSchedule::bitget_hourly)
      .def("load_tape", &flox::FundingSchedule::loadTape, py::arg("path"),
           "Read CSV with columns timestamp_ns, symbol, funding_rate. "
           "Returns True on success.")
      .def("set_constant_rate", &flox::FundingSchedule::setConstantRate,
           py::arg("rate"))
      .def("reset", &flox::FundingSchedule::reset)
      .def("interval_ns", &flox::FundingSchedule::intervalNs)
      .def("constant_rate", &flox::FundingSchedule::constantRate)
      .def("last_tick_ns", &flox::FundingSchedule::lastTickNs)
      .def("settlement_timestamps", &flox::FundingSchedule::settlementTimestamps)
      .def("per_symbol_tape", &flox::FundingSchedule::perSymbolTape)
      .def("tick", &flox::FundingSchedule::tick, py::arg("now_ns"), py::arg("symbols"),
           py::arg("positions"), py::arg("mark_prices"));
}

}  // namespace flox_py
