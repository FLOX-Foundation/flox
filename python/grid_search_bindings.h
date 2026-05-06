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

#include "flox/backtest/grid_search.h"
#include "flox/error/flox_error.h"
#include "walk_forward_bindings.h"  // for statsToDict

#include <utility>
#include <vector>

namespace py = pybind11;

namespace flox_py
{

class PyGridSearch
{
 public:
  PyGridSearch() = default;

  void add_axis(std::vector<double> values)
  {
    _gs.addAxis(std::move(values));
  }

  void set_factory(py::object factory)
  {
    _factory = std::move(factory);
  }

  std::size_t total() const { return _gs.totalCombinations(); }

  std::vector<double> params_for_index(std::size_t idx) const
  {
    return _gs.paramsForIndex(idx);
  }

  py::list run()
  {
    if (!_factory || _factory.is_none())
    {
      throw flox::FloxError(
          "E_RUN_001",
          "GridSearch.run() called before set_factory().");
    }
    const std::size_t total = _gs.totalCombinations();
    if (total == 0)
    {
      throw flox::FloxError(
          "E_RUN_002",
          "GridSearch.run() called with no axes / empty axis. "
          "Add at least one non-empty axis via add_axis(values).");
    }
    py::list out;
    for (std::size_t i = 0; i < total; ++i)
    {
      auto params = _gs.paramsForIndex(i);
      // Factory contract: takes list[float], returns dict[str, Any]
      // matching BacktestRunner.run_csv()'s stats schema.
      py::list pyParams;
      for (double v : params)
      {
        pyParams.append(v);
      }
      py::object res = _factory(pyParams);
      if (!py::isinstance<py::dict>(res))
      {
        throw flox::FloxError(
            "E_RUN_003",
            "GridSearch factory must return a dict (BacktestRunner stats); "
            "got something else.");
      }
      py::dict d;
      d["index"] = i;
      d["params"] = pyParams;
      d["stats"] = res;
      out.append(d);
    }
    return out;
  }

 private:
  flox::GridSearch _gs;
  py::object _factory;
};

inline void bindGridSearch(py::module_& m)
{
  py::class_<PyGridSearch>(m, "GridSearch")
      .def(py::init<>())
      .def("add_axis", &PyGridSearch::add_axis, py::arg("values"),
           "Append an axis of parameter values. Total combinations is "
           "the product of axis lengths; the last axis varies fastest.")
      .def("set_factory", &PyGridSearch::set_factory, py::arg("factory"),
           "Callable[[List[float]], Dict[str, Any]] — receives one "
           "parameter point in axis order, returns a stats dict (the "
           "shape returned by BacktestRunner.run_csv).")
      .def("total", &PyGridSearch::total)
      .def("params_for_index", &PyGridSearch::params_for_index,
           py::arg("index"))
      .def("run", &PyGridSearch::run,
           "Run the grid sequentially. Returns a list of dicts with "
           "keys 'index', 'params', 'stats'.");
}

}  // namespace flox_py
