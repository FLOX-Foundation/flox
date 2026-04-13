// python/profile_bindings.h

#pragma once

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <chrono>

#include "flox/aggregator/custom/footprint_bar.h"
#include "flox/aggregator/custom/market_profile.h"
#include "flox/aggregator/custom/volume_profile.h"
#include "flox/book/events/trade_event.h"
#include "flox/common.h"
#include "flox/util/base/time.h"

namespace py = pybind11;

namespace
{

using namespace flox;

// ---------------------------------------------------------------------------
// Wrapper: FootprintBar
// ---------------------------------------------------------------------------
class PyFootprintBar
{
 public:
  explicit PyFootprintBar(double tickSize)
  {
    _fp.setTickSize(Price::fromDouble(tickSize));
  }

  void addTrade(double price, double quantity, bool isBuy)
  {
    TradeEvent te;
    te.trade.price = Price::fromDouble(price);
    te.trade.quantity = Quantity::fromDouble(quantity);
    te.trade.isBuy = isBuy;
    _fp.addTrade(te);
  }

  void addTrades(py::array_t<double> prices, py::array_t<double> quantities,
                 py::array_t<uint8_t> isBuy)
  {
    const size_t n = prices.size();
    if (quantities.size() != static_cast<py::ssize_t>(n) ||
        isBuy.size() != static_cast<py::ssize_t>(n))
    {
      throw std::invalid_argument("prices, quantities, and is_buy must have the same length");
    }

    const auto* px = prices.data();
    const auto* qt = quantities.data();
    const auto* ib = isBuy.data();

    py::gil_scoped_release release;
    for (size_t i = 0; i < n; ++i)
    {
      TradeEvent te;
      te.trade.price = Price::fromDouble(px[i]);
      te.trade.quantity = Quantity::fromDouble(qt[i]);
      te.trade.isBuy = (ib[i] != 0);
      _fp.addTrade(te);
    }
  }

  double totalDelta() const { return _fp.totalDelta().toDouble(); }
  double totalVolume() const { return _fp.totalVolume().toDouble(); }
  int numLevels() const { return static_cast<int>(_fp.numLevels()); }

  py::list levels() const
  {
    py::list result;
    for (size_t i = 0; i < _fp.numLevels(); ++i)
    {
      const auto* lvl = _fp.level(i);
      py::dict d;
      d["price"] = lvl->price.toDouble();
      d["bid_volume"] = lvl->bidVolume.toDouble();
      d["ask_volume"] = lvl->askVolume.toDouble();
      d["delta"] = lvl->delta().toDouble();
      d["imbalance_ratio"] = lvl->imbalanceRatio();
      d["total_volume"] = lvl->totalVolume().toDouble();
      result.append(d);
    }
    return result;
  }

  double highestBuyingPressure() const { return _fp.highestBuyingPressure().toDouble(); }
  double highestSellingPressure() const { return _fp.highestSellingPressure().toDouble(); }

  py::object strongestImbalance(double threshold) const
  {
    Price p = _fp.strongestImbalance(threshold);
    if (p.isZero())
    {
      return py::none();
    }
    return py::cast(p.toDouble());
  }

  void clear() { _fp.clear(); }

 private:
  FootprintBar<64> _fp;
};

// ---------------------------------------------------------------------------
// Wrapper: VolumeProfile
// ---------------------------------------------------------------------------
class PyVolumeProfile
{
 public:
  explicit PyVolumeProfile(double tickSize)
  {
    _vp.setTickSize(Price::fromDouble(tickSize));
  }

  void addTrade(double price, double quantity, bool isBuy)
  {
    TradeEvent te;
    te.trade.price = Price::fromDouble(price);
    te.trade.quantity = Quantity::fromDouble(quantity);
    te.trade.isBuy = isBuy;
    _vp.addTrade(te);
  }

  void addTrades(py::array_t<double> prices, py::array_t<double> quantities,
                 py::array_t<uint8_t> isBuy)
  {
    const size_t n = prices.size();
    if (quantities.size() != static_cast<py::ssize_t>(n) ||
        isBuy.size() != static_cast<py::ssize_t>(n))
    {
      throw std::invalid_argument("prices, quantities, and is_buy must have the same length");
    }

    const auto* px = prices.data();
    const auto* qt = quantities.data();
    const auto* ib = isBuy.data();

    py::gil_scoped_release release;
    for (size_t i = 0; i < n; ++i)
    {
      TradeEvent te;
      te.trade.price = Price::fromDouble(px[i]);
      te.trade.quantity = Quantity::fromDouble(qt[i]);
      te.trade.isBuy = (ib[i] != 0);
      _vp.addTrade(te);
    }
  }

