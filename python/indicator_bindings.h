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

// =================================================================
// Streaming indicator wrappers
// =================================================================

class PySMA
{
 public:
  PySMA(size_t period) : _period(period), _buffer(period, 0), _sum(0), _count(0), _index(0) {}
  double update(double value)
  {
    if (_count < _period)
    {
      _buffer[_count] = value;
      _sum += value;
      _count++;
    }
    else
    {
      _sum -= _buffer[_index];
      _buffer[_index] = value;
      _sum += value;
      _index = (_index + 1) % _period;
    }
    _value = _sum / _count;
    return _value;
  }
  double getValue() const { return _value; }
  bool isReady() const { return _count >= _period; }

 private:
  size_t _period, _count, _index;
  std::vector<double> _buffer;
  double _sum, _value = 0;
};

class PyEMA
{
 public:
  PyEMA(size_t period) : _period(period), _mult(2.0 / (period + 1)) {}
  double update(double value)
  {
    if (_count < _period)
    {
      _sum += value;
      _count++;
      _value = _sum / _count;
    }
    else
    {
      _value = (value - _value) * _mult + _value;
    }
    return _value;
  }
  double getValue() const { return _value; }
  bool isReady() const { return _count >= _period; }

 private:
  size_t _period, _count = 0;
  double _mult, _sum = 0, _value = 0;
};

class PyRMA
{
 public:
  PyRMA(size_t period) : _period(period), _alpha(1.0 / period) {}
  double update(double value)
  {
    if (_count < _period)
    {
      _sum += value;
      _count++;
      _value = _sum / _count;
    }
    else
    {
      _value = _alpha * value + (1 - _alpha) * _value;
    }
    return _value;
  }
  double getValue() const { return _value; }
  bool isReady() const { return _count >= _period; }

 private:
  size_t _period, _count = 0;
  double _alpha, _sum = 0, _value = 0;
};

class PyRSI
{
 public:
  PyRSI(size_t period) : _period(period) {}
  double update(double value)
  {
    if (_count == 0)
    {
      _prev = value;
      _count++;
      return _value;
    }
    double change = value - _prev;
    _prev = value;
    double gain = change > 0 ? change : 0;
    double loss = change < 0 ? -change : 0;
    if (_count <= _period)
    {
      _avgGain += gain / _period;
      _avgLoss += loss / _period;
      _count++;
    }
    else
    {
      _avgGain = (_avgGain * (_period - 1) + gain) / _period;
      _avgLoss = (_avgLoss * (_period - 1) + loss) / _period;
    }
    _value = _avgLoss == 0 ? 100.0 : 100.0 - 100.0 / (1.0 + _avgGain / _avgLoss);
    return _value;
  }
  double getValue() const { return _value; }
  bool isReady() const { return _count > _period; }

 private:
  size_t _period, _count = 0;
  double _prev = 0, _avgGain = 0, _avgLoss = 0, _value = 50;
};

class PyATR
{
 public:
  PyATR(size_t period) : _period(period) {}
  double update(double high, double low, double close)
  {
    double tr;
    if (_count == 0)
    {
      tr = high - low;
    }
    else
    {
      tr = std::max({high - low, std::abs(high - _prevClose), std::abs(low - _prevClose)});
    }
    _prevClose = close;
    _count++;
    if (_count <= _period)
    {
      _sum += tr;
      _value = _sum / _count;
    }
    else
    {
      _value = (_value * (_period - 1) + tr) / _period;
    }
    return _value;
  }
  double getValue() const { return _value; }
  bool isReady() const { return _count >= _period; }

 private:
  size_t _period, _count = 0;
  double _prevClose = 0, _sum = 0, _value = 0;
};

class PyDEMA
{
 public:
  PyDEMA(size_t period) : _ema1(period), _ema2(period) {}
  double update(double value)
  {
    double e1 = _ema1.update(value);
    double e2 = _ema2.update(e1);
    _value = 2 * e1 - e2;
    return _value;
  }
  double getValue() const { return _value; }
  bool isReady() const { return _ema2.isReady(); }

 private:
  PyEMA _ema1, _ema2;
  double _value = 0;
};

class PyTEMA
{
 public:
  PyTEMA(size_t period) : _ema1(period), _ema2(period), _ema3(period) {}
  double update(double value)
  {
    double e1 = _ema1.update(value);
    double e2 = _ema2.update(e1);
    double e3 = _ema3.update(e2);
    _value = 3 * e1 - 3 * e2 + e3;
    return _value;
  }
  double getValue() const { return _value; }
  bool isReady() const { return _ema3.isReady(); }

