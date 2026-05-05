#pragma once

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

#include <pybind11/stl.h>
#include "flox/error/flox_error.h"

#include <cstring>
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

// Single Python class for the DAG. Same instance exposes BOTH:
//   - batch path: set_bars(...) / require(...) / get(...) / field accessors
//   - streaming path: step(...) / current(...) / bar_count(...) / reset(...)
// Same nodes serve both. Same cache. Same invalidation rules. There is no
// separate StreamingIndicatorGraph type.
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
        throw flox::FloxError(
            "E_LEN_001",
            "set_bars: 'high' array must have the same length as 'close'.");
      }
      h = static_cast<const double*>(b.ptr);
    }
    if (low)
    {
      auto b = low->request();
      if (static_cast<size_t>(b.shape[0]) != n)
      {
        throw flox::FloxError(
            "E_LEN_001",
            "set_bars: 'low' array must have the same length as 'close'.");
      }
      l = static_cast<const double*>(b.ptr);
    }
    if (volume)
    {
      auto b = volume->request();
      if (static_cast<size_t>(b.shape[0]) != n)
      {
        throw flox::FloxError(
            "E_LEN_001",
            "set_bars: 'volume' array must have the same length as 'close'.");
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
    _graph.setBars(static_cast<flox::SymbolId>(symbol),
                   std::span<const flox::Bar>(bars));
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

  // ── Streaming path on the same graph ─────────────────────────────
  void step(uint32_t symbol, double close, std::optional<double> high,
            std::optional<double> low, std::optional<double> volume)
  {
    flox::Bar bar;
    double h = high.value_or(close);
    double l = low.value_or(close);
    double v = volume.value_or(0.0);
    bar.open = flox::Price::fromDouble(close);
    bar.high = flox::Price::fromDouble(h);
    bar.low = flox::Price::fromDouble(l);
    bar.close = flox::Price::fromDouble(close);
    bar.volume = flox::Volume::fromDouble(v);
    _graph.step(static_cast<flox::SymbolId>(symbol), bar);
  }

  double current(uint32_t symbol, const std::string& name)
  {
    return _graph.current(static_cast<flox::SymbolId>(symbol), name);
  }

  size_t barCount(uint32_t symbol) const
  {
    return _graph.barCount(static_cast<flox::SymbolId>(symbol));
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
  void reset(uint32_t symbol) { _graph.reset(static_cast<flox::SymbolId>(symbol)); }
  void resetAll() { _graph.resetAll(); }

 private:
  static py::array_t<double> toArray(const std::vector<double>& v)
  {
    py::array_t<double> out(v.size());
    std::memcpy(out.mutable_data(), v.data(), v.size() * sizeof(double));
    return out;
  }

  flox::indicator::IndicatorGraph _graph;
};

}  // namespace

inline void bindIndicatorGraph(py::module_& m)
{
  auto cls = py::class_<PyIndicatorGraph>(m, "IndicatorGraph");
  cls.def(py::init<>())
      .def("set_bars", &PyIndicatorGraph::setBars, py::arg("symbol"), py::arg("close"),
           py::arg("high") = py::none(), py::arg("low") = py::none(),
           py::arg("volume") = py::none())
      .def("add_node", &PyIndicatorGraph::addNode, py::arg("name"), py::arg("deps"),
           py::arg("fn"))
      .def("require", &PyIndicatorGraph::require, py::arg("symbol"), py::arg("name"))
      .def("get", &PyIndicatorGraph::get, py::arg("symbol"), py::arg("name"))
      // Streaming path on the same instance — no separate type.
      .def("step", &PyIndicatorGraph::step, py::arg("symbol"), py::arg("close"),
           py::arg("high") = py::none(), py::arg("low") = py::none(),
           py::arg("volume") = py::none())
      .def("current", &PyIndicatorGraph::current, py::arg("symbol"), py::arg("name"))
      .def("bar_count", &PyIndicatorGraph::barCount, py::arg("symbol"))
      .def("close", &PyIndicatorGraph::close, py::arg("symbol"))
      .def("high", &PyIndicatorGraph::high, py::arg("symbol"))
      .def("low", &PyIndicatorGraph::low, py::arg("symbol"))
      .def("volume", &PyIndicatorGraph::volume, py::arg("symbol"))
      .def("invalidate", &PyIndicatorGraph::invalidate, py::arg("symbol"))
      .def("invalidate_all", &PyIndicatorGraph::invalidateAll)
      .def("reset", &PyIndicatorGraph::reset, py::arg("symbol"))
      .def("reset_all", &PyIndicatorGraph::resetAll);

  // Declarative helper. Sugar over add_node:
  //
  //   g.indicator("ema5", flox.EMA(5), source="close")
  //
  // is equivalent to:
  //
  //   g.add_node("ema5", [], lambda gr, sym: ind.compute(gr.<source>(sym)))
  //
  // The indicator object is moved into the closure; pass a fresh instance
  // per call.
  cls.def(
      "indicator",
      [](PyIndicatorGraph& self, const std::string& name, py::object indicator_obj,
         const std::string& source)
      {
        auto fn = py::cpp_function(
            [indicator_obj, source](py::object graph, uint32_t sym) -> py::object
            {
              auto field = graph.attr(source.c_str())(sym);
              return indicator_obj.attr("compute")(field);
            });
        self.addNode(name, {}, fn);
      },
      py::arg("name"), py::arg("indicator"), py::arg("source") = "close",
      "Add a node that runs `indicator.compute(graph.<source>(sym))`. "
      "Sugar over add_node.");

  // Backward-compat alias: the old StreamingIndicatorGraph name resolves to
  // the same class. Will be removed in a future major version. New code
  // should use IndicatorGraph directly.
  m.attr("StreamingIndicatorGraph") = m.attr("IndicatorGraph");
}
