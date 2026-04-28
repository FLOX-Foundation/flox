#pragma once

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <cmath>
#include <cstring>

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

inline py::object optVal(double v, bool ready)
{
  return ready ? py::cast(v) : py::none();
}

class PySMA
{
 public:
  PySMA(size_t period) : _period(period), _buffer(period, 0), _sum(0), _count(0), _index(0) {}
  py::object update(double value)
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
    return optVal(_value, isReady());
  }
  py::object getValue() const { return optVal(_value, isReady()); }
  bool isReady() const { return _count >= _period; }
  void reset()
  {
    _sum = 0;
    _count = 0;
    _index = 0;
    _value = 0;
    std::fill(_buffer.begin(), _buffer.end(), 0);
  }
  PySMA fresh() const { return PySMA(_period); }

 private:
  size_t _period, _count, _index;
  std::vector<double> _buffer;
  double _sum, _value = 0;
};

class PyEMA
{
 public:
  PyEMA(size_t period) : _period(period), _mult(2.0 / (period + 1)) {}
  py::object update(double value)
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
    return optVal(_value, isReady());
  }
  py::object getValue() const { return optVal(_value, isReady()); }
  bool isReady() const { return _count >= _period; }
  void reset()
  {
    _count = 0;
    _sum = 0;
    _value = 0;
  }
  PyEMA fresh() const { return PyEMA(_period); }

 private:
  size_t _period, _count = 0;
  double _mult, _sum = 0, _value = 0;
};

class PyRMA
{
 public:
  PyRMA(size_t period) : _period(period), _alpha(1.0 / period) {}
  py::object update(double value)
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
    return optVal(_value, isReady());
  }
  py::object getValue() const { return optVal(_value, isReady()); }
  bool isReady() const { return _count >= _period; }
  void reset()
  {
    _count = 0;
    _sum = 0;
    _value = 0;
  }
  PyRMA fresh() const { return PyRMA(_period); }

 private:
  size_t _period, _count = 0;
  double _alpha, _sum = 0, _value = 0;
};

class PyRSI
{
 public:
  PyRSI(size_t period) : _period(period) {}
  py::object update(double value)
  {
    if (_count == 0)
    {
      _prev = value;
      _count++;
      return py::none();
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
    return optVal(_value, isReady());
  }
  py::object getValue() const { return optVal(_value, isReady()); }
  bool isReady() const { return _count > _period; }
  void reset()
  {
    _count = 0;
    _prev = 0;
    _avgGain = 0;
    _avgLoss = 0;
    _value = 50;
  }
  PyRSI fresh() const { return PyRSI(_period); }

 private:
  size_t _period, _count = 0;
  double _prev = 0, _avgGain = 0, _avgLoss = 0, _value = 50;
};

class PyATR
{
 public:
  PyATR(size_t period) : _period(period) {}
  py::object update(double high, double low, double close)
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
    return optVal(_value, isReady());
  }
  py::object getValue() const { return optVal(_value, isReady()); }
  bool isReady() const { return _count >= _period; }
  void reset()
  {
    _count = 0;
    _prevClose = 0;
    _sum = 0;
    _value = 0;
  }
  PyATR fresh() const { return PyATR(_period); }

 private:
  size_t _period, _count = 0;
  double _prevClose = 0, _sum = 0, _value = 0;
};

class PyDEMA
{
 public:
  PyDEMA(size_t period) : _period(period), _ema1(period), _ema2(period) {}
  py::object update(double value)
  {
    _ema1.update(value);
    double e1 = _ema1.isReady() ? py::cast<double>(_ema1.getValue()) : 0;
    _ema2.update(e1);
    double e2 = _ema2.isReady() ? py::cast<double>(_ema2.getValue()) : 0;
    _value = 2 * e1 - e2;
    return optVal(_value, isReady());
  }
  py::object getValue() const { return optVal(_value, isReady()); }
  bool isReady() const { return _ema2.isReady(); }
  void reset()
  {
    _ema1.reset();
    _ema2.reset();
    _value = 0;
  }
  PyDEMA fresh() const { return PyDEMA(_period); }

 private:
  size_t _period;
  PyEMA _ema1, _ema2;
  double _value = 0;
};