  double poc() const { return _vp.poc().toDouble(); }
  double valueAreaHigh() const { return _vp.valueAreaHigh().toDouble(); }
  double valueAreaLow() const { return _vp.valueAreaLow().toDouble(); }
  double totalVolume() const { return _vp.totalVolume().toDouble(); }
  double totalDelta() const { return _vp.totalDelta().toDouble(); }
  int numLevels() const { return static_cast<int>(_vp.numLevels()); }

  py::list levels() const
  {
    py::list result;
    for (size_t i = 0; i < _vp.numLevels(); ++i)
    {
      const auto* lvl = _vp.level(i);
      py::dict d;
      d["price"] = lvl->price.toDouble();
      d["volume"] = lvl->volume.toDouble();
      d["buy_volume"] = lvl->buyVolume.toDouble();
      d["sell_volume"] = lvl->sellVolume().toDouble();
      d["delta"] = lvl->delta().toDouble();
      result.append(d);
    }
    return result;
  }

  double volumeAt(double price) const
  {
    return _vp.volumeAt(Price::fromDouble(price)).toDouble();
  }

  void clear() { _vp.clear(); }

 private:
  VolumeProfile<256> _vp;
};

// ---------------------------------------------------------------------------
// Wrapper: MarketProfile
// ---------------------------------------------------------------------------
class PyMarketProfile
{
 public:
  PyMarketProfile(double tickSize, int periodMinutes, int64_t sessionStartNs)
  {
    _mp.setTickSize(Price::fromDouble(tickSize));
    _mp.setPeriodDuration(std::chrono::minutes(periodMinutes));
    _mp.setSessionStart(static_cast<uint64_t>(sessionStartNs));
  }

  void addTrade(int64_t timestampNs, double price, double quantity, bool isBuy)
  {
    TradeEvent te;
    te.trade.price = Price::fromDouble(price);
    te.trade.quantity = Quantity::fromDouble(quantity);
    te.trade.isBuy = isBuy;
    te.trade.exchangeTsNs = timestampNs;
    _mp.addTrade(te);
  }

  void addTrades(py::array_t<int64_t> timestampsNs, py::array_t<double> prices,
                 py::array_t<double> quantities, py::array_t<uint8_t> isBuy)
  {
    const size_t n = prices.size();
    if (timestampsNs.size() != static_cast<py::ssize_t>(n) ||
        quantities.size() != static_cast<py::ssize_t>(n) ||
        isBuy.size() != static_cast<py::ssize_t>(n))
    {
      throw std::invalid_argument(
          "timestamps, prices, quantities, and is_buy must have the same length");
    }

    const auto* ts = timestampsNs.data();
    const auto* px = prices.data();
    const auto* qt = quantities.data();
    const auto* ib = isBuy.data();

    py::gil_scoped_release release;
    for (size_t i = 0; i < n; ++i)
    {
      TradeEvent te;
      te.trade.price = Price::fromDouble(px[i]);
      te.trade.quantity = Quantity::fromDouble(qt[i]);
      te.trade.isBuy = (ib[i] != 0);
      te.trade.exchangeTsNs = ts[i];
      _mp.addTrade(te);
    }
  }

  double poc() const { return _mp.poc().toDouble(); }
  double valueAreaHigh() const { return _mp.valueAreaHigh().toDouble(); }
  double valueAreaLow() const { return _mp.valueAreaLow().toDouble(); }
  double initialBalanceHigh() const { return _mp.initialBalanceHigh().toDouble(); }
  double initialBalanceLow() const { return _mp.initialBalanceLow().toDouble(); }

  py::list singlePrints() const
  {
    auto [count, prices] = _mp.singlePrints();
    py::list result;
    for (size_t i = 0; i < count; ++i)
    {
      result.append(prices[i].toDouble());
    }
    return result;
  }

  bool isPoorHigh() const { return _mp.isPoorHigh(); }
  bool isPoorLow() const { return _mp.isPoorLow(); }
  int numLevels() const { return static_cast<int>(_mp.numLevels()); }
  int currentPeriod() const { return static_cast<int>(_mp.currentPeriod()); }

