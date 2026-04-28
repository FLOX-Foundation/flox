#pragma once

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "flox/aggregator/bar.h"
#include "flox/common.h"
#include "flox/indicator/indicator_pipeline.h"

namespace py = pybind11;

namespace
{

using contiguous_double_graph =
    py::array_t<double, py::array::c_style | py::array::forcecast>;

// Wraps flox::indicator::IndicatorGraph for Python. Single-symbol path is
// the primary use; multi-symbol works by passing distinct symbol IDs.
class PyIndicatorGraph
{
 public:
  PyIndicatorGraph() = default;

  void setBars(uint32_t symbol, contiguous_double_graph close,
               std::optional<contiguous_double_graph> high,
               std::optional<contiguous_double_graph> low,
               std::optional<contiguous_double_graph> volume)
  {
    auto cBuf = close.request();
    size_t n = cBuf.shape[0];
    const double* c = static_cast<const double*>(cBuf.ptr);
    const double* h = c;
    const double* l = c;
    const double* v = nullptr;
    if (high)
    {
      auto b = high->request();
      if (static_cast<size_t>(b.shape[0]) != n)
      {
        throw std::invalid_argument("set_bars: high must match close length");
      }
      h = static_cast<const double*>(b.ptr);
    }
    if (low)
    {
      auto b = low->request();
      if (static_cast<size_t>(b.shape[0]) != n)
      {
        throw std::invalid_argument("set_bars: low must match close length");
      }
      l = static_cast<const double*>(b.ptr);
    }
    if (volume)
    {
      auto b = volume->request();
      if (static_cast<size_t>(b.shape[0]) != n)
      {
        throw std::invalid_argument("set_bars: volume must match close length");
      }
      v = static_cast<const double*>(b.ptr);
    }

    std::vector<flox::Bar> bars(n);
    for (size_t i = 0; i < n; ++i)
    {
      bars[i].open = flox::Price::fromDouble(c[i]);
      bars[i].high = flox::Price::fromDouble(h[i]);
      bars[i].low = flox::Price::fromDouble(l[i]);
      bars[i].close = flox::Price::fromDouble(c[i]);
      bars[i].volume = v ? flox::Volume::fromDouble(v[i]) : flox::Volume{};
    }
    _bars[symbol] = std::move(bars);
    _graph.setBars(static_cast<flox::SymbolId>(symbol),
                   std::span<const flox::Bar>(_bars[symbol]));
  }

  void addNode(const std::string& name, std::vector<std::string> deps, py::object fn)
  {
    PyIndicatorGraph* self = this;
    _graph.addNode(
        name, std::move(deps),
        [self, fn](flox::indicator::IndicatorGraph&, flox::SymbolId sym) -> std::vector<double>
        {
          py::object result = fn(py::cast(self, py::return_value_policy::reference),
                                 static_cast<uint32_t>(sym));
          py::array_t<double, py::array::c_style | py::array::forcecast> arr =
              result.cast<py::array_t<double, py::array::c_style | py::array::forcecast>>();
          auto buf = arr.request();
          size_t n = buf.shape[0];
          const double* p = static_cast<const double*>(buf.ptr);
          return std::vector<double>(p, p + n);
        });
  }

  py::array_t<double> require(uint32_t symbol, const std::string& name)
  {
    const auto& v = _graph.require(static_cast<flox::SymbolId>(symbol), name);
    return toArray(v);
  }

  py::object get(uint32_t symbol, const std::string& name) const
  {
    const auto* v = _graph.get(static_cast<flox::SymbolId>(symbol), name);
    if (!v)
    {
      return py::none();
    }
    return toArray(*v);
  }

  py::array_t<double> close(uint32_t symbol)
  {
    return toArray(_graph.close(static_cast<flox::SymbolId>(symbol)));
  }
  py::array_t<double> high(uint32_t symbol)
  {
    return toArray(_graph.high(static_cast<flox::SymbolId>(symbol)));
  }
  py::array_t<double> low(uint32_t symbol)
  {
    return toArray(_graph.low(static_cast<flox::SymbolId>(symbol)));
  }
  py::array_t<double> volume(uint32_t symbol)
  {
    return toArray(_graph.volume(static_cast<flox::SymbolId>(symbol)));
  }

  void invalidate(uint32_t symbol) { _graph.invalidate(static_cast<flox::SymbolId>(symbol)); }
  void invalidateAll() { _graph.invalidateAll(); }

 private:
  static py::array_t<double> toArray(const std::vector<double>& v)
  {
    py::array_t<double> out(v.size());
    std::memcpy(out.mutable_data(), v.data(), v.size() * sizeof(double));
    return out;
  }

  flox::indicator::IndicatorGraph _graph;
  std::unordered_map<uint32_t, std::vector<flox::Bar>> _bars;
};

}  // namespace

inline void bindIndicatorGraph(py::module_& m)
{
  py::class_<PyIndicatorGraph>(m, "IndicatorGraph")
      .def(py::init<>())
      .def("set_bars", &PyIndicatorGraph::setBars, py::arg("symbol"), py::arg("close"),
           py::arg("high") = py::none(), py::arg("low") = py::none(),
           py::arg("volume") = py::none())
      .def("add_node", &PyIndicatorGraph::addNode, py::arg("name"), py::arg("deps"),
           py::arg("fn"))
      .def("require", &PyIndicatorGraph::require, py::arg("symbol"), py::arg("name"))
      .def("get", &PyIndicatorGraph::get, py::arg("symbol"), py::arg("name"))
      .def("close", &PyIndicatorGraph::close, py::arg("symbol"))
      .def("high", &PyIndicatorGraph::high, py::arg("symbol"))
      .def("low", &PyIndicatorGraph::low, py::arg("symbol"))
      .def("volume", &PyIndicatorGraph::volume, py::arg("symbol"))
      .def("invalidate", &PyIndicatorGraph::invalidate, py::arg("symbol"))
      .def("invalidate_all", &PyIndicatorGraph::invalidateAll);
}