class PyTEMA
{
 public:
  PyTEMA(size_t period) : _period(period), _ema1(period), _ema2(period), _ema3(period) {}
  py::object update(double value)
  {
    _ema1.update(value);
    double e1 = _ema1.isReady() ? py::cast<double>(_ema1.getValue()) : 0;
    _ema2.update(e1);
    double e2 = _ema2.isReady() ? py::cast<double>(_ema2.getValue()) : 0;
    _ema3.update(e2);
    double e3 = _ema3.isReady() ? py::cast<double>(_ema3.getValue()) : 0;
    _value = 3 * e1 - 3 * e2 + e3;
    return optVal(_value, isReady());
  }
  py::object getValue() const { return optVal(_value, isReady()); }
  bool isReady() const { return _ema3.isReady(); }
  void reset()
  {
    _ema1.reset();
    _ema2.reset();
    _ema3.reset();
    _value = 0;
  }
  PyTEMA fresh() const { return PyTEMA(_period); }

 private:
  size_t _period;
  PyEMA _ema1, _ema2, _ema3;
  double _value = 0;
};

class PyKAMA
{
 public:
  PyKAMA(size_t period, size_t fast = 2, size_t slow = 30)
      : _period(period), _fast(fast), _slow(slow), _fastSc(2.0 / (fast + 1)), _slowSc(2.0 / (slow + 1))
  {
  }
  py::object update(double value)
  {
    _buffer.push_back(value);
    _count++;
    if (_count <= _period)
    {
      _value = value;
      return py::none();
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
    return py::cast(_value);
  }
  py::object getValue() const { return optVal(_value, isReady()); }
  bool isReady() const { return _count > _period; }
  void reset()
  {
    _count = 0;
    _value = 0;
    _buffer.clear();
  }
  PyKAMA fresh() const { return PyKAMA(_period, _fast, _slow); }

 private:
  size_t _period, _fast, _slow, _count = 0;
  double _fastSc, _slowSc, _value = 0;
  std::vector<double> _buffer;
};

class PySlope
{
 public:
  PySlope(size_t length) : _length(length) {}
  py::object update(double value)
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
    return optVal(_value, isReady());
  }
  py::object getValue() const { return optVal(_value, isReady()); }
  bool isReady() const { return _buffer.size() > _length; }
  void reset()
  {
    _value = 0;
    _buffer.clear();
  }
  PySlope fresh() const { return PySlope(_length); }

 private:
  size_t _length;
  double _value = 0;
  std::vector<double> _buffer;
};

class PyMACD
{
 public:
  PyMACD(size_t fast = 12, size_t slow = 26, size_t signal = 9)
      : _fast(fast), _slow(slow), _signal(signal), _fastEma(fast), _slowEma(slow), _signalEma(signal)
  {
  }
  py::object update(double value)
  {
    _fastEma.update(value);
    _slowEma.update(value);
    double f = _fastEma.isReady() ? py::cast<double>(_fastEma.getValue()) : 0;
    double s = _slowEma.isReady() ? py::cast<double>(_slowEma.getValue()) : 0;
    _line = f - s;
    if (_slowEma.isReady())
    {
      _signalEma.update(_line);
      _ready = _signalEma.isReady();
      _signalVal = _signalEma.isReady() ? py::cast<double>(_signalEma.getValue()) : 0;
    }
    _histogram = _line - _signalVal;
    return optVal(_line, _ready);
  }
  py::object getLine() const { return optVal(_line, _ready); }
  py::object getSignal() const { return optVal(_signalVal, _ready); }
  py::object getHistogram() const { return optVal(_histogram, _ready); }
  py::object getValue() const { return optVal(_line, _ready); }
  bool isReady() const { return _ready; }
  void reset()
  {
    _fastEma.reset();
    _slowEma.reset();
    _signalEma.reset();
    _line = 0;
    _signalVal = 0;
    _histogram = 0;
    _ready = false;
  }
  PyMACD fresh() const { return PyMACD(_fast, _slow, _signal); }

 private:
  size_t _fast, _slow, _signal;
  PyEMA _fastEma, _slowEma, _signalEma;
  double _line = 0, _signalVal = 0, _histogram = 0;
  bool _ready = false;
};

