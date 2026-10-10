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

#include "lrvx/backtest/fee_schedule.h"

namespace py = pybind11;

namespace lrvx
{

inline void bindFeeSchedule(py::module_& m)
{
  py::class_<lrvx::FeeTier>(m, "FeeTier")
      .def_readonly("min_notional_30d", &lrvx::FeeTier::minNotional30d)
      .def_readonly("maker_bps", &lrvx::FeeTier::makerBps)
      .def_readonly("taker_bps", &lrvx::FeeTier::takerBps);

  py::class_<lrvx::FeeSchedule>(m, "FeeSchedule")
      .def(py::init<>())
      .def("add_tier", &lrvx::FeeSchedule::addTier, py::arg("min_notional_30d"),
           py::arg("maker_bps"), py::arg("taker_bps"))
      .def_static("binance_um_futures", &lrvx::FeeSchedule::binance_um_futures)
      .def_static("bybit_linear", &lrvx::FeeSchedule::bybit_linear)
      .def_static("okx_swap", &lrvx::FeeSchedule::okx_swap)
      .def_static("deribit", &lrvx::FeeSchedule::deribit)
      .def("record_fill", &lrvx::FeeSchedule::recordFill, py::arg("ts_ns"),
           py::arg("notional"))
      .def("fee_for", &lrvx::FeeSchedule::feeFor, py::arg("ts_ns"),
           py::arg("notional"), py::arg("is_maker"))
      .def("current_bps", &lrvx::FeeSchedule::currentBps, py::arg("now_ns"))
      .def("current_tier_index", &lrvx::FeeSchedule::currentTierIndex)
      .def("tier_count", &lrvx::FeeSchedule::tierCount)
      .def("tiers", &lrvx::FeeSchedule::tiers)
      .def("tier_transition_ts_ns", &lrvx::FeeSchedule::tierTransitionTsNs)
      .def("rolling_notional_30d", &lrvx::FeeSchedule::rollingNotional30d)
      .def("reset_rolling", &lrvx::FeeSchedule::resetRolling)
      .def("bind_account", &lrvx::FeeSchedule::bindAccount, py::arg("account"),
           py::keep_alive<1, 2>(),
           "Bind an Account so 30d rolling notional reads from the account's "
           "aggregate counter instead of this schedule's internal one.")
      .def("clear_account_binding", &lrvx::FeeSchedule::clearAccountBinding);
}

}  // namespace lrvx
