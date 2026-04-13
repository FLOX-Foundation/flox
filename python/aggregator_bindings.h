// python/aggregator_bindings.h

#pragma once

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "flox/aggregator/aggregation_policy.h"
#include "flox/aggregator/bar.h"
#include "flox/aggregator/policies/heikin_ashi_bar_policy.h"
#include "flox/aggregator/policies/range_bar_policy.h"
#include "flox/aggregator/policies/renko_bar_policy.h"
#include "flox/aggregator/policies/tick_bar_policy.h"
#include "flox/aggregator/policies/time_bar_policy.h"
#include "flox/aggregator/policies/volume_bar_policy.h"
#include "flox/book/events/trade_event.h"
#include "flox/book/trade.h"
#include "flox/common.h"
#include "flox/util/base/time.h"

#include <chrono>
#include <cstring>
#include <vector>

namespace py = pybind11;

namespace
{

using namespace flox;

#pragma pack(push, 1)
struct PyExtBar
{
  int64_t start_time_ns;
  int64_t end_time_ns;
  int64_t open_raw;
  int64_t high_raw;
  int64_t low_raw;
  int64_t close_raw;
  int64_t volume_raw;
  int64_t buy_volume_raw;
  int64_t trade_count;
};
#pragma pack(pop)
static_assert(sizeof(PyExtBar) == 72);

inline int64_t toUnixNs(TimePoint tp)
{
  return tp.time_since_epoch().count() - unix_to_flox_offset_ns().load(std::memory_order_relaxed);
}

inline PyExtBar barToExtBar(const Bar& b)
{
  return {.start_time_ns = toUnixNs(b.startTime),
          .end_time_ns = toUnixNs(b.endTime),
          .open_raw = b.open.raw(),
          .high_raw = b.high.raw(),
          .low_raw = b.low.raw(),
          .close_raw = b.close.raw(),
          .volume_raw = b.volume.raw(),
          .buy_volume_raw = b.buyVolume.raw(),
          .trade_count = b.tradeCount.raw()};
}

// Batch aggregation: takes pre-extracted vectors (no GIL needed)
template <typename Policy>
std::vector<PyExtBar> doAggregate(Policy& policy, const int64_t* ts, const double* px,
                                  const double* qty, const uint8_t* ib, size_t n)
{
  std::vector<PyExtBar> bars;
  bars.reserve(n / 10);  // rough estimate
  Bar currentBar;
  bool initialized = false;

  for (size_t i = 0; i < n; ++i)
  {
    TradeEvent trade;
    trade.trade.price = Price::fromDouble(px[i]);
    trade.trade.quantity = Quantity::fromDouble(qty[i]);
    trade.trade.isBuy = (ib[i] != 0);
    trade.trade.exchangeTsNs = ts[i];
    trade.trade.symbol = 1;
    trade.trade.instrument = InstrumentType::Spot;

    if (!initialized)
    {
      policy.initBar(trade, currentBar);
      initialized = true;
      continue;
    }

    if (policy.shouldClose(trade, currentBar))
    {
      bars.push_back(barToExtBar(currentBar));
      policy.initBar(trade, currentBar);
      continue;
    }

    policy.update(trade, currentBar);
  }

  if (initialized)
  {
    bars.push_back(barToExtBar(currentBar));
  }

  return bars;
}

template <typename Policy>
py::array_t<PyExtBar> aggregateBars(Policy policy, py::array_t<int64_t> timestamps,
                                    py::array_t<double> prices, py::array_t<double> quantities,
                                    py::array_t<uint8_t> isBuy)
{
  size_t n = timestamps.size();
  const auto* ts = timestamps.data();
  const auto* px = prices.data();
  const auto* qt = quantities.data();
  const auto* ib = isBuy.data();

  std::vector<PyExtBar> bars;
  {
    py::gil_scoped_release release;
    bars = doAggregate(policy, ts, px, qt, ib, n);
  }

  py::array_t<PyExtBar> result(bars.size());
  if (!bars.empty())
  {
    std::memcpy(result.mutable_data(), bars.data(), bars.size() * sizeof(PyExtBar));
  }
  return result;
}

}  // namespace