class PyBollinger
{
 public:
  PyBollinger(size_t period = 20, double multiplier = 2.0)
      : _sma(period), _period(period), _multiplier(multiplier)
  {
  }
  py::object update(double value)
  {
    _sma.update(value);
    _buffer.push_back(value);
    if (_buffer.size() > _period)
    {
      _buffer.erase(_buffer.begin());
    }
    if (_sma.isReady())
    {
      _middle = py::cast<double>(_sma.getValue());
      double sum = 0;
      for (double v : _buffer)
      {
        double d = v - _middle;
        sum += d * d;
      }
      double s = std::sqrt(sum / _buffer.size());
      _upper = _middle + _multiplier * s;
      _lower = _middle - _multiplier * s;
    }
    return optVal(_middle, isReady());
  }
  py::object getUpper() const { return optVal(_upper, isReady()); }
  py::object getMiddle() const { return optVal(_middle, isReady()); }
  py::object getLower() const { return optVal(_lower, isReady()); }
  py::object getValue() const { return optVal(_middle, isReady()); }
  bool isReady() const { return _sma.isReady(); }
  void reset()
  {
    _sma.reset();
    _buffer.clear();
    _upper = 0;
    _middle = 0;
    _lower = 0;
  }
  PyBollinger fresh() const { return PyBollinger(_period, _multiplier); }

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
      : _kPeriod(kPeriod), _dPeriod(dPeriod), _dSma(dPeriod)
  {
  }
  py::object update(double high, double low, double close)
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
      _dSma.update(_k);
      _d = _dSma.isReady() ? py::cast<double>(_dSma.getValue()) : 0;
    }
    return optVal(_k, isReady());
  }
  py::object getK() const { return optVal(_k, isReady()); }
  py::object getD() const { return optVal(_d, isReady()); }
  py::object getValue() const { return optVal(_k, isReady()); }
  bool isReady() const { return _highs.size() >= _kPeriod && _dSma.isReady(); }
  void reset()
  {
    _highs.clear();
    _lows.clear();
    _dSma.reset();
    _k = 0;
    _d = 0;
  }
  PyStochastic fresh() const { return PyStochastic(_kPeriod, _dPeriod); }

 private:
  size_t _kPeriod, _dPeriod;
  PySMA _dSma;
  std::vector<double> _highs, _lows;
  double _k = 0, _d = 0;
};

class PyCCI
{
 public:
  PyCCI(size_t period = 20) : _period(period) {}
  py::object update(double high, double low, double close)
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
    return optVal(_value, isReady());
  }
  py::object getValue() const { return optVal(_value, isReady()); }
  bool isReady() const { return _buffer.size() >= _period; }
  void reset()
  {
    _value = 0;
    _buffer.clear();
  }
  PyCCI fresh() const { return PyCCI(_period); }

 private:
  size_t _period;
  double _value = 0;
  std::vector<double> _buffer;
};

class PyOBV
{
 public:
  py::object update(double close, double volume)
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
    return py::cast(_value);
  }
  py::object getValue() const { return optVal(_value, isReady()); }
  bool isReady() const { return _count > 0; }
  void reset()
  {
    _count = 0;
    _prevClose = 0;
    _value = 0;
  }
  PyOBV fresh() const { return PyOBV(); }

 private:
  size_t _count = 0;
  double _prevClose = 0, _value = 0;
};

class PyVWAP
{
 public:
  PyVWAP(size_t window) : _window(window) {}
  py::object update(double close, double volume)
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
    return optVal(_value, isReady());
  }
  py::object getValue() const { return optVal(_value, isReady()); }
  bool isReady() const { return _prices.size() >= _window; }
  void reset()
  {
    _value = 0;
    _prices.clear();
    _volumes.clear();
  }
  PyVWAP fresh() const { return PyVWAP(_window); }

 private:
  size_t _window;
  double _value = 0;
  std::vector<double> _prices, _volumes;
};

class PyCVD
{
 public:
  py::object update(double open, double high, double low, double close, double volume)
  {
    double range = high - low;
    double delta = range > 0 ? volume * (close - open) / range : 0;
    _value += delta;
    _count++;
    return py::cast(_value);
  }
  py::object getValue() const { return optVal(_value, isReady()); }
  bool isReady() const { return _count > 0; }
  void reset()
  {
    _count = 0;
    _value = 0;
  }
  PyCVD fresh() const { return PyCVD(); }

 private:
  size_t _count = 0;
  double _value = 0;
};

