#pragma once

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

#include "flox/indicator/adf.h"
#include "flox/indicator/adx.h"
#include "flox/indicator/atr.h"
#include "flox/indicator/autocorrelation.h"
#include "flox/indicator/bollinger.h"
#include "flox/indicator/cci.h"
#include "flox/indicator/chop.h"
#include "flox/indicator/correlation.h"
#include "flox/indicator/cvd.h"
#include "flox/indicator/dema.h"
#include "flox/indicator/ema.h"
#include "flox/indicator/kama.h"
#include "flox/indicator/kurtosis.h"
#include "flox/indicator/macd.h"
#include "flox/indicator/obv.h"
#include "flox/indicator/parkinson_vol.h"
#include "flox/indicator/rma.h"
#include "flox/indicator/rogers_satchell_vol.h"
#include "flox/indicator/rolling_zscore.h"
#include "flox/indicator/rsi.h"
#include "flox/indicator/shannon_entropy.h"
#include "flox/indicator/skewness.h"
#include "flox/indicator/slope.h"
#include "flox/indicator/sma.h"
#include "flox/indicator/stochastic.h"
#include "flox/indicator/vwap.h"

namespace py = pybind11;

namespace
{

inline void checkSameSize(size_t a, size_t b, const char* msg)
{
  if (a != b)
  {
    throw std::invalid_argument(msg);
  }
}

using contiguous_double = py::array_t<double, py::array::c_style | py::array::forcecast>;
using contiguous_int8 = py::array_t<int8_t, py::array::c_style | py::array::forcecast>;
using contiguous_uint8 = py::array_t<uint8_t, py::array::c_style | py::array::forcecast>;

template <typename Indicator>
py::array_t<double> computeSingle(const Indicator& ind, contiguous_double input)
{
  auto buf = input.request();
  auto* ptr = static_cast<const double*>(buf.ptr);
  size_t n = buf.shape[0];

  py::array_t<double> result(n);
  auto out = result.mutable_data();
  {
    py::gil_scoped_release release;
    ind.compute(std::span<const double>(ptr, n), std::span<double>(out, n));
  }
  return result;
}

// Optional<float> wrapper used by streaming indicator bindings: returns
// std::nullopt when the indicator is not yet ready (`value()` is NaN).
// pybind11 maps std::optional<double> to `Optional[float]` in stubs, so
// the type checker sees the real return shape instead of `Any`.
inline std::optional<double> optFloat(double v)
{
  return std::isnan(v) ? std::nullopt : std::optional<double>{v};
}

// ── Helper templates: bind a C++ indicator class with both compute() and
// streaming methods (update/value/ready/reset) on the same Python type.
// One indicator class → one Python type → both APIs on the same instance.

template <typename T>
auto bindSingleIndicator(py::module_& m, const char* name)
{
  return py::class_<T>(m, name)
      .def("compute",
           [](const T& self, contiguous_double arr)
           {
             auto buf = arr.request();
             auto* p = static_cast<const double*>(buf.ptr);
             size_t n = buf.shape[0];
             std::vector<double> result;
             {
               py::gil_scoped_release release;
               result = self.compute(std::span<const double>(p, n));
             }
             py::array_t<double> out(result.size());
             std::memcpy(out.mutable_data(), result.data(), result.size() * sizeof(double));
             return out;
           })
      .def("update", [](T& self, double v) -> std::optional<double>
           { self.update(v); return optFloat(self.value()); }, py::arg("value"))
      .def("reset", &T::reset)
      .def_property_readonly("value", [](const T& self)
                             { return optFloat(self.value()); })
      .def_property_readonly("ready", [](const T& self)
                             { return self.ready(); })
      .def_property_readonly("count", [](const T& self)
                             { return self.count(); });
}

template <typename T>
auto bindBarIndicator(py::module_& m, const char* name)
{
  return py::class_<T>(m, name)
      .def("compute",
           [](const T& self, contiguous_double high, contiguous_double low,
              contiguous_double close)
           {
             size_t n = high.request().shape[0];
             checkSameSize(n, low.request().shape[0], "high and low size");
             checkSameSize(n, close.request().shape[0], "high and close size");
             auto* h = high.data();
             auto* l = low.data();
             auto* c = close.data();
             std::vector<double> result;
             {
               py::gil_scoped_release release;
               result = self.compute(std::span<const double>(h, n),
                                     std::span<const double>(l, n),
                                     std::span<const double>(c, n));
             }
             py::array_t<double> out(result.size());
             std::memcpy(out.mutable_data(), result.data(), result.size() * sizeof(double));
             return out;
           })
      .def("update", [](T& self, double high, double low, double close) -> std::optional<double>
           { self.update(high, low, close); return optFloat(self.value()); }, py::arg("high"), py::arg("low"), py::arg("close"))
      .def("reset", &T::reset)
      .def_property_readonly("value", [](const T& self)
                             { return optFloat(self.value()); })
      .def_property_readonly("ready", [](const T& self)
                             { return self.ready(); })
      .def_property_readonly("count", [](const T& self)
                             { return self.count(); });
}

template <typename T>
auto bindHighLowIndicator(py::module_& m, const char* name)
{
  return py::class_<T>(m, name)
      .def("compute",
           [](const T& self, contiguous_double high, contiguous_double low)
           {
             size_t n = high.request().shape[0];
             checkSameSize(n, low.request().shape[0], "high and low size");
             auto* h = high.data();
             auto* l = low.data();
             std::vector<double> result;
             {
               py::gil_scoped_release release;
               result = self.compute(std::span<const double>(h, n),
                                     std::span<const double>(l, n));
             }
             py::array_t<double> out(result.size());
             std::memcpy(out.mutable_data(), result.data(), result.size() * sizeof(double));
             return out;
           })
      .def("update", [](T& self, double high, double low) -> std::optional<double>
           { self.update(high, low); return optFloat(self.value()); }, py::arg("high"), py::arg("low"))
      .def("reset", &T::reset)
      .def_property_readonly("value", [](const T& self)
                             { return optFloat(self.value()); })
      .def_property_readonly("ready", [](const T& self)
                             { return self.ready(); })
      .def_property_readonly("count", [](const T& self)
                             { return self.count(); });
}

template <typename T>
auto bindOhlcIndicator(py::module_& m, const char* name)
{
  return py::class_<T>(m, name)
      .def("compute",
           [](const T& self, contiguous_double open, contiguous_double high,
              contiguous_double low, contiguous_double close)
           {
             size_t n = open.request().shape[0];
             checkSameSize(n, high.request().shape[0], "ohlc size");
             checkSameSize(n, low.request().shape[0], "ohlc size");
             checkSameSize(n, close.request().shape[0], "ohlc size");
             auto* o = open.data();
             auto* h = high.data();
             auto* l = low.data();
             auto* c = close.data();
             std::vector<double> result;
             {
               py::gil_scoped_release release;
               result = self.compute(std::span<const double>(o, n),
                                     std::span<const double>(h, n),
                                     std::span<const double>(l, n),
                                     std::span<const double>(c, n));
             }
             py::array_t<double> out(result.size());
             std::memcpy(out.mutable_data(), result.data(), result.size() * sizeof(double));
             return out;
           })
      .def("update", [](T& self, double open, double high, double low, double close) -> std::optional<double>
           { self.update(open, high, low, close); return optFloat(self.value()); }, py::arg("open"), py::arg("high"), py::arg("low"), py::arg("close"))
      .def("reset", &T::reset)
      .def_property_readonly("value", [](const T& self)
                             { return optFloat(self.value()); })
      .def_property_readonly("ready", [](const T& self)
                             { return self.ready(); })
      .def_property_readonly("count", [](const T& self)
                             { return self.count(); });
}

template <typename T>
auto bindPairIndicator(py::module_& m, const char* name)
{
  return py::class_<T>(m, name)
      .def("compute",
           [](const T& self, contiguous_double x, contiguous_double y)
           {
             size_t n = x.request().shape[0];
             checkSameSize(n, y.request().shape[0], "x and y size");
             auto* xp = x.data();
             auto* yp = y.data();
             std::vector<double> result;
             {
               py::gil_scoped_release release;
               result = self.compute(std::span<const double>(xp, n),
                                     std::span<const double>(yp, n));
             }
             py::array_t<double> out(result.size());
             std::memcpy(out.mutable_data(), result.data(), result.size() * sizeof(double));
             return out;
           })
      .def("update", [](T& self, double x, double y) -> std::optional<double>
           { self.update(x, y); return optFloat(self.value()); }, py::arg("x"), py::arg("y"))
      .def("reset", &T::reset)
      .def_property_readonly("value", [](const T& self)
                             { return optFloat(self.value()); })
      .def_property_readonly("ready", [](const T& self)
                             { return self.ready(); })
      .def_property_readonly("count", [](const T& self)
                             { return self.count(); });
}

}  // namespace