 private:
  PyEMA _ema1, _ema2, _ema3;
  double _value = 0;
};

class PyKAMA
{
 public:
  PyKAMA(size_t period, size_t fast = 2, size_t slow = 30)
      : _period(period), _fastSc(2.0 / (fast + 1)), _slowSc(2.0 / (slow + 1))
  {
  }
  double update(double value)
  {
    _buffer.push_back(value);
    _count++;
    if (_count <= _period)
    {
      _value = value;
      return _value;
    }
    if (_buffer.size() > _period + 1)
    {
      _buffer.erase(_buffer.begin());
    }
    double direction = std::abs(value - _buffer.front());
    double volatility = 0;
    for (size_t i = 1; i < _buffer.size(); i++)
    {
      volatility += std::abs(_buffer[i] - _buffer[i - 1]);
    }
    double er = volatility != 0 ? direction / volatility : 0;
    double sc = er * (_fastSc - _slowSc) + _slowSc;
    sc *= sc;
    _value += sc * (value - _value);
    return _value;
  }
  double getValue() const { return _value; }
  bool isReady() const { return _count > _period; }

 private:
  size_t _period, _count = 0;
  double _fastSc, _slowSc, _value = 0;
  std::vector<double> _buffer;
};

class PySlope
{
 public:
  PySlope(size_t length) : _length(length) {}
  double update(double value)
  {
    _buffer.push_back(value);
    if (_buffer.size() > _length + 1)
    {
      _buffer.erase(_buffer.begin());
    }
    if (_buffer.size() > _length)
    {
      _value = (value - _buffer.front()) / _length;
    }
    return _value;
  }
  double getValue() const { return _value; }
  bool isReady() const { return _buffer.size() > _length; }

 private:
  size_t _length;
  double _value = 0;
  std::vector<double> _buffer;
};

class PyMACD
{
 public:
  PyMACD(size_t fast = 12, size_t slow = 26, size_t signal = 9)
      : _fastEma(fast), _slowEma(slow), _signalEma(signal)
  {
  }
  double update(double value)
  {
    double f = _fastEma.update(value);
    double s = _slowEma.update(value);
    _line = f - s;
    if (_slowEma.isReady())
    {
      _signal = _signalEma.update(_line);
      _ready = _signalEma.isReady();
    }
    _histogram = _line - _signal;
    return _line;
  }
  double getLine() const { return _line; }
  double getSignal() const { return _signal; }
  double getHistogram() const { return _histogram; }
  double getValue() const { return _line; }
  bool isReady() const { return _ready; }

 private:
  PyEMA _fastEma, _slowEma, _signalEma;
  double _line = 0, _signal = 0, _histogram = 0;
  bool _ready = false;
};

class PyBollinger
{
 public:
  PyBollinger(size_t period = 20, double multiplier = 2.0)
      : _sma(period), _period(period), _multiplier(multiplier)
  {
  }
  double update(double value)
  {
    _middle = _sma.update(value);
    _buffer.push_back(value);
    if (_buffer.size() > _period)
    {
      _buffer.erase(_buffer.begin());
    }
    if (_sma.isReady())
    {
      double sum = 0;
      for (double v : _buffer)
      {
        double d = v - _middle;
        sum += d * d;
      }
      double std = std::sqrt(sum / _buffer.size());
      _upper = _middle + _multiplier * std;
      _lower = _middle - _multiplier * std;
    }
    return _middle;
  }
  double getUpper() const { return _upper; }
  double getMiddle() const { return _middle; }
  double getLower() const { return _lower; }
  double getValue() const { return _middle; }
  bool isReady() const { return _sma.isReady(); }

 private:
  PySMA _sma;
  size_t _period;
  double _multiplier, _upper = 0, _middle = 0, _lower = 0;
  std::vector<double> _buffer;
};

class PyStochastic
{
 public:
  PyStochastic(size_t kPeriod = 14, size_t dPeriod = 3)
      : _kPeriod(kPeriod), _dSma(dPeriod)
  {
  }
  double update(double high, double low, double close)
  {
    _highs.push_back(high);
    _lows.push_back(low);
    if (_highs.size() > _kPeriod)
    {
      _highs.erase(_highs.begin());
      _lows.erase(_lows.begin());
    }
    if (_highs.size() >= _kPeriod)
    {
      double hh = *std::max_element(_highs.begin(), _highs.end());
      double ll = *std::min_element(_lows.begin(), _lows.end());
      _k = (hh != ll) ? 100.0 * (close - ll) / (hh - ll) : 0;
      _d = _dSma.update(_k);
    }
    return _k;
  }
  double getK() const { return _k; }
  double getD() const { return _d; }
  double getValue() const { return _k; }
  bool isReady() const { return _highs.size() >= _kPeriod && _dSma.isReady(); }