  py::list levels() const
  {
    py::list result;
    for (size_t i = 0; i < _mp.numLevels(); ++i)
    {
      const auto* lvl = _mp.level(i);
      py::dict d;
      d["price"] = lvl->price.toDouble();
      d["tpo_count"] = lvl->tpoCount;
      d["is_single_print"] = lvl->isSinglePrint();
      result.append(d);
    }
    return result;
  }

  void clear() { _mp.clear(); }

 private:
  MarketProfile<256, 26> _mp;
};

}  // namespace

inline void bindProfiles(py::module_& m)
{
  py::class_<PyFootprintBar>(m, "FootprintBar",
                             "Footprint bar for order flow analysis. "
                             "Tracks bid/ask volume at each price level.")
      .def(py::init<double>(), py::arg("tick_size"))
      .def("add_trade", &PyFootprintBar::addTrade,
           py::arg("price"), py::arg("quantity"), py::arg("is_buy"))
      .def("add_trades", &PyFootprintBar::addTrades,
           py::arg("prices"), py::arg("quantities"), py::arg("is_buy"))
      .def("total_delta", &PyFootprintBar::totalDelta)
      .def("total_volume", &PyFootprintBar::totalVolume)
      .def("num_levels", &PyFootprintBar::numLevels)
      .def("levels", &PyFootprintBar::levels)
      .def("highest_buying_pressure", &PyFootprintBar::highestBuyingPressure)
      .def("highest_selling_pressure", &PyFootprintBar::highestSellingPressure)
      .def("strongest_imbalance", &PyFootprintBar::strongestImbalance,
           py::arg("threshold") = 0.7)
      .def("clear", &PyFootprintBar::clear);

  py::class_<PyVolumeProfile>(m, "VolumeProfile",
                              "Volume Profile aggregator. "
                              "Tracks volume distribution across price levels.")
      .def(py::init<double>(), py::arg("tick_size"))
      .def("add_trade", &PyVolumeProfile::addTrade,
           py::arg("price"), py::arg("quantity"), py::arg("is_buy"))
      .def("add_trades", &PyVolumeProfile::addTrades,
           py::arg("prices"), py::arg("quantities"), py::arg("is_buy"))
      .def("poc", &PyVolumeProfile::poc)
      .def("value_area_high", &PyVolumeProfile::valueAreaHigh)
      .def("value_area_low", &PyVolumeProfile::valueAreaLow)
      .def("total_volume", &PyVolumeProfile::totalVolume)
      .def("total_delta", &PyVolumeProfile::totalDelta)
      .def("num_levels", &PyVolumeProfile::numLevels)
      .def("levels", &PyVolumeProfile::levels)
      .def("volume_at", &PyVolumeProfile::volumeAt, py::arg("price"))
      .def("clear", &PyVolumeProfile::clear);

  py::class_<PyMarketProfile>(m, "MarketProfile",
                              "Market Profile (TPO) aggregator. "
                              "Tracks price activity across time periods.")
      .def(py::init<double, int, int64_t>(),
           py::arg("tick_size"), py::arg("period_minutes"), py::arg("session_start_ns"))
      .def("add_trade", &PyMarketProfile::addTrade,
           py::arg("timestamp_ns"), py::arg("price"), py::arg("quantity"), py::arg("is_buy"))
      .def("add_trades", &PyMarketProfile::addTrades,
           py::arg("timestamps_ns"), py::arg("prices"), py::arg("quantities"), py::arg("is_buy"))
      .def("poc", &PyMarketProfile::poc)
      .def("value_area_high", &PyMarketProfile::valueAreaHigh)
      .def("value_area_low", &PyMarketProfile::valueAreaLow)
      .def("initial_balance_high", &PyMarketProfile::initialBalanceHigh)
      .def("initial_balance_low", &PyMarketProfile::initialBalanceLow)
      .def("single_prints", &PyMarketProfile::singlePrints)
      .def("is_poor_high", &PyMarketProfile::isPoorHigh)
      .def("is_poor_low", &PyMarketProfile::isPoorLow)
      .def("num_levels", &PyMarketProfile::numLevels)
      .def("current_period", &PyMarketProfile::currentPeriod)
      .def("levels", &PyMarketProfile::levels)
      .def("clear", &PyMarketProfile::clear);
}