class PySkewness
{
 public:
  PySkewness(size_t period) : _period(period) {}
  py::object update(double value)
  {
    _buffer.push_back(value);
    if (_buffer.size() > _period)
    {
      _buffer.erase(_buffer.begin());
    }
    if (_buffer.size() >= _period)
    {
      double p = static_cast<double>(_period);
      double mean = 0;
      for (double v : _buffer)
      {
        mean += v;
      }
      mean /= p;
      double m2 = 0, m3 = 0;
      for (double v : _buffer)
      {
        double d = v - mean;
        double d2 = d * d;
        m2 += d2;
        m3 += d2 * d;
      }
      double var = m2 / (p - 1.0);
      if (var == 0.0)
      {
        return py::none();
      }
      double s = std::sqrt(var);
      _value = (p / ((p - 1.0) * (p - 2.0))) * (m3 / (s * s * s));
      return py::cast(_value);
    }
    return py::none();
  }
  py::object getValue() const { return optVal(_value, isReady()); }
  bool isReady() const { return _buffer.size() >= _period; }
  void reset()
  {
    _value = 0;
    _buffer.clear();
  }
  PySkewness fresh() const { return PySkewness(_period); }

 private:
  size_t _period;
  double _value = 0;
  std::vector<double> _buffer;
};

class PyKurtosis
{
 public:
  PyKurtosis(size_t period) : _period(period) {}
  py::object update(double value)
  {
    _buffer.push_back(value);
    if (_buffer.size() > _period)
    {
      _buffer.erase(_buffer.begin());
    }
    if (_buffer.size() >= _period)
    {
      double p = static_cast<double>(_period);
      double mean = 0;
      for (double v : _buffer)
      {
        mean += v;
      }
      mean /= p;
      double m2 = 0, m4 = 0;
      for (double v : _buffer)
      {
        double d = v - mean;
        double d2 = d * d;
        m2 += d2;
        m4 += d2 * d2;
      }
      double var = m2 / (p - 1.0);
      if (var == 0.0)
      {
        return py::none();
      }
      double s2 = var;
      double t1 = (p * (p + 1.0)) / ((p - 1.0) * (p - 2.0) * (p - 3.0));
      double t2 = m4 / (s2 * s2);
      double t3 = (3.0 * (p - 1.0) * (p - 1.0)) / ((p - 2.0) * (p - 3.0));
      _value = t1 * t2 - t3;
      return py::cast(_value);
    }
    return py::none();
  }
  py::object getValue() const { return optVal(_value, isReady()); }
  bool isReady() const { return _buffer.size() >= _period; }
  void reset()
  {
    _value = 0;
    _buffer.clear();
  }
  PyKurtosis fresh() const { return PyKurtosis(_period); }

 private:
  size_t _period;
  double _value = 0;
  std::vector<double> _buffer;
};

class PyRollingZScore
{
 public:
  PyRollingZScore(size_t period) : _period(period) {}
  py::object update(double value)
  {
    _buffer.push_back(value);
    if (_buffer.size() > _period)
    {
      _buffer.erase(_buffer.begin());
    }
    if (_buffer.size() >= _period)
    {
      double p = static_cast<double>(_period);
      double mean = 0;
      for (double v : _buffer)
      {
        mean += v;
      }
      mean /= p;
      double sumSq = 0;
      for (double v : _buffer)
      {
        double d = v - mean;
        sumSq += d * d;
      }
      double s = std::sqrt(sumSq / (p - 1.0));
      if (s == 0.0)
      {
        return py::none();
      }
      _value = (value - mean) / s;
      return py::cast(_value);
    }
    return py::none();
  }
  py::object getValue() const { return optVal(_value, isReady()); }
  bool isReady() const { return _buffer.size() >= _period; }
  void reset()
  {
    _value = 0;
    _buffer.clear();
  }
  PyRollingZScore fresh() const { return PyRollingZScore(_period); }

 private:
  size_t _period;
  double _value = 0;
  std::vector<double> _buffer;
};

class PyShannonEntropy
{
 public:
  PyShannonEntropy(size_t period, size_t bins = 10) : _period(period), _bins(bins) {}
  py::object update(double value)
  {
    _buffer.push_back(value);
    if (_buffer.size() > _period)
    {
      _buffer.erase(_buffer.begin());
    }
    if (_buffer.size() >= _period)
    {
      double lo = *std::min_element(_buffer.begin(), _buffer.end());
      double hi = *std::max_element(_buffer.begin(), _buffer.end());
      if (lo == hi)
      {
        _value = 0.0;
        return py::cast(_value);
      }
      std::vector<size_t> counts(_bins, 0);
      double range = hi - lo;
      for (double v : _buffer)
      {
        size_t bin = static_cast<size_t>(((v - lo) / range) * _bins);
        if (bin >= _bins)
        {
          bin = _bins - 1;
        }
        counts[bin]++;
      }
      double ent = 0;
      double denom = static_cast<double>(_period);
      for (size_t b = 0; b < _bins; ++b)
      {
        if (counts[b] > 0)
        {
          double p = static_cast<double>(counts[b]) / denom;
          ent -= p * std::log(p);
        }
      }
      _value = ent / std::log(static_cast<double>(_bins));
      return py::cast(_value);
    }
    return py::none();
  }
  py::object getValue() const { return optVal(_value, isReady()); }
  bool isReady() const { return _buffer.size() >= _period; }
  void reset()
  {
    _value = 0;
    _buffer.clear();
  }
  PyShannonEntropy fresh() const { return PyShannonEntropy(_period, _bins); }