 private:
  size_t _kPeriod;
  PySMA _dSma;
  std::vector<double> _highs, _lows;
  double _k = 0, _d = 0;
};

class PyCCI
{
 public:
  PyCCI(size_t period = 20) : _period(period) {}
  double update(double high, double low, double close)
  {
    double tp = (high + low + close) / 3.0;
    _buffer.push_back(tp);
    if (_buffer.size() > _period)
    {
      _buffer.erase(_buffer.begin());
    }
    if (_buffer.size() >= _period)
    {
      double mean = 0;
      for (double v : _buffer)
      {
        mean += v;
      }
      mean /= _buffer.size();
      double devSum = 0;
      for (double v : _buffer)
      {
        devSum += std::abs(v - mean);
      }
      double meanDev = devSum / _buffer.size();
      _value = meanDev != 0 ? (tp - mean) / (0.015 * meanDev) : 0;
    }
    return _value;
  }
  double getValue() const { return _value; }
  bool isReady() const { return _buffer.size() >= _period; }

 private:
  size_t _period;
  double _value = 0;
  std::vector<double> _buffer;
};

class PyOBV
{
 public:
  double update(double close, double volume)
  {
    if (_count == 0)
    {
      _value = volume;
    }
    else if (close > _prevClose)
    {
      _value += volume;
    }
    else if (close < _prevClose)
    {
      _value -= volume;
    }
    _prevClose = close;
    _count++;
    return _value;
  }
  double getValue() const { return _value; }
  bool isReady() const { return _count > 0; }

 private:
  size_t _count = 0;
  double _prevClose = 0, _value = 0;
};

class PyVWAP
{
 public:
  PyVWAP(size_t window) : _window(window) {}
  double update(double close, double volume)
  {
    _prices.push_back(close);
    _volumes.push_back(volume);
    if (_prices.size() > _window)
    {
      _prices.erase(_prices.begin());
      _volumes.erase(_volumes.begin());
    }
    double pvSum = 0, vSum = 0;
    for (size_t i = 0; i < _prices.size(); i++)
    {
      pvSum += _prices[i] * _volumes[i];
      vSum += _volumes[i];
    }
    _value = vSum > 0 ? pvSum / vSum : 0;
    return _value;
  }
  double getValue() const { return _value; }
  bool isReady() const { return _prices.size() >= _window; }

 private:
  size_t _window;
  double _value = 0;
  std::vector<double> _prices, _volumes;
};

class PyCVD
{
 public:
  double update(double open, double high, double low, double close, double volume)
  {
    double range = high - low;
    double delta = range > 0 ? volume * (close - open) / range : 0;
    _value += delta;
    _count++;
    return _value;
  }
  double getValue() const { return _value; }
  bool isReady() const { return _count > 0; }