inline void bindAggregators(py::module_& m)
{
  PYBIND11_NUMPY_DTYPE(PyExtBar, start_time_ns, end_time_ns, open_raw, high_raw, low_raw,
                       close_raw, volume_raw, buy_volume_raw, trade_count);

  // Ensure time mapping is initialized
  flox::init_timebase_mapping();

  m.def(
      "aggregate_time_bars",
      [](py::array_t<int64_t> ts, py::array_t<double> px, py::array_t<double> qty,
         py::array_t<uint8_t> is_buy, double interval_seconds)
      {
        auto policy = flox::TimeBarPolicy(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::duration<double>(interval_seconds)));
        return aggregateBars(std::move(policy), ts, px, qty, is_buy);
      },
      "Aggregate trades into time bars",
      py::arg("timestamps"), py::arg("prices"), py::arg("quantities"),
      py::arg("is_buy"), py::arg("interval_seconds"));

  m.def(
      "aggregate_tick_bars",
      [](py::array_t<int64_t> ts, py::array_t<double> px, py::array_t<double> qty,
         py::array_t<uint8_t> is_buy, uint32_t tick_count)
      { return aggregateBars(flox::TickBarPolicy(tick_count), ts, px, qty, is_buy); },
      "Aggregate trades into tick bars",
      py::arg("timestamps"), py::arg("prices"), py::arg("quantities"),
      py::arg("is_buy"), py::arg("tick_count"));

  m.def(
      "aggregate_volume_bars",
      [](py::array_t<int64_t> ts, py::array_t<double> px, py::array_t<double> qty,
         py::array_t<uint8_t> is_buy, double volume_threshold)
      { return aggregateBars(flox::VolumeBarPolicy::fromDouble(volume_threshold), ts, px, qty, is_buy); },
      "Aggregate trades into volume bars",
      py::arg("timestamps"), py::arg("prices"), py::arg("quantities"),
      py::arg("is_buy"), py::arg("volume_threshold"));

  m.def(
      "aggregate_range_bars",
      [](py::array_t<int64_t> ts, py::array_t<double> px, py::array_t<double> qty,
         py::array_t<uint8_t> is_buy, double range_size)
      { return aggregateBars(flox::RangeBarPolicy::fromDouble(range_size), ts, px, qty, is_buy); },
      "Aggregate trades into range bars",
      py::arg("timestamps"), py::arg("prices"), py::arg("quantities"),
      py::arg("is_buy"), py::arg("range_size"));

  m.def(
      "aggregate_renko_bars",
      [](py::array_t<int64_t> ts, py::array_t<double> px, py::array_t<double> qty,
         py::array_t<uint8_t> is_buy, double brick_size)
      { return aggregateBars(flox::RenkoBarPolicy::fromDouble(brick_size), ts, px, qty, is_buy); },
      "Aggregate trades into renko bars",
      py::arg("timestamps"), py::arg("prices"), py::arg("quantities"),
      py::arg("is_buy"), py::arg("brick_size"));

  m.def(
      "aggregate_heikin_ashi_bars",
      [](py::array_t<int64_t> ts, py::array_t<double> px, py::array_t<double> qty,
         py::array_t<uint8_t> is_buy, double interval_seconds)
      {
        auto policy = flox::HeikinAshiBarPolicy(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::duration<double>(interval_seconds)));
        return aggregateBars(std::move(policy), ts, px, qty, is_buy);
      },
      "Aggregate trades into Heikin-Ashi bars",
      py::arg("timestamps"), py::arg("prices"), py::arg("quantities"),
      py::arg("is_buy"), py::arg("interval_seconds"));
}
