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

#include <memory>
#include <vector>

#include "lrvx/backtest/latency_model.h"

namespace py = pybind11;

namespace lrvx
{

inline void bindLatencyModels(py::module_& m)
{
  py::class_<lrvx::LatencySample>(m, "LatencySample",
                                  "One draw from a LatencyModel covering feed, order, and fill in "
                                  "non-negative nanoseconds.")
      .def(py::init<>())
      .def(py::init([](int64_t feed_ns, int64_t order_ns, int64_t fill_ns)
                    {
                      lrvx::LatencySample s;
                      s.feed_ns = feed_ns;
                      s.order_ns = order_ns;
                      s.fill_ns = fill_ns;
                      return s; }),
           py::arg("feed_ns") = 0,
           py::arg("order_ns") = 0,
           py::arg("fill_ns") = 0)
      .def_readwrite("feed_ns", &lrvx::LatencySample::feed_ns)
      .def_readwrite("order_ns", &lrvx::LatencySample::order_ns)
      .def_readwrite("fill_ns", &lrvx::LatencySample::fill_ns)
      .def("to_dict", [](const lrvx::LatencySample& s)
           {
             py::dict d;
             d["feed_ns"] = s.feed_ns;
             d["order_ns"] = s.order_ns;
             d["fill_ns"] = s.fill_ns;
             return d; });

  // Trampoline so Python subclasses can override delays.
  class PyLatencyModel : public lrvx::LatencyModel
  {
   public:
    using lrvx::LatencyModel::LatencyModel;
    int64_t feedDelay() override
    {
      PYBIND11_OVERRIDE_PURE(int64_t, lrvx::LatencyModel, feedDelay);
    }
    int64_t orderDelay() override
    {
      PYBIND11_OVERRIDE_PURE(int64_t, lrvx::LatencyModel, orderDelay);
    }
    int64_t fillDelay() override
    {
      PYBIND11_OVERRIDE_PURE(int64_t, lrvx::LatencyModel, fillDelay);
    }
    void reset(uint64_t seed) override
    {
      PYBIND11_OVERRIDE(void, lrvx::LatencyModel, reset, seed);
    }
  };

  py::class_<lrvx::LatencyModel, PyLatencyModel,
             std::shared_ptr<lrvx::LatencyModel>>(m, "LatencyModel",
                                                  "Abstract sampler. Subclasses implement feed_delay / order_delay "
                                                  "/ fill_delay returning non-negative nanoseconds.")
      .def(py::init<>())
      .def("feed_delay", &lrvx::LatencyModel::feedDelay)
      .def("order_delay", &lrvx::LatencyModel::orderDelay)
      .def("fill_delay", &lrvx::LatencyModel::fillDelay)
      .def("sample", &lrvx::LatencyModel::sample,
           "Composite draw. Returns a LatencySample.")
      .def("reset", &lrvx::LatencyModel::reset, py::arg("seed") = 0,
           "Re-seed the underlying RNG. No-op for deterministic models.");

  py::class_<lrvx::ConstantLatency, lrvx::LatencyModel,
             std::shared_ptr<lrvx::ConstantLatency>>(m, "ConstantLatency",
                                                     "Returns the same nanoseconds every call. Useful as a baseline.")
      .def(py::init<int64_t, int64_t, int64_t>(),
           py::arg("feed_ns") = 0,
           py::arg("order_ns") = 0,
           py::arg("fill_ns") = 0);

  py::class_<lrvx::GaussianLatency, lrvx::LatencyModel,
             std::shared_ptr<lrvx::GaussianLatency>>(m, "GaussianLatency",
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

  py::class_<lrvx::ExponentialLatency, lrvx::LatencyModel,
             std::shared_ptr<lrvx::ExponentialLatency>>(m, "ExponentialLatency",
                                                        "Exponential per component, parameterised by mean. Heavy right "
                                                        "tail by default.")
      .def(py::init<double, double, double, uint64_t>(),
           py::arg("feed_mean_ns") = 0.0,
           py::arg("order_mean_ns") = 0.0,
           py::arg("fill_mean_ns") = 0.0,
           py::arg("seed") = 0);

  py::class_<lrvx::EmpiricalLatency, lrvx::LatencyModel,
             std::shared_ptr<lrvx::EmpiricalLatency>>(m, "EmpiricalLatency",
                                                      "Resample with replacement from observed values. Pass three "
                                                      "lists of measured latencies (one per component).")
      .def(py::init<std::vector<int64_t>, std::vector<int64_t>,
                    std::vector<int64_t>, uint64_t>(),
           py::arg("feed_samples") = std::vector<int64_t>{},
           py::arg("order_samples") = std::vector<int64_t>{},
           py::arg("fill_samples") = std::vector<int64_t>{},
           py::arg("seed") = 0);
}

}  // namespace lrvx
