// python/indicator_bindings.h

#pragma once

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstring>

#include "flox/indicator/adx.h"
#include "flox/indicator/atr.h"
#include "flox/indicator/bollinger.h"
#include "flox/indicator/cci.h"
#include "flox/indicator/chop.h"
#include "flox/indicator/cvd.h"
#include "flox/indicator/dema.h"
#include "flox/indicator/ema.h"
#include "flox/indicator/kama.h"
#include "flox/indicator/macd.h"
#include "flox/indicator/obv.h"
#include "flox/indicator/rma.h"
#include "flox/indicator/rsi.h"
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

template <typename Indicator>
py::array_t<double> computeSingle(const Indicator& ind, py::array_t<double> input)
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

}  // namespace

inline void bindIndicators(py::module_& m)
{
  m.def(
      "ema", [](py::array_t<double> input, size_t period)
      { return computeSingle(flox::indicator::EMA(period), input); },
      py::arg("input"), py::arg("period"));

  m.def(
      "sma", [](py::array_t<double> input, size_t period)
      { return computeSingle(flox::indicator::SMA(period), input); },
      py::arg("input"), py::arg("period"));

  m.def(
      "rsi", [](py::array_t<double> input, size_t period)
      { return computeSingle(flox::indicator::RSI(period), input); },
      py::arg("input"), py::arg("period"));

  m.def(
      "atr",
      [](py::array_t<double> high, py::array_t<double> low, py::array_t<double> close,
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
      [](py::array_t<double> input, size_t fast, size_t slow, size_t signal) -> py::dict
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
      "slope", [](py::array_t<double> input, size_t length)
      { return computeSingle(flox::indicator::Slope(length), input); },
      py::arg("input"), py::arg("length") = 1);

  m.def(
      "kama", [](py::array_t<double> input, size_t period)
      { return computeSingle(flox::indicator::KAMA(period), input); },
      py::arg("input"), py::arg("period") = 10);

  m.def(
      "obv",
      [](py::array_t<double> close, py::array_t<double> volume) -> py::array_t<double>
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
      [](py::array_t<double> high, py::array_t<double> low, py::array_t<double> close,
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
      [](py::array_t<double> high, py::array_t<double> low, py::array_t<double> close,
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
      [](py::array_t<double> input, size_t period, double stddev) -> py::dict
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
      [](py::array_t<int8_t> signal_long, py::array_t<int8_t> signal_short,
         py::array_t<double> log_returns) -> py::array_t<double>
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
      [](py::array_t<int8_t> signal_long, py::array_t<int8_t> signal_short,
         py::array_t<double> log_returns) -> py::array_t<double>
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
      [](py::array_t<double> returns) -> double
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
      [](py::array_t<double> trade_pnls) -> double
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
      "rma", [](py::array_t<double> input, size_t period)
      { return computeSingle(flox::indicator::RMA(period), input); },
      "Wilder's Moving Average (alpha=1/period)", py::arg("input"), py::arg("period"));

  m.def(
      "dema", [](py::array_t<double> input, size_t period)
      {
        auto buf = input.request();
        size_t n = buf.shape[0];
        auto* ptr = static_cast<const double*>(buf.ptr);
        std::vector<double> result;
        { py::gil_scoped_release r; result = flox::indicator::DEMA(period).compute(std::span<const double>(ptr, n)); }
        py::array_t<double> out(n);
        std::memcpy(out.mutable_data(), result.data(), n * sizeof(double));
        return out; }, "Double EMA: 2*EMA - EMA(EMA)", py::arg("input"), py::arg("period"));

  m.def(
      "tema", [](py::array_t<double> input, size_t period)
      {
        auto buf = input.request();
        size_t n = buf.shape[0];
        auto* ptr = static_cast<const double*>(buf.ptr);
        std::vector<double> result;
        { py::gil_scoped_release r; result = flox::indicator::TEMA(period).compute(std::span<const double>(ptr, n)); }
        py::array_t<double> out(n);
        std::memcpy(out.mutable_data(), result.data(), n * sizeof(double));
        return out; }, "Triple EMA: 3*EMA - 3*EMA(EMA) + EMA(EMA(EMA))", py::arg("input"), py::arg("period"));

  m.def(
      "stochastic",
      [](py::array_t<double> high, py::array_t<double> low, py::array_t<double> close,
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
      "Stochastic %K and %D",
      py::arg("high"), py::arg("low"), py::arg("close"),
      py::arg("k_period") = 14, py::arg("d_period") = 3);

  m.def(
      "cci",
      [](py::array_t<double> high, py::array_t<double> low, py::array_t<double> close,
         size_t period)
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
      "Commodity Channel Index",
      py::arg("high"), py::arg("low"), py::arg("close"), py::arg("period") = 20);

  m.def(
      "vwap",
      [](py::array_t<double> close, py::array_t<double> volume, size_t window)
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
      "Rolling VWAP",
      py::arg("close"), py::arg("volume"), py::arg("window") = 96);

  m.def(
      "cvd",
      [](py::array_t<double> open, py::array_t<double> high, py::array_t<double> low,
         py::array_t<double> close, py::array_t<double> volume)
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
      "Cumulative Volume Delta",
      py::arg("open"), py::arg("high"), py::arg("low"), py::arg("close"), py::arg("volume"));
}
