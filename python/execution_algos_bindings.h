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

#include <memory>
#include <utility>
#include <vector>

#include "flox/execution/algos.h"

namespace py = pybind11;

namespace flox_py
{

inline py::dict childToDict(const flox::execution::ChildOrder& c)
{
  py::dict d;
  d["order_id"] = c.order_id;
  d["timestamp_ns"] = c.timestamp_ns;
  d["qty"] = c.qty;
  d["price"] = c.price;
  d["type"] = (c.type == flox::execution::OrderType::Limit) ? "limit" : "market";
  return d;
}

inline py::list pendingToList(const flox::execution::ExecutionAlgo& algo)
{
  py::list out;
  for (const auto& c : algo.pending())
  {
    out.append(childToDict(c));
  }
  return out;
}

inline void bindExecutionAlgos(py::module_& m)
{
  using ExecSide = flox::execution::Side;
  using ExecOrderType = flox::execution::OrderType;
  using flox::execution::ExecutionAlgo;
  using flox::execution::IcebergExecutor;
  using flox::execution::POVExecutor;
  using flox::execution::TWAPExecutor;
  using flox::execution::VWAPExecutor;

  py::enum_<ExecSide>(m, "_ExecSide")
      .value("Buy", ExecSide::Buy)
      .value("Sell", ExecSide::Sell);

  py::enum_<ExecOrderType>(m, "_ExecOrderType")
      .value("Market", ExecOrderType::Market)
      .value("Limit", ExecOrderType::Limit);

  py::class_<ExecutionAlgo>(m, "_ExecutionAlgoBase")
      .def("step", &ExecutionAlgo::step, py::arg("now_ns"))
      .def("report_fill", &ExecutionAlgo::reportFill, py::arg("qty"))
      .def("observe_volume", &ExecutionAlgo::observeVolume, py::arg("qty"))
      .def("pending", &pendingToList,
           "Return the pending child-order list as a Python list of dicts.")
      .def("clear_pending", &ExecutionAlgo::clearPending)
      .def_property_readonly("target_qty", &ExecutionAlgo::targetQty)
      .def_property_readonly("submitted_qty", &ExecutionAlgo::submittedQty)
      .def_property_readonly("filled_qty", &ExecutionAlgo::filledQty)
      .def_property_readonly("remaining_qty", &ExecutionAlgo::remainingQty)
      .def("is_done", &ExecutionAlgo::isDone);

  py::class_<TWAPExecutor, ExecutionAlgo>(m, "_TWAPExecutorNative")
      .def(py::init<double, ExecSide, uint32_t, ExecOrderType, double, int64_t,
                    uint32_t, int64_t>(),
           py::arg("target_qty"), py::arg("side"),
           py::arg("symbol") = 0,
           py::arg("type") = ExecOrderType::Market,
           py::arg("limit_price") = 0.0,
           py::arg("duration_ns") = 0,
           py::arg("slice_count") = 0,
           py::arg("start_time_ns") = 0);

  py::class_<VWAPExecutor, ExecutionAlgo>(m, "_VWAPExecutorNative")
      .def(py::init([](double target_qty, ExecSide side,
                       std::vector<std::pair<int64_t, double>> curve,
                       uint32_t symbol, ExecOrderType type, double limit_price)
                    { return std::make_unique<VWAPExecutor>(target_qty, side, symbol, type,
                                                            limit_price, std::move(curve)); }),
           py::arg("target_qty"), py::arg("side"),
           py::arg("volume_curve"),
           py::arg("symbol") = 0,
           py::arg("type") = ExecOrderType::Market,
           py::arg("limit_price") = 0.0);

  py::class_<IcebergExecutor, ExecutionAlgo>(m, "_IcebergExecutorNative")
      .def(py::init([](double target_qty, ExecSide side, double visible_qty,
                       uint32_t symbol, ExecOrderType type, double limit_price)
                    { return std::make_unique<IcebergExecutor>(target_qty, side, symbol, type,
                                                               limit_price, visible_qty); }),
           py::arg("target_qty"), py::arg("side"),
           py::arg("visible_qty"),
           py::arg("symbol") = 0,
           py::arg("type") = ExecOrderType::Market,
           py::arg("limit_price") = 0.0);

  py::class_<POVExecutor, ExecutionAlgo>(m, "_POVExecutorNative")
      .def(py::init([](double target_qty, ExecSide side, double participation_rate,
                       uint32_t symbol, ExecOrderType type, double limit_price,
                       double min_slice_qty)
                    { return std::make_unique<POVExecutor>(target_qty, side, symbol, type,
                                                           limit_price, participation_rate,
                                                           min_slice_qty); }),
           py::arg("target_qty"), py::arg("side"),
           py::arg("participation_rate"),
           py::arg("symbol") = 0,
           py::arg("type") = ExecOrderType::Market,
           py::arg("limit_price") = 0.0,
           py::arg("min_slice_qty") = 0.0);
}

}  // namespace flox_py
