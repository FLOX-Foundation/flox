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

#include "lrvx/testing/bar_dispatch_recorder.h"

namespace py = pybind11;

namespace lrvx
{

inline void bindBarDispatchRecorder(py::module_& m)
{
  py::class_<lrvx::testing::BarDispatchRecorder>(m, "BarDispatchRecorder")
      .def(py::init<>())
      .def("add_time_interval_seconds", [](lrvx::testing::BarDispatchRecorder& r, uint32_t seconds)
           { return r.addTimeIntervalSeconds(seconds); }, py::arg("seconds"))
      .def("on_trade", &lrvx::testing::BarDispatchRecorder::onTrade, py::arg("symbol"), py::arg("price"), py::arg("qty"), py::arg("ts_ns"))
      .def("finalize", &lrvx::testing::BarDispatchRecorder::finalize)
      .def("count", &lrvx::testing::BarDispatchRecorder::count)
      .def("type_at", &lrvx::testing::BarDispatchRecorder::typeAt, py::arg("index"))
      .def("param_at", &lrvx::testing::BarDispatchRecorder::paramAt, py::arg("index"));
}

}  // namespace lrvx