inline void bindIndicators(py::module_& m)
{
  m.def(
      "ema", [](contiguous_double input, size_t period)
      { return computeSingle(flox::indicator::EMA(period), input); },
      py::arg("input"), py::arg("period"));

  m.def(
      "sma", [](contiguous_double input, size_t period)
      { return computeSingle(flox::indicator::SMA(period), input); },
      py::arg("input"), py::arg("period"));

  m.def(
      "rsi", [](contiguous_double input, size_t period)
      { return computeSingle(flox::indicator::RSI(period), input); },
      py::arg("input"), py::arg("period"));

  m.def(
      "atr",
      [](contiguous_double high, contiguous_double low, contiguous_double close,
         size_t period) -> py::array_t<double>
      {
        size_t n = high.request().shape[0];
        checkSameSize(n, low.request().shape[0], "high and low must have same size");
        checkSameSize(n, close.request().shape[0], "high and close must have same size");
        auto* h = high.data();
        auto* l = low.data();
        auto* c = close.data();
        py::array_t<double> result(n);
        auto* o = result.mutable_data();
        {
          py::gil_scoped_release release;
          flox::indicator::ATR(period).compute(
              std::span<const double>(h, n), std::span<const double>(l, n),
              std::span<const double>(c, n), std::span<double>(o, n));
        }
        return result;
      },
      py::arg("high"), py::arg("low"), py::arg("close"), py::arg("period"));

  m.def(
      "macd",
      [](contiguous_double input, size_t fast, size_t slow, size_t signal) -> py::dict
      {
        size_t n = input.request().shape[0];
        auto* inp = input.data();
        py::array_t<double> line(n), sig(n), hist(n);
        auto* lp = line.mutable_data();
        auto* sp = sig.mutable_data();
        auto* hp = hist.mutable_data();
        {
          py::gil_scoped_release release;
          flox::indicator::MACD(fast, slow, signal)
              .compute(std::span<const double>(inp, n),
                       std::span<double>(lp, n), std::span<double>(sp, n), std::span<double>(hp, n));
        }
        py::dict d;
        d["line"] = line;
        d["signal"] = sig;
        d["histogram"] = hist;
        return d;
      },
      py::arg("input"), py::arg("fast") = 12, py::arg("slow") = 26, py::arg("signal") = 9);

  m.def(
      "slope", [](contiguous_double input, size_t length)
      { return computeSingle(flox::indicator::Slope(length), input); },
      py::arg("input"), py::arg("length") = 1);

  m.def(
      "kama", [](contiguous_double input, size_t period)
      { return computeSingle(flox::indicator::KAMA(period), input); },
      py::arg("input"), py::arg("period") = 10);

  m.def(
      "obv",
      [](contiguous_double close, contiguous_double volume) -> py::array_t<double>
      {
        size_t n = close.request().shape[0];
        checkSameSize(n, volume.request().shape[0], "close and volume must have same size");
        auto* c = close.data();
        auto* v = volume.data();
        py::array_t<double> result(n);
        auto* o = result.mutable_data();
        {
          py::gil_scoped_release release;
          flox::indicator::OBV().compute(
              std::span<const double>(c, n), std::span<const double>(v, n), std::span<double>(o, n));
        }
        return result;
      },
      py::arg("close"), py::arg("volume"));

  m.def(
      "adx",
      [](contiguous_double high, contiguous_double low, contiguous_double close,
         size_t period) -> py::dict
      {
        size_t n = high.request().shape[0];
        checkSameSize(n, low.request().shape[0], "arrays must have same size");
        checkSameSize(n, close.request().shape[0], "arrays must have same size");
        auto* h = high.data();
        auto* l = low.data();
        auto* c = close.data();
        flox::indicator::AdxResult result;
        {
          py::gil_scoped_release release;
          result = flox::indicator::ADX(period).compute(
              std::span<const double>(h, n), std::span<const double>(l, n),
              std::span<const double>(c, n));
        }
        py::array_t<double> adx_out(n), pdi(n), ndi(n);
        std::memcpy(adx_out.mutable_data(), result.adx.data(), n * sizeof(double));
        std::memcpy(pdi.mutable_data(), result.plus_di.data(), n * sizeof(double));
        std::memcpy(ndi.mutable_data(), result.minus_di.data(), n * sizeof(double));
        py::dict d;
        d["adx"] = adx_out;
        d["plus_di"] = pdi;
        d["minus_di"] = ndi;
        return d;
      },
      py::arg("high"), py::arg("low"), py::arg("close"), py::arg("period") = 14);

  m.def(
      "chop",
      [](contiguous_double high, contiguous_double low, contiguous_double close,
         size_t period) -> py::array_t<double>
      {
        size_t n = high.request().shape[0];
        checkSameSize(n, low.request().shape[0], "arrays must have same size");
        checkSameSize(n, close.request().shape[0], "arrays must have same size");
        auto* h = high.data();
        auto* l = low.data();
        auto* c = close.data();
        std::vector<double> result;
        {
          py::gil_scoped_release release;
          result = flox::indicator::CHOP(period).compute(
              std::span<const double>(h, n), std::span<const double>(l, n),
              std::span<const double>(c, n));
        }
        py::array_t<double> out(n);
        std::memcpy(out.mutable_data(), result.data(), n * sizeof(double));
        return out;
      },
      py::arg("high"), py::arg("low"), py::arg("close"), py::arg("period") = 14);

  m.def(
      "bollinger",
      [](contiguous_double input, size_t period, double stddev) -> py::dict
      {
        size_t n = input.request().shape[0];
        auto* inp = input.data();
        flox::indicator::BollingerResult result;
        {
          py::gil_scoped_release release;
          result = flox::indicator::Bollinger(period, stddev)
                       .compute(std::span<const double>(inp, n));
        }
        py::array_t<double> upper(n), middle(n), lower(n);
        std::memcpy(upper.mutable_data(), result.upper.data(), n * sizeof(double));
        std::memcpy(middle.mutable_data(), result.middle.data(), n * sizeof(double));
        std::memcpy(lower.mutable_data(), result.lower.data(), n * sizeof(double));
        py::dict d;
        d["upper"] = upper;
        d["middle"] = middle;
        d["lower"] = lower;
        return d;
      },
      py::arg("input"), py::arg("period") = 20, py::arg("stddev") = 2.0);

  m.def(
      "bar_returns",
      [](contiguous_int8 signal_long, contiguous_int8 signal_short,
         contiguous_double log_returns) -> py::array_t<double>
      {
        size_t n = signal_long.request().shape[0];
        auto* sl = signal_long.data();
        auto* ss = signal_short.data();
        auto* lr = log_returns.data();
        py::array_t<double> out(n);
        auto* o = out.mutable_data();
        o[0] = 0.0;
        for (size_t i = 1; i < n; ++i)
        {
          o[i] = static_cast<double>(sl[i - 1] + ss[i - 1]) * lr[i];
        }
        return out;
      },
      py::arg("signal_long"), py::arg("signal_short"), py::arg("log_returns"));

  m.def(
      "trade_pnl",
      [](contiguous_int8 signal_long, contiguous_int8 signal_short,
         contiguous_double log_returns) -> py::array_t<double>
      {
        size_t n = signal_long.request().shape[0];
        auto* sl = signal_long.data();
        auto* ss = signal_short.data();
        auto* lr = log_returns.data();
        std::vector<double> trades;
        double pnl = 0.0;
        int8_t prevPos = 0;
        bool inTrade = false;
        for (size_t i = 1; i < n; ++i)
        {
          int8_t pos = sl[i - 1] + ss[i - 1];
          double ret = static_cast<double>(pos) * lr[i];
          if (pos != prevPos)
          {
            if (inTrade)
            {
              trades.push_back(pnl);
              pnl = 0.0;
            }
            inTrade = (pos != 0);
          }
          if (pos != 0)
          {
            pnl += ret;
          }
          prevPos = pos;
        }
        if (inTrade)
        {
          trades.push_back(pnl);
        }
        py::array_t<double> out(trades.size());
        std::memcpy(out.mutable_data(), trades.data(), trades.size() * sizeof(double));
        return out;
      },
      py::arg("signal_long"), py::arg("signal_short"), py::arg("log_returns"));

  m.def(
      "profit_factor",
      [](contiguous_double returns) -> double
      {
        size_t n = returns.request().shape[0];
        auto* r = returns.data();
        double pos = 0.0, neg = 0.0;
        for (size_t i = 0; i < n; ++i)
        {
          if (r[i] > 0)
          {
            pos += r[i];
          }
          else if (r[i] < 0)
          {
            neg -= r[i];
          }
        }
        return neg > 0 ? pos / neg : (pos > 0 ? 999.0 : 0.0);
      },
      py::arg("returns"));

  m.def(
      "win_rate",
      [](contiguous_double trade_pnls) -> double
      {
        size_t n = trade_pnls.request().shape[0];
        if (n == 0)
        {
          return 0.0;
        }
        auto* p = trade_pnls.data();
        size_t wins = 0;
        for (size_t i = 0; i < n; ++i)
        {
          if (p[i] > 0)
          {
            ++wins;
          }
        }
        return static_cast<double>(wins) / n;
      },
      py::arg("trade_pnls"));

  m.def(
      "rma", [](contiguous_double input, size_t period)
      { return computeSingle(flox::indicator::RMA(period), input); },
      py::arg("input"), py::arg("period"));

  m.def(
      "dema", [](contiguous_double input, size_t period)
      {
        auto buf = input.request();
        size_t n = buf.shape[0];
        auto* ptr = static_cast<const double*>(buf.ptr);
        std::vector<double> result;
        { py::gil_scoped_release r; result = flox::indicator::DEMA(period).compute(std::span<const double>(ptr, n)); }
        py::array_t<double> out(n);
        std::memcpy(out.mutable_data(), result.data(), n * sizeof(double));
        return out; },
      py::arg("input"), py::arg("period"));

  m.def(
      "tema", [](contiguous_double input, size_t period)
      {
        auto buf = input.request();
        size_t n = buf.shape[0];
        auto* ptr = static_cast<const double*>(buf.ptr);
        std::vector<double> result;
        { py::gil_scoped_release r; result = flox::indicator::TEMA(period).compute(std::span<const double>(ptr, n)); }
        py::array_t<double> out(n);
        std::memcpy(out.mutable_data(), result.data(), n * sizeof(double));
        return out; },
      py::arg("input"), py::arg("period"));

  m.def(
      "stochastic",
      [](contiguous_double high, contiguous_double low, contiguous_double close,
         size_t k_period, size_t d_period) -> py::dict
      {
        size_t n = high.request().shape[0];
        checkSameSize(n, low.request().shape[0], "arrays must have same size");
        checkSameSize(n, close.request().shape[0], "arrays must have same size");
        auto* h = high.data();
        auto* l = low.data();
        auto* c = close.data();
        flox::indicator::StochasticResult result;
        {
          py::gil_scoped_release r;
          result = flox::indicator::Stochastic(k_period, d_period).compute(std::span<const double>(h, n), std::span<const double>(l, n), std::span<const double>(c, n));
        }
        py::array_t<double> ko(n), dout(n);
        std::memcpy(ko.mutable_data(), result.k.data(), n * sizeof(double));
        std::memcpy(dout.mutable_data(), result.d.data(), n * sizeof(double));
        py::dict d;
        d["k"] = ko;
        d["d"] = dout;
        return d;
      },
      py::arg("high"), py::arg("low"), py::arg("close"),
      py::arg("k_period") = 14, py::arg("d_period") = 3);

  m.def(
      "cci",
      [](contiguous_double high, contiguous_double low, contiguous_double close, size_t period)
      {
        size_t n = high.request().shape[0];
        checkSameSize(n, low.request().shape[0], "arrays must have same size");
        checkSameSize(n, close.request().shape[0], "arrays must have same size");
        auto* h = high.data();
        auto* l = low.data();
        auto* c = close.data();
        std::vector<double> result;
        {
          py::gil_scoped_release r;
          result = flox::indicator::CCI(period).compute(
              std::span<const double>(h, n), std::span<const double>(l, n), std::span<const double>(c, n));
        }
        py::array_t<double> out(n);
        std::memcpy(out.mutable_data(), result.data(), n * sizeof(double));
        return out;
      },
      py::arg("high"), py::arg("low"), py::arg("close"), py::arg("period") = 20);

  m.def(
      "vwap",
      [](contiguous_double close, contiguous_double volume, size_t window)
      {
        size_t n = close.request().shape[0];
        checkSameSize(n, volume.request().shape[0], "arrays must have same size");
        auto* c = close.data();
        auto* v = volume.data();
        std::vector<double> result;
        {
          py::gil_scoped_release r;
          result = flox::indicator::VWAP(window).compute(
              std::span<const double>(c, n), std::span<const double>(v, n));
        }
        py::array_t<double> out(n);
        std::memcpy(out.mutable_data(), result.data(), n * sizeof(double));
        return out;
      },
      py::arg("close"), py::arg("volume"), py::arg("window") = 96);

  m.def(
      "cvd",
      [](contiguous_double open, contiguous_double high, contiguous_double low,
         contiguous_double close, contiguous_double volume)
      {
        size_t n = close.request().shape[0];
        checkSameSize(n, open.request().shape[0], "arrays must have same size");
        checkSameSize(n, high.request().shape[0], "arrays must have same size");
        checkSameSize(n, low.request().shape[0], "arrays must have same size");
        checkSameSize(n, volume.request().shape[0], "arrays must have same size");
        auto* o = open.data();
        auto* h = high.data();
        auto* l = low.data();
        auto* c = close.data();
        auto* v = volume.data();
        std::vector<double> result;
        {
          py::gil_scoped_release r;
          result = flox::indicator::CVD().compute(
              std::span<const double>(o, n), std::span<const double>(h, n),
              std::span<const double>(l, n), std::span<const double>(c, n),
              std::span<const double>(v, n));
        }
        py::array_t<double> out(n);
        std::memcpy(out.mutable_data(), result.data(), n * sizeof(double));
        return out;
      },
      py::arg("open"), py::arg("high"), py::arg("low"), py::arg("close"), py::arg("volume"));

  m.def(
      "skewness", [](contiguous_double input, size_t period)
      { return computeSingle(flox::indicator::Skewness(period), input); },
      py::arg("input"), py::arg("period"));

  m.def(
      "kurtosis", [](contiguous_double input, size_t period)
      { return computeSingle(flox::indicator::Kurtosis(period), input); },
      py::arg("input"), py::arg("period"));

  m.def(
      "rolling_zscore", [](contiguous_double input, size_t period)
      { return computeSingle(flox::indicator::RollingZScore(period), input); },
      py::arg("input"), py::arg("period"));

  m.def(
      "shannon_entropy", [](contiguous_double input, size_t period, size_t bins)
      { return computeSingle(flox::indicator::ShannonEntropy(period, bins), input); },
      py::arg("input"), py::arg("period"), py::arg("bins") = 10);

  m.def(
      "parkinson_vol",
      [](contiguous_double high, contiguous_double low, size_t period) -> py::array_t<double>
      {
        size_t n = high.request().shape[0];
        checkSameSize(n, low.request().shape[0], "high and low must have same size");
        auto* h = high.data();
        auto* l = low.data();
        py::array_t<double> result(n);
        auto* o = result.mutable_data();
        {
          py::gil_scoped_release release;
          flox::indicator::ParkinsonVol(period).compute(
              std::span<const double>(h, n), std::span<const double>(l, n), std::span<double>(o, n));
        }
        return result;
      },
      py::arg("high"), py::arg("low"), py::arg("period"));

  m.def(
      "rogers_satchell_vol",
      [](contiguous_double open, contiguous_double high, contiguous_double low,
         contiguous_double close, size_t period) -> py::array_t<double>
      {
        size_t n = open.request().shape[0];
        checkSameSize(n, high.request().shape[0], "arrays must have same size");
        checkSameSize(n, low.request().shape[0], "arrays must have same size");
        checkSameSize(n, close.request().shape[0], "arrays must have same size");
        auto* o = open.data();
        auto* h = high.data();
        auto* l = low.data();
        auto* c = close.data();
        py::array_t<double> result(n);
        auto* out = result.mutable_data();
        {
          py::gil_scoped_release release;
          flox::indicator::RogersSatchellVol(period).compute(
              std::span<const double>(o, n), std::span<const double>(h, n),
              std::span<const double>(l, n), std::span<const double>(c, n),
              std::span<double>(out, n));
        }
        return result;
      },
      py::arg("open"), py::arg("high"), py::arg("low"), py::arg("close"), py::arg("period"));

  m.def(
      "adf",
      [](contiguous_double input, size_t max_lag, const std::string& regression) -> py::dict
      {
        auto buf = input.request();
        size_t n = buf.shape[0];
        const auto* p = static_cast<const double*>(buf.ptr);
        flox::indicator::AdfResult r;
        {
          py::gil_scoped_release release;
          r = flox::indicator::adf(std::span<const double>(p, n), max_lag, regression);
        }
        py::dict d;
        d["test_stat"] = r.test_stat;
        d["p_value"] = r.p_value;
        d["used_lag"] = r.used_lag;
        return d;
      },
      py::arg("input"), py::arg("max_lag") = 4, py::arg("regression") = "c");

  m.def(
      "autocorrelation",
      [](contiguous_double input, size_t window, size_t lag) -> py::array_t<double>
      {
        size_t n = input.request().shape[0];
        const double* p = input.data();
        py::array_t<double> result(n);
        auto* o = result.mutable_data();
        {
          py::gil_scoped_release release;
          flox::indicator::AutoCorrelation(window, lag)
              .compute(std::span<const double>(p, n), std::span<double>(o, n));
        }
        return result;
      },
      py::arg("input"), py::arg("window"), py::arg("lag"));

  // Renamed from "correlation" to free that name for the single-number
  // Pearson helper in optimizer_bindings.h, which loaded after this and
  // was silently shadowing the rolling indicator. Both are now reachable.
  m.def(
      "rolling_correlation",
      [](contiguous_double x, contiguous_double y, size_t period) -> py::array_t<double>
      {
        size_t n = x.request().shape[0];
        checkSameSize(n, y.request().shape[0], "x and y must have same size");
        auto* xp = x.data();
        auto* yp = y.data();
        py::array_t<double> result(n);
        auto* o = result.mutable_data();
        {
          py::gil_scoped_release release;
          flox::indicator::Correlation(period).compute(
              std::span<const double>(xp, n), std::span<const double>(yp, n), std::span<double>(o, n));
        }
        return result;
      },
      py::arg("x"), py::arg("y"), py::arg("period"));

  bindSingleIndicator<flox::indicator::SMA>(m, "SMA")
      .def(py::init<size_t>(), py::arg("period"));

  bindSingleIndicator<flox::indicator::EMA>(m, "EMA")
      .def(py::init<size_t>(), py::arg("period"));

  bindSingleIndicator<flox::indicator::RMA>(m, "RMA")
      .def(py::init<size_t>(), py::arg("period"));

  bindSingleIndicator<flox::indicator::RSI>(m, "RSI")
      .def(py::init<size_t>(), py::arg("period"));

  bindBarIndicator<flox::indicator::ATR>(m, "ATR")
      .def(py::init<size_t>(), py::arg("period"));

  bindSingleIndicator<flox::indicator::DEMA>(m, "DEMA")
      .def(py::init<size_t>(), py::arg("period"));

  bindSingleIndicator<flox::indicator::TEMA>(m, "TEMA")
      .def(py::init<size_t>(), py::arg("period"));

  bindSingleIndicator<flox::indicator::KAMA>(m, "KAMA")
      .def(py::init<size_t, size_t, size_t>(), py::arg("period"), py::arg("fast") = 2,
           py::arg("slow") = 30);

  bindSingleIndicator<flox::indicator::Slope>(m, "Slope")
      .def(py::init<size_t>(), py::arg("length"));

  py::class_<flox::indicator::MACD>(m, "MACD")
      .def(py::init<size_t, size_t, size_t>(), py::arg("fast") = 12, py::arg("slow") = 26,
           py::arg("signal") = 9)
      .def("compute",
           [](const flox::indicator::MACD& self, contiguous_double input)
           {
             auto buf = input.request();
             auto* p = static_cast<const double*>(buf.ptr);
             size_t n = buf.shape[0];
             flox::indicator::MacdResult r;
             {
               py::gil_scoped_release release;
               r = self.compute(std::span<const double>(p, n));
             }
             py::dict d;
             py::array_t<double> line(n), sig(n), hist(n);
             std::memcpy(line.mutable_data(), r.line.data(), n * sizeof(double));
             std::memcpy(sig.mutable_data(), r.signal.data(), n * sizeof(double));
             std::memcpy(hist.mutable_data(), r.histogram.data(), n * sizeof(double));
             d["line"] = line;
             d["signal"] = sig;
             d["histogram"] = hist;
             return d;
           })
      .def("update", [](flox::indicator::MACD& self, double v) -> std::optional<double>
           { self.update(v); return optFloat(self.value()); }, py::arg("value"))
      .def("reset", &flox::indicator::MACD::reset)
      .def_property_readonly("value", [](const flox::indicator::MACD& self)
                             { return optFloat(self.value()); })
      .def_property_readonly("line", [](const flox::indicator::MACD& self)
                             { return optFloat(self.value()); })
      .def_property_readonly("signal", [](const flox::indicator::MACD& self)
                             { return optFloat(self.signalValue()); })
      .def_property_readonly("histogram", [](const flox::indicator::MACD& self)
                             { return optFloat(self.histogramValue()); })
      .def_property_readonly("ready", &flox::indicator::MACD::ready)
      .def_property_readonly("count", &flox::indicator::MACD::count);

  py::class_<flox::indicator::Bollinger>(m, "Bollinger")
      .def(py::init<size_t, double>(), py::arg("period") = 20, py::arg("multiplier") = 2.0)
      .def("compute",
           [](const flox::indicator::Bollinger& self, contiguous_double input)
           {
             auto buf = input.request();
             auto* p = static_cast<const double*>(buf.ptr);
             size_t n = buf.shape[0];
             flox::indicator::BollingerResult r;
             {
               py::gil_scoped_release release;
               r = self.compute(std::span<const double>(p, n));
             }
             py::dict d;
             py::array_t<double> upper(n), middle(n), lower(n);
             std::memcpy(upper.mutable_data(), r.upper.data(), n * sizeof(double));
             std::memcpy(middle.mutable_data(), r.middle.data(), n * sizeof(double));
             std::memcpy(lower.mutable_data(), r.lower.data(), n * sizeof(double));
             d["upper"] = upper;
             d["middle"] = middle;
             d["lower"] = lower;
             return d;
           })
      .def("update", [](flox::indicator::Bollinger& self, double v) -> std::optional<double>
           { self.update(v); return optFloat(self.value()); }, py::arg("value"))
      .def("reset", &flox::indicator::Bollinger::reset)
      .def_property_readonly("value", [](const flox::indicator::Bollinger& self)
                             { return optFloat(self.value()); })
      .def_property_readonly("middle", [](const flox::indicator::Bollinger& self)
                             { return optFloat(self.middleValue()); })
      .def_property_readonly("upper", [](const flox::indicator::Bollinger& self)
                             { return optFloat(self.upperValue()); })
      .def_property_readonly("lower", [](const flox::indicator::Bollinger& self)
                             { return optFloat(self.lowerValue()); })
      .def_property_readonly("ready", &flox::indicator::Bollinger::ready)
      .def_property_readonly("count", &flox::indicator::Bollinger::count);

  py::class_<flox::indicator::Stochastic>(m, "Stochastic")
      .def(py::init<size_t, size_t>(), py::arg("k_period") = 14, py::arg("d_period") = 3)
      .def("compute",
           [](const flox::indicator::Stochastic& self, contiguous_double high,
              contiguous_double low, contiguous_double close)
           {
             size_t n = high.request().shape[0];
             checkSameSize(n, low.request().shape[0], "stochastic size");
             checkSameSize(n, close.request().shape[0], "stochastic size");
             flox::indicator::StochasticResult r;
             {
               py::gil_scoped_release release;
               r = self.compute(std::span<const double>(high.data(), n),
                                std::span<const double>(low.data(), n),
                                std::span<const double>(close.data(), n));
             }
             py::dict d;
             py::array_t<double> k(n), dArr(n);
             std::memcpy(k.mutable_data(), r.k.data(), n * sizeof(double));
             std::memcpy(dArr.mutable_data(), r.d.data(), n * sizeof(double));
             d["k"] = k;
             d["d"] = dArr;
             return d;
           })
      .def("update", [](flox::indicator::Stochastic& self, double high, double low, double close) -> std::optional<double>
           { self.update(high, low, close); return optFloat(self.value()); }, py::arg("high"), py::arg("low"), py::arg("close"))
      .def("reset", &flox::indicator::Stochastic::reset)
      .def_property_readonly("value", [](const flox::indicator::Stochastic& self)
                             { return optFloat(self.value()); })
      .def_property_readonly("k", [](const flox::indicator::Stochastic& self)
                             { return optFloat(self.kValue()); })
      .def_property_readonly("d", [](const flox::indicator::Stochastic& self)
                             { return optFloat(self.dValue()); })
      .def_property_readonly("ready", &flox::indicator::Stochastic::ready)
      .def_property_readonly("count", &flox::indicator::Stochastic::count);

  bindBarIndicator<flox::indicator::CCI>(m, "CCI")
      .def(py::init<size_t>(), py::arg("period") = 20);

  // OBV/VWAP/CVD: batch-only (top-level functions registered above);
  // streaming wrappers can be added in a follow-up if needed.

  bindSingleIndicator<flox::indicator::Skewness>(m, "Skewness")
      .def(py::init<size_t>(), py::arg("period"));

  bindSingleIndicator<flox::indicator::Kurtosis>(m, "Kurtosis")
      .def(py::init<size_t>(), py::arg("period"));

  bindSingleIndicator<flox::indicator::RollingZScore>(m, "RollingZScore")
      .def(py::init<size_t>(), py::arg("period"));

  bindSingleIndicator<flox::indicator::ShannonEntropy>(m, "ShannonEntropy")
      .def(py::init<size_t, size_t>(), py::arg("period"), py::arg("bins") = 10);

  bindHighLowIndicator<flox::indicator::ParkinsonVol>(m, "ParkinsonVol")
      .def(py::init<size_t>(), py::arg("period"));

  bindOhlcIndicator<flox::indicator::RogersSatchellVol>(m, "RogersSatchellVol")
      .def(py::init<size_t>(), py::arg("period"));

  bindPairIndicator<flox::indicator::Correlation>(m, "Correlation")
      .def(py::init<size_t>(), py::arg("period"));

  bindSingleIndicator<flox::indicator::AutoCorrelation>(m, "AutoCorrelation")
      .def(py::init<size_t, size_t>(), py::arg("window"), py::arg("lag"));

  // ── Discovery API ────────────────────────────────────────────────
  // Generated from include/flox/indicator/registry.def. Adding an indicator
  // there makes it appear in list_indicators() automatically.
  m.def(
      "list_indicators",
      []()
      {
        py::list out;
#define FLOX_INDICATOR(Cls, name, Kind, Args) out.append(#Cls);
#include "flox/indicator/registry.def"
#undef FLOX_INDICATOR
        return out;
      },
      "Return the list of indicator names registered in this build "
      "(both batch compute() and streaming update()/value on each).");
}
