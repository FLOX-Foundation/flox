// python/aggregator_bindings.h

#pragma once

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "bindings_common.h"
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

// PyExtBar is the structured numpy dtype returned by the `aggregate_*_bars`
// helpers. Keeping it at file scope (rather than inside an anonymous
// namespace) gives the type a stable mangled name so pybind11-stubgen can
// resolve `py::array_t<PyExtBar>` to `numpy.ndarray[PyExtBar]` instead of
// `numpy.ndarray[...]` (Any).
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

namespace
{

using namespace flox;

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

// Batch aggregation: takes pre-extracted vectors (no GIL needed).
//
// This is one of four copies of the close path (the others: BarAggregator,
// MultiTimeframeAggregator, and doAggregateC in the C ABI). The two policy
// customization points -- a late trade the policy recognizes, and a policy
// that owns its own close, both declared in aggregation_policy.h -- are
// branched on here in the same order and with the same meaning, so the four
// produce the same bars for the same trades.
//
// Only fully closed bars are returned -- the trailing bar still open when
// the input runs out is dropped, exactly like the C ABI's doAggregateC
// (src/capi/flox_capi.cpp), which every other binding (Node, QuickJS,
// Codon) goes through. This used to append that trailing bar unconditionally,
// which made the bar count binding-dependent (Python returned one more bar
// than everyone else on the same input) and input-order-dependent (appending
// one more trade could silently turn "the last real bar" into "a different,
// still-open bar" with no way to tell the two apart -- FloxBar/PyExtBar
// carry no closed/partial flag). A batch call's result has to depend only on
// its input, not on where the caller happened to stop feeding it; a
// consumer that wants the currently-open bar during live streaming has that
// through a different, already-existing path: BarAggregator (bus-fed) via
// Strategy::lastClosedBar, or the partial bar surfaced with
// BarCloseReason::Forced when BarAggregator::stop() flushes it.
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
    trade.trade.exchangeTsNs = UnixNanos::fromRaw(ts[i]);
    trade.trade.symbol = 1;
    trade.trade.instrument = InstrumentType::Spot;

    if (!initialized)
    {
      policy.initBar(trade, currentBar);
      initialized = true;
      continue;
    }

    if constexpr (flox::DetectsLateTrades<Policy>)
    {
      if (policy.isLate(trade, currentBar))
      {
        continue;
      }
    }

    if (policy.shouldClose(trade, currentBar))
    {
      if constexpr (flox::ClosesAndReopens<Policy>)
      {
        policy.closeAndReopen(trade, currentBar,
                              [&bars](const Bar& bar)
                              { bars.push_back(barToExtBar(bar)); });
      }
      else
      {
        bars.push_back(barToExtBar(currentBar));
        policy.initBar(trade, currentBar);
      }
      continue;
    }

    policy.update(trade, currentBar);
  }

  return bars;
}

template <typename Policy>
py::array_t<PyExtBar> aggregateBars(
    Policy policy, py::array_t<int64_t, py::array::c_style | py::array::forcecast> timestamps,
    py::array_t<double, py::array::c_style | py::array::forcecast> prices,
    py::array_t<double, py::array::c_style | py::array::forcecast> quantities,
    py::array_t<uint8_t, py::array::c_style | py::array::forcecast> isBuy)
{
  size_t n = timestamps.size();
  checkSameSize(n, static_cast<size_t>(prices.size()), "timestamps and prices size");
  checkSameSize(n, static_cast<size_t>(quantities.size()), "timestamps and quantities size");
  checkSameSize(n, static_cast<size_t>(isBuy.size()), "timestamps and is_buy size");
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
      [](py::array_t<int64_t, py::array::c_style | py::array::forcecast> ts,
         py::array_t<double, py::array::c_style | py::array::forcecast> px,
         py::array_t<double, py::array::c_style | py::array::forcecast> qty,
         py::array_t<uint8_t, py::array::c_style | py::array::forcecast> is_buy, double interval_seconds)
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
      [](py::array_t<int64_t, py::array::c_style | py::array::forcecast> ts,
         py::array_t<double, py::array::c_style | py::array::forcecast> px,
         py::array_t<double, py::array::c_style | py::array::forcecast> qty,
         py::array_t<uint8_t, py::array::c_style | py::array::forcecast> is_buy, uint32_t tick_count)
      { return aggregateBars(flox::TickBarPolicy(tick_count), ts, px, qty, is_buy); },
      "Aggregate trades into tick bars",
      py::arg("timestamps"), py::arg("prices"), py::arg("quantities"),
      py::arg("is_buy"), py::arg("tick_count"));

  m.def(
      "aggregate_volume_bars",
      [](py::array_t<int64_t, py::array::c_style | py::array::forcecast> ts,
         py::array_t<double, py::array::c_style | py::array::forcecast> px,
         py::array_t<double, py::array::c_style | py::array::forcecast> qty,
         py::array_t<uint8_t, py::array::c_style | py::array::forcecast> is_buy, double volume_threshold)
      { return aggregateBars(flox::VolumeBarPolicy::fromDouble(volume_threshold), ts, px, qty, is_buy); },
      "Aggregate trades into volume bars",
      py::arg("timestamps"), py::arg("prices"), py::arg("quantities"),
      py::arg("is_buy"), py::arg("volume_threshold"));

  m.def(
      "aggregate_range_bars",
      [](py::array_t<int64_t, py::array::c_style | py::array::forcecast> ts,
         py::array_t<double, py::array::c_style | py::array::forcecast> px,
         py::array_t<double, py::array::c_style | py::array::forcecast> qty,
         py::array_t<uint8_t, py::array::c_style | py::array::forcecast> is_buy, double range_size)
      { return aggregateBars(flox::RangeBarPolicy::fromDouble(range_size), ts, px, qty, is_buy); },
      "Aggregate trades into range bars",
      py::arg("timestamps"), py::arg("prices"), py::arg("quantities"),
      py::arg("is_buy"), py::arg("range_size"));

  m.def(
      "aggregate_renko_bars",
      [](py::array_t<int64_t, py::array::c_style | py::array::forcecast> ts,
         py::array_t<double, py::array::c_style | py::array::forcecast> px,
         py::array_t<double, py::array::c_style | py::array::forcecast> qty,
         py::array_t<uint8_t, py::array::c_style | py::array::forcecast> is_buy, double brick_size)
      { return aggregateBars(flox::RenkoBarPolicy::fromDouble(brick_size), ts, px, qty, is_buy); },
      "Aggregate trades into renko bars",
      py::arg("timestamps"), py::arg("prices"), py::arg("quantities"),
      py::arg("is_buy"), py::arg("brick_size"));

  m.def(
      "aggregate_heikin_ashi_bars",
      [](py::array_t<int64_t, py::array::c_style | py::array::forcecast> ts,
         py::array_t<double, py::array::c_style | py::array::forcecast> px,
         py::array_t<double, py::array::c_style | py::array::forcecast> qty,
         py::array_t<uint8_t, py::array::c_style | py::array::forcecast> is_buy, double interval_seconds)
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
