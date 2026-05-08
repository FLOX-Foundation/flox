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
#include <vector>

#include "flox/backtest/latency_model.h"

namespace py = pybind11;

namespace flox_py
{

inline void bindLatencyModels(py::module_& m)
{
  py::class_<flox::LatencySample>(m, "LatencySample",
      "One draw from a LatencyModel covering feed, order, and fill in "
      "non-negative nanoseconds.")
      .def(py::init<>())
      .def(py::init([](int64_t feed_ns, int64_t order_ns, int64_t fill_ns)
                    {
                      flox::LatencySample s;
                      s.feed_ns = feed_ns;
                      s.order_ns = order_ns;
                      s.fill_ns = fill_ns;
                      return s; }),
           py::arg("feed_ns") = 0,
           py::arg("order_ns") = 0,
           py::arg("fill_ns") = 0)
      .def_readwrite("feed_ns", &flox::LatencySample::feed_ns)
      .def_readwrite("order_ns", &flox::LatencySample::order_ns)
      .def_readwrite("fill_ns", &flox::LatencySample::fill_ns)
      .def("to_dict", [](const flox::LatencySample& s)
           {
             py::dict d;
             d["feed_ns"] = s.feed_ns;
             d["order_ns"] = s.order_ns;
             d["fill_ns"] = s.fill_ns;
             return d; });

  // Trampoline so Python subclasses can override delays.
  class PyLatencyModel : public flox::LatencyModel
  {
   public:
    using flox::LatencyModel::LatencyModel;
    int64_t feedDelay() override
    {
      PYBIND11_OVERRIDE_PURE(int64_t, flox::LatencyModel, feedDelay);
    }
    int64_t orderDelay() override
    {
      PYBIND11_OVERRIDE_PURE(int64_t, flox::LatencyModel, orderDelay);
    }
    int64_t fillDelay() override
    {
      PYBIND11_OVERRIDE_PURE(int64_t, flox::LatencyModel, fillDelay);
    }
    void reset(uint64_t seed) override
    {
      PYBIND11_OVERRIDE(void, flox::LatencyModel, reset, seed);
    }
  };

  py::class_<flox::LatencyModel, PyLatencyModel,
             std::shared_ptr<flox::LatencyModel>>(m, "LatencyModel",
      "Abstract sampler. Subclasses implement feed_delay / order_delay "
      "/ fill_delay returning non-negative nanoseconds.")
      .def(py::init<>())
      .def("feed_delay", &flox::LatencyModel::feedDelay)
      .def("order_delay", &flox::LatencyModel::orderDelay)
      .def("fill_delay", &flox::LatencyModel::fillDelay)
      .def("sample", &flox::LatencyModel::sample,
           "Composite draw. Returns a LatencySample.")
      .def("reset", &flox::LatencyModel::reset, py::arg("seed") = 0,
           "Re-seed the underlying RNG. No-op for deterministic models.");

  py::class_<flox::ConstantLatency, flox::LatencyModel,
             std::shared_ptr<flox::ConstantLatency>>(m, "ConstantLatency",
      "Returns the same nanoseconds every call. Useful as a baseline.")
      .def(py::init<int64_t, int64_t, int64_t>(),
           py::arg("feed_ns") = 0,
           py::arg("order_ns") = 0,
           py::arg("fill_ns") = 0);

  py::class_<flox::GaussianLatency, flox::LatencyModel,
             std::shared_ptr<flox::GaussianLatency>>(m, "GaussianLatency",
      "Independent normal samples per component, clamped to "
      "non-negative. Stddev <= 0 collapses the component to a "
      "deterministic mean.")
      .def(py::init<double, double, double, double, double, double, uint64_t>(),
           py::arg("feed_mean_ns") = 0.0,
           py::arg("feed_stddev_ns") = 0.0,
           py::arg("order_mean_ns") = 0.0,
           py::arg("order_stddev_ns") = 0.0,
           py::arg("fill_mean_ns") = 0.0,
           py::arg("fill_stddev_ns") = 0.0,
           py::arg("seed") = 0);

  py::class_<flox::ExponentialLatency, flox::LatencyModel,
             std::shared_ptr<flox::ExponentialLatency>>(m, "ExponentialLatency",
      "Exponential per component, parameterised by mean. Heavy right "
      "tail by default.")
      .def(py::init<double, double, double, uint64_t>(),
           py::arg("feed_mean_ns") = 0.0,
           py::arg("order_mean_ns") = 0.0,
           py::arg("fill_mean_ns") = 0.0,
           py::arg("seed") = 0);

  py::class_<flox::EmpiricalLatency, flox::LatencyModel,
             std::shared_ptr<flox::EmpiricalLatency>>(m, "EmpiricalLatency",
      "Resample with replacement from observed values. Pass three "
      "lists of measured latencies (one per component).")
      .def(py::init<std::vector<int64_t>, std::vector<int64_t>,
                    std::vector<int64_t>, uint64_t>(),
           py::arg("feed_samples") = std::vector<int64_t>{},
           py::arg("order_samples") = std::vector<int64_t>{},
           py::arg("fill_samples") = std::vector<int64_t>{},
           py::arg("seed") = 0);
}

}  // namespace flox_py