 private:
  size_t _period, _bins;
  double _value = 0;
  std::vector<double> _buffer;
};

class PyParkinsonVol
{
 public:
  PyParkinsonVol(size_t period) : _period(period) {}
  py::object update(double high, double low)
  {
    if (low <= 0.0 || high < low)
    {
      _hlsq.push_back(std::nan(""));
    }
    else
    {
      double lnhl = std::log(high / low);
      _hlsq.push_back(lnhl * lnhl);
    }
    if (_hlsq.size() > _period)
    {
      _hlsq.erase(_hlsq.begin());
    }
    if (_hlsq.size() >= _period)
    {
      double sum = 0;
      bool valid = true;
      for (double v : _hlsq)
      {
        if (std::isnan(v))
        {
          valid = false;
          break;
        }
        sum += v;
      }
      if (valid)
      {
        _value = std::sqrt(sum / static_cast<double>(_period) / (4.0 * std::log(2.0)));
        return py::cast(_value);
      }
    }
    return py::none();
  }
  py::object getValue() const { return optVal(_value, isReady()); }
  bool isReady() const { return _hlsq.size() >= _period; }
  void reset()
  {
    _value = 0;
    _hlsq.clear();
  }
  PyParkinsonVol fresh() const { return PyParkinsonVol(_period); }

 private:
  size_t _period;
  double _value = 0;
  std::vector<double> _hlsq;
};

class PyRogersSatchellVol
{
 public:
  PyRogersSatchellVol(size_t period) : _period(period) {}
  py::object update(double open, double high, double low, double close)
  {
    if (open <= 0.0 || close <= 0.0)
    {
      _rsq.push_back(std::nan(""));
    }
    else
    {
      double v = std::log(high / close) * std::log(high / open) +
                 std::log(low / close) * std::log(low / open);
      _rsq.push_back(v);
    }
    if (_rsq.size() > _period)
    {
      _rsq.erase(_rsq.begin());
    }
    if (_rsq.size() >= _period)
    {
      double sum = 0;
      bool valid = true;
      for (double v : _rsq)
      {
        if (std::isnan(v))
        {
          valid = false;
          break;
        }
        sum += v;
      }
      if (valid)
      {
        double avg = sum / static_cast<double>(_period);
        _value = avg >= 0.0 ? std::sqrt(avg) : 0.0;
        return py::cast(_value);
      }
    }
    return py::none();
  }
  py::object getValue() const { return optVal(_value, isReady()); }
  bool isReady() const { return _rsq.size() >= _period; }
  void reset()
  {
    _value = 0;
    _rsq.clear();
  }
  PyRogersSatchellVol fresh() const { return PyRogersSatchellVol(_period); }

 private:
  size_t _period;
  double _value = 0;
  std::vector<double> _rsq;
};

class PyCorrelation
{
 public:
  PyCorrelation(size_t period) : _period(period) {}
  py::object update(double x, double y)
  {
    _xbuf.push_back(x);
    _ybuf.push_back(y);
    if (_xbuf.size() > _period)
    {
      _xbuf.erase(_xbuf.begin());
      _ybuf.erase(_ybuf.begin());
    }
    if (_xbuf.size() >= _period)
    {
      double p = static_cast<double>(_period);
      double sx = 0, sy = 0, sxy = 0, sx2 = 0, sy2 = 0;
      for (size_t i = 0; i < _period; ++i)
      {
        sx += _xbuf[i];
        sy += _ybuf[i];
        sxy += _xbuf[i] * _ybuf[i];
        sx2 += _xbuf[i] * _xbuf[i];
        sy2 += _ybuf[i] * _ybuf[i];
      }
      double num = p * sxy - sx * sy;
      double den = std::sqrt((p * sx2 - sx * sx) * (p * sy2 - sy * sy));
      if (den == 0.0)
      {
        return py::none();
      }
      _value = num / den;
      return py::cast(_value);
    }
    return py::none();
  }
  py::object getValue() const { return optVal(_value, isReady()); }
  bool isReady() const { return _xbuf.size() >= _period; }
  void reset()
  {
    _value = 0;
    _xbuf.clear();
    _ybuf.clear();
  }
  PyCorrelation fresh() const { return PyCorrelation(_period); }

