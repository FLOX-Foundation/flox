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

#include "flox/testing/bar_dispatch_recorder.h"

namespace py = pybind11;

namespace flox_py
{

inline void bindBarDispatchRecorder(py::module_& m)
{
  py::class_<flox::testing::BarDispatchRecorder>(m, "BarDispatchRecorder")
      .def(py::init<>())
      .def("add_time_interval_seconds", [](flox::testing::BarDispatchRecorder& r, uint32_t seconds)
           { return r.addTimeIntervalSeconds(seconds); }, py::arg("seconds"))
      .def("on_trade", &flox::testing::BarDispatchRecorder::onTrade, py::arg("symbol"), py::arg("price"), py::arg("qty"), py::arg("ts_ns"))
      .def("finalize", &flox::testing::BarDispatchRecorder::finalize)
      .def("count", &flox::testing::BarDispatchRecorder::count)
      .def("type_at", &flox::testing::BarDispatchRecorder::typeAt, py::arg("index"))
      .def("param_at", &flox::testing::BarDispatchRecorder::paramAt, py::arg("index"));
}

}  // namespace flox_py