 private:
  size_t _count = 0;
  double _value = 0;
};

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

  // =================================================================
  // Streaming indicator classes
  // =================================================================

  py::class_<PySMA>(m, "SMA")
      .def(py::init<size_t>(), py::arg("period"))
      .def("update", &PySMA::update, py::arg("value"))
      .def_property_readonly("value", &PySMA::getValue)
      .def_property_readonly("ready", &PySMA::isReady);

  py::class_<PyEMA>(m, "EMA")
      .def(py::init<size_t>(), py::arg("period"))
      .def("update", &PyEMA::update, py::arg("value"))
      .def_property_readonly("value", &PyEMA::getValue)
      .def_property_readonly("ready", &PyEMA::isReady);

  py::class_<PyRMA>(m, "RMA")
      .def(py::init<size_t>(), py::arg("period"))
      .def("update", &PyRMA::update, py::arg("value"))
      .def_property_readonly("value", &PyRMA::getValue)
      .def_property_readonly("ready", &PyRMA::isReady);

  py::class_<PyRSI>(m, "RSI")
      .def(py::init<size_t>(), py::arg("period"))
      .def("update", &PyRSI::update, py::arg("value"))
      .def_property_readonly("value", &PyRSI::getValue)
      .def_property_readonly("ready", &PyRSI::isReady);

  py::class_<PyATR>(m, "ATR")
      .def(py::init<size_t>(), py::arg("period"))
      .def("update", &PyATR::update, py::arg("high"), py::arg("low"), py::arg("close"))
      .def_property_readonly("value", &PyATR::getValue)
      .def_property_readonly("ready", &PyATR::isReady);

  py::class_<PyDEMA>(m, "DEMA")
      .def(py::init<size_t>(), py::arg("period"))
      .def("update", &PyDEMA::update, py::arg("value"))
      .def_property_readonly("value", &PyDEMA::getValue)
      .def_property_readonly("ready", &PyDEMA::isReady);

  py::class_<PyTEMA>(m, "TEMA")
      .def(py::init<size_t>(), py::arg("period"))
      .def("update", &PyTEMA::update, py::arg("value"))
      .def_property_readonly("value", &PyTEMA::getValue)
      .def_property_readonly("ready", &PyTEMA::isReady);

  py::class_<PyKAMA>(m, "KAMA")
      .def(py::init<size_t, size_t, size_t>(), py::arg("period"), py::arg("fast") = 2,
           py::arg("slow") = 30)
      .def("update", &PyKAMA::update, py::arg("value"))
      .def_property_readonly("value", &PyKAMA::getValue)
      .def_property_readonly("ready", &PyKAMA::isReady);

  py::class_<PySlope>(m, "Slope")
      .def(py::init<size_t>(), py::arg("length"))
      .def("update", &PySlope::update, py::arg("value"))
      .def_property_readonly("value", &PySlope::getValue)
      .def_property_readonly("ready", &PySlope::isReady);

  py::class_<PyMACD>(m, "MACD")
      .def(py::init<size_t, size_t, size_t>(), py::arg("fast") = 12, py::arg("slow") = 26,
           py::arg("signal") = 9)
      .def("update", &PyMACD::update, py::arg("value"))
      .def_property_readonly("line", &PyMACD::getLine)
      .def_property_readonly("signal", &PyMACD::getSignal)
      .def_property_readonly("histogram", &PyMACD::getHistogram)
      .def_property_readonly("value", &PyMACD::getValue)
      .def_property_readonly("ready", &PyMACD::isReady);

  py::class_<PyBollinger>(m, "Bollinger")
      .def(py::init<size_t, double>(), py::arg("period") = 20, py::arg("multiplier") = 2.0)
      .def("update", &PyBollinger::update, py::arg("value"))
      .def_property_readonly("upper", &PyBollinger::getUpper)
      .def_property_readonly("middle", &PyBollinger::getMiddle)
      .def_property_readonly("lower", &PyBollinger::getLower)
      .def_property_readonly("value", &PyBollinger::getValue)
      .def_property_readonly("ready", &PyBollinger::isReady);

  py::class_<PyStochastic>(m, "Stochastic")
      .def(py::init<size_t, size_t>(), py::arg("k_period") = 14, py::arg("d_period") = 3)
      .def("update", &PyStochastic::update, py::arg("high"), py::arg("low"), py::arg("close"))
      .def_property_readonly("k", &PyStochastic::getK)
      .def_property_readonly("d", &PyStochastic::getD)
      .def_property_readonly("value", &PyStochastic::getValue)
      .def_property_readonly("ready", &PyStochastic::isReady);

  py::class_<PyCCI>(m, "CCI")
      .def(py::init<size_t>(), py::arg("period") = 20)
      .def("update", &PyCCI::update, py::arg("high"), py::arg("low"), py::arg("close"))
      .def_property_readonly("value", &PyCCI::getValue)
      .def_property_readonly("ready", &PyCCI::isReady);

  py::class_<PyOBV>(m, "OBV")
      .def(py::init<>())
      .def("update", &PyOBV::update, py::arg("close"), py::arg("volume"))
      .def_property_readonly("value", &PyOBV::getValue)
      .def_property_readonly("ready", &PyOBV::isReady);

  py::class_<PyVWAP>(m, "VWAP")
      .def(py::init<size_t>(), py::arg("window"))
      .def("update", &PyVWAP::update, py::arg("close"), py::arg("volume"))
      .def_property_readonly("value", &PyVWAP::getValue)
      .def_property_readonly("ready", &PyVWAP::isReady);

  py::class_<PyCVD>(m, "CVD")
      .def(py::init<>())
      .def("update", &PyCVD::update, py::arg("open"), py::arg("high"), py::arg("low"),
           py::arg("close"), py::arg("volume"))
      .def_property_readonly("value", &PyCVD::getValue)
      .def_property_readonly("ready", &PyCVD::isReady);
}