 private:
  size_t _period;
  double _value = 0;
  std::vector<double> _xbuf, _ybuf;
};

class PyAutoCorrelation
{
 public:
  PyAutoCorrelation(size_t window, size_t lag) : _window(window), _lag(lag) {}
  py::object update(double x)
  {
    _buf.push_back(x);
    size_t needed = _window + _lag;
    if (_buf.size() > needed)
    {
      _buf.erase(_buf.begin());
    }
    if (_buf.size() < needed)
    {
      return py::none();
    }
    double w = static_cast<double>(_window);
    double sx = 0, sy = 0, sxy = 0, sx2 = 0, sy2 = 0;
    for (size_t i = 0; i < _window; ++i)
    {
      double xi = _buf[i + _lag];
      double yi = _buf[i];
      sx += xi;
      sy += yi;
      sxy += xi * yi;
      sx2 += xi * xi;
      sy2 += yi * yi;
    }
    double num = w * sxy - sx * sy;
    double den = std::sqrt((w * sx2 - sx * sx) * (w * sy2 - sy * sy));
    if (den == 0.0)
    {
      return py::none();
    }
    _value = num / den;
    return py::cast(_value);
  }
  py::object getValue() const { return optVal(_value, isReady()); }
  bool isReady() const { return _buf.size() >= _window + _lag; }
  void reset()
  {
    _value = 0;
    _buf.clear();
  }
  PyAutoCorrelation fresh() const { return PyAutoCorrelation(_window, _lag); }

