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

#include "flox/backtest/fee_schedule.h"

namespace py = pybind11;

namespace flox_py
{

inline void bindFeeSchedule(py::module_& m)
{
  py::class_<flox::FeeTier>(m, "FeeTier")
      .def_readonly("min_notional_30d", &flox::FeeTier::minNotional30d)
      .def_readonly("maker_bps", &flox::FeeTier::makerBps)
      .def_readonly("taker_bps", &flox::FeeTier::takerBps);

  py::class_<flox::FeeSchedule>(m, "FeeSchedule")
      .def(py::init<>())
      .def("add_tier", &flox::FeeSchedule::addTier, py::arg("min_notional_30d"),
           py::arg("maker_bps"), py::arg("taker_bps"))
      .def_static("binance_um_futures", &flox::FeeSchedule::binance_um_futures)
      .def_static("bybit_linear", &flox::FeeSchedule::bybit_linear)
      .def_static("okx_swap", &flox::FeeSchedule::okx_swap)
      .def_static("deribit", &flox::FeeSchedule::deribit)
      .def("record_fill", &flox::FeeSchedule::recordFill, py::arg("ts_ns"),
           py::arg("notional"))
      .def("fee_for", &flox::FeeSchedule::feeFor, py::arg("ts_ns"),
           py::arg("notional"), py::arg("is_maker"))
      .def("current_bps", &flox::FeeSchedule::currentBps, py::arg("now_ns"))
      .def("current_tier_index", &flox::FeeSchedule::currentTierIndex)
      .def("tier_count", &flox::FeeSchedule::tierCount)
      .def("tiers", &flox::FeeSchedule::tiers)
      .def("tier_transition_ts_ns", &flox::FeeSchedule::tierTransitionTsNs)
      .def("rolling_notional_30d", &flox::FeeSchedule::rollingNotional30d)
      .def("reset_rolling", &flox::FeeSchedule::resetRolling)
      .def("bind_account", &flox::FeeSchedule::bindAccount, py::arg("account"),
           py::keep_alive<1, 2>(),
           "Bind an Account so 30d rolling notional reads from the account's "
           "aggregate counter instead of this schedule's internal one.")
      .def("clear_account_binding", &flox::FeeSchedule::clearAccountBinding);
}

}  // namespace flox_py
