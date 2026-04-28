#pragma once

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstring>

#include "flox/target/future_ctc_volatility.h"
#include "flox/target/future_linear_slope.h"
#include "flox/target/future_return.h"

namespace py = pybind11;

namespace
{

using contiguous_double_target =
    py::array_t<double, py::array::c_style | py::array::forcecast>;

template <typename Target>
py::array_t<double> computeTarget(const Target& t, contiguous_double_target input)
{
  auto buf = input.request();
  size_t n = buf.shape[0];
  auto* ptr = static_cast<const double*>(buf.ptr);
  py::array_t<double> result(n);
  auto* out = result.mutable_data();
  {
    py::gil_scoped_release release;
    t.compute(std::span<const double>(ptr, n), std::span<double>(out, n));
  }
  return result;
}

}  // namespace

inline void bindTargets(py::module_& parent)
{
  auto m = parent.def_submodule(
      "targets",
      "Forward-looking labels (research-only). Distinct from indicators: feeding "
      "these into a live update loop is a look-ahead-bias bug.");

  m.def(
      "future_return",
      [](contiguous_double_target close, size_t horizon)
      { return computeTarget(flox::target::FutureReturn(horizon), close); },
      py::arg("close"), py::arg("horizon"));

  m.def(
      "future_ctc_volatility",
      [](contiguous_double_target close, size_t horizon)
      { return computeTarget(flox::target::FutureCTCVolatility(horizon), close); },
      py::arg("close"), py::arg("horizon"));

  m.def(
      "future_linear_slope",
      [](contiguous_double_target close, size_t horizon)
      { return computeTarget(flox::target::FutureLinearSlope(horizon), close); },
      py::arg("close"), py::arg("horizon"));
}