 private:
  size_t _window;
  size_t _lag;
  double _value = 0;
  std::vector<double> _buf;
};

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

  m.def(
      "correlation",
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

  py::class_<PySMA>(m, "SMA")
      .def(py::init<size_t>(), py::arg("period"))
      .def("update", &PySMA::update, py::arg("value"))
      .def("reset", &PySMA::reset)
      .def("fresh", &PySMA::fresh)
      .def_property_readonly("value", &PySMA::getValue)
      .def_property_readonly("ready", &PySMA::isReady);

  py::class_<PyEMA>(m, "EMA")
      .def(py::init<size_t>(), py::arg("period"))
      .def("update", &PyEMA::update, py::arg("value"))
      .def("reset", &PyEMA::reset)
      .def("fresh", &PyEMA::fresh)
      .def_property_readonly("value", &PyEMA::getValue)
      .def_property_readonly("ready", &PyEMA::isReady);

  py::class_<PyRMA>(m, "RMA")
      .def(py::init<size_t>(), py::arg("period"))
      .def("update", &PyRMA::update, py::arg("value"))
      .def("reset", &PyRMA::reset)
      .def("fresh", &PyRMA::fresh)
      .def_property_readonly("value", &PyRMA::getValue)
      .def_property_readonly("ready", &PyRMA::isReady);

  py::class_<PyRSI>(m, "RSI")
      .def(py::init<size_t>(), py::arg("period"))
      .def("update", &PyRSI::update, py::arg("value"))
      .def("reset", &PyRSI::reset)
      .def("fresh", &PyRSI::fresh)
      .def_property_readonly("value", &PyRSI::getValue)
      .def_property_readonly("ready", &PyRSI::isReady);

  py::class_<PyATR>(m, "ATR")
      .def(py::init<size_t>(), py::arg("period"))
      .def("update", &PyATR::update, py::arg("high"), py::arg("low"), py::arg("close"))
      .def("reset", &PyATR::reset)
      .def("fresh", &PyATR::fresh)
      .def_property_readonly("value", &PyATR::getValue)
      .def_property_readonly("ready", &PyATR::isReady);

  py::class_<PyDEMA>(m, "DEMA")
      .def(py::init<size_t>(), py::arg("period"))
      .def("update", &PyDEMA::update, py::arg("value"))
      .def("reset", &PyDEMA::reset)
      .def("fresh", &PyDEMA::fresh)
      .def_property_readonly("value", &PyDEMA::getValue)
      .def_property_readonly("ready", &PyDEMA::isReady);

  py::class_<PyTEMA>(m, "TEMA")
      .def(py::init<size_t>(), py::arg("period"))
      .def("update", &PyTEMA::update, py::arg("value"))
      .def("reset", &PyTEMA::reset)
      .def("fresh", &PyTEMA::fresh)
      .def_property_readonly("value", &PyTEMA::getValue)
      .def_property_readonly("ready", &PyTEMA::isReady);

  py::class_<PyKAMA>(m, "KAMA")
      .def(py::init<size_t, size_t, size_t>(), py::arg("period"), py::arg("fast") = 2,
           py::arg("slow") = 30)
      .def("update", &PyKAMA::update, py::arg("value"))
      .def("reset", &PyKAMA::reset)
      .def("fresh", &PyKAMA::fresh)
      .def_property_readonly("value", &PyKAMA::getValue)
      .def_property_readonly("ready", &PyKAMA::isReady);

  py::class_<PySlope>(m, "Slope")
      .def(py::init<size_t>(), py::arg("length"))
      .def("update", &PySlope::update, py::arg("value"))
      .def("reset", &PySlope::reset)
      .def("fresh", &PySlope::fresh)
      .def_property_readonly("value", &PySlope::getValue)
      .def_property_readonly("ready", &PySlope::isReady);

  py::class_<PyMACD>(m, "MACD")
      .def(py::init<size_t, size_t, size_t>(), py::arg("fast") = 12, py::arg("slow") = 26,
           py::arg("signal") = 9)
      .def("update", &PyMACD::update, py::arg("value"))
      .def("reset", &PyMACD::reset)
      .def("fresh", &PyMACD::fresh)
      .def_property_readonly("line", &PyMACD::getLine)
      .def_property_readonly("signal", &PyMACD::getSignal)
      .def_property_readonly("histogram", &PyMACD::getHistogram)
      .def_property_readonly("value", &PyMACD::getValue)
      .def_property_readonly("ready", &PyMACD::isReady);

  py::class_<PyBollinger>(m, "Bollinger")
      .def(py::init<size_t, double>(), py::arg("period") = 20, py::arg("multiplier") = 2.0)
      .def("update", &PyBollinger::update, py::arg("value"))
      .def("reset", &PyBollinger::reset)
      .def("fresh", &PyBollinger::fresh)
      .def_property_readonly("upper", &PyBollinger::getUpper)
      .def_property_readonly("middle", &PyBollinger::getMiddle)
      .def_property_readonly("lower", &PyBollinger::getLower)
      .def_property_readonly("value", &PyBollinger::getValue)
      .def_property_readonly("ready", &PyBollinger::isReady);

  py::class_<PyStochastic>(m, "Stochastic")
      .def(py::init<size_t, size_t>(), py::arg("k_period") = 14, py::arg("d_period") = 3)
      .def("update", &PyStochastic::update, py::arg("high"), py::arg("low"), py::arg("close"))
      .def("reset", &PyStochastic::reset)
      .def("fresh", &PyStochastic::fresh)
      .def_property_readonly("k", &PyStochastic::getK)
      .def_property_readonly("d", &PyStochastic::getD)
      .def_property_readonly("value", &PyStochastic::getValue)
      .def_property_readonly("ready", &PyStochastic::isReady);

  py::class_<PyCCI>(m, "CCI")
      .def(py::init<size_t>(), py::arg("period") = 20)
      .def("update", &PyCCI::update, py::arg("high"), py::arg("low"), py::arg("close"))
      .def("reset", &PyCCI::reset)
      .def("fresh", &PyCCI::fresh)
      .def_property_readonly("value", &PyCCI::getValue)
      .def_property_readonly("ready", &PyCCI::isReady);

  py::class_<PyOBV>(m, "OBV")
      .def(py::init<>())
      .def("update", &PyOBV::update, py::arg("close"), py::arg("volume"))
      .def("reset", &PyOBV::reset)
      .def("fresh", &PyOBV::fresh)
      .def_property_readonly("value", &PyOBV::getValue)
      .def_property_readonly("ready", &PyOBV::isReady);

  py::class_<PyVWAP>(m, "VWAP")
      .def(py::init<size_t>(), py::arg("window"))
      .def("update", &PyVWAP::update, py::arg("close"), py::arg("volume"))
      .def("reset", &PyVWAP::reset)
      .def("fresh", &PyVWAP::fresh)
      .def_property_readonly("value", &PyVWAP::getValue)
      .def_property_readonly("ready", &PyVWAP::isReady);

  py::class_<PyCVD>(m, "CVD")
      .def(py::init<>())
      .def("update", &PyCVD::update, py::arg("open"), py::arg("high"), py::arg("low"),
           py::arg("close"), py::arg("volume"))
      .def("reset", &PyCVD::reset)
      .def("fresh", &PyCVD::fresh)
      .def_property_readonly("value", &PyCVD::getValue)
      .def_property_readonly("ready", &PyCVD::isReady);

  py::class_<PySkewness>(m, "Skewness")
      .def(py::init<size_t>(), py::arg("period"))
      .def("update", &PySkewness::update, py::arg("value"))
      .def("reset", &PySkewness::reset)
      .def("fresh", &PySkewness::fresh)
      .def_property_readonly("value", &PySkewness::getValue)
      .def_property_readonly("ready", &PySkewness::isReady);

  py::class_<PyKurtosis>(m, "Kurtosis")
      .def(py::init<size_t>(), py::arg("period"))
      .def("update", &PyKurtosis::update, py::arg("value"))
      .def("reset", &PyKurtosis::reset)
      .def("fresh", &PyKurtosis::fresh)
      .def_property_readonly("value", &PyKurtosis::getValue)
      .def_property_readonly("ready", &PyKurtosis::isReady);

  py::class_<PyRollingZScore>(m, "RollingZScore")
      .def(py::init<size_t>(), py::arg("period"))
      .def("update", &PyRollingZScore::update, py::arg("value"))
      .def("reset", &PyRollingZScore::reset)
      .def("fresh", &PyRollingZScore::fresh)
      .def_property_readonly("value", &PyRollingZScore::getValue)
      .def_property_readonly("ready", &PyRollingZScore::isReady);

  py::class_<PyShannonEntropy>(m, "ShannonEntropy")
      .def(py::init<size_t, size_t>(), py::arg("period"), py::arg("bins") = 10)
      .def("update", &PyShannonEntropy::update, py::arg("value"))
      .def("reset", &PyShannonEntropy::reset)
      .def("fresh", &PyShannonEntropy::fresh)
      .def_property_readonly("value", &PyShannonEntropy::getValue)
      .def_property_readonly("ready", &PyShannonEntropy::isReady);

  py::class_<PyParkinsonVol>(m, "ParkinsonVol")
      .def(py::init<size_t>(), py::arg("period"))
      .def("update", &PyParkinsonVol::update, py::arg("high"), py::arg("low"))
      .def("reset", &PyParkinsonVol::reset)
      .def("fresh", &PyParkinsonVol::fresh)
      .def_property_readonly("value", &PyParkinsonVol::getValue)
      .def_property_readonly("ready", &PyParkinsonVol::isReady);

  py::class_<PyRogersSatchellVol>(m, "RogersSatchellVol")
      .def(py::init<size_t>(), py::arg("period"))
      .def("update", &PyRogersSatchellVol::update, py::arg("open"), py::arg("high"),
           py::arg("low"), py::arg("close"))
      .def("reset", &PyRogersSatchellVol::reset)
      .def("fresh", &PyRogersSatchellVol::fresh)
      .def_property_readonly("value", &PyRogersSatchellVol::getValue)
      .def_property_readonly("ready", &PyRogersSatchellVol::isReady);

  py::class_<PyCorrelation>(m, "Correlation")
      .def(py::init<size_t>(), py::arg("period"))
      .def("update", &PyCorrelation::update, py::arg("x"), py::arg("y"))
      .def("reset", &PyCorrelation::reset)
      .def("fresh", &PyCorrelation::fresh)
      .def_property_readonly("value", &PyCorrelation::getValue)
      .def_property_readonly("ready", &PyCorrelation::isReady);

  py::class_<PyAutoCorrelation>(m, "AutoCorrelation")
      .def(py::init<size_t, size_t>(), py::arg("window"), py::arg("lag"))
      .def("update", &PyAutoCorrelation::update, py::arg("value"))
      .def("reset", &PyAutoCorrelation::reset)
      .def("fresh", &PyAutoCorrelation::fresh)
      .def_property_readonly("value", &PyAutoCorrelation::getValue)
      .def_property_readonly("ready", &PyAutoCorrelation::isReady);
}
