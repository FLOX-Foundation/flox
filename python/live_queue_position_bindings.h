/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "flox/execution/live_queue_position_estimator.h"

namespace py = pybind11;

namespace flox_py
{

inline void bindLiveQueuePositionEstimator(py::module_& m)
{
  py::class_<flox::LiveQueuePositionEstimator>(m, "LiveQueuePositionEstimator")
      .def(py::init<>())
      .def("set_confidence_half_life_ns",
           &flox::LiveQueuePositionEstimator::setConfidenceHalfLifeNs,
           py::arg("half_life_ns"))
      .def("set_shrink_attribution_factor",
           &flox::LiveQueuePositionEstimator::setShrinkAttributionFactor,
           py::arg("factor"))
      .def(
          "on_order_placed",
          [](flox::LiveQueuePositionEstimator& e, uint32_t symbol, uint8_t side,
             double price, uint64_t order_id, double order_qty, double level_qty,
             int64_t ts_ns)
          {
            e.onOrderPlaced(symbol, static_cast<flox::Side>(side),
                            flox::Price::fromDouble(price), order_id,
                            flox::Quantity::fromDouble(order_qty),
                            flox::Quantity::fromDouble(level_qty), ts_ns);
          },
          py::arg("symbol"), py::arg("side"), py::arg("price"), py::arg("order_id"),
          py::arg("order_qty"), py::arg("level_qty_now"), py::arg("ts_ns") = 0)
      .def("on_order_cancelled", &flox::LiveQueuePositionEstimator::onOrderCancelled,
           py::arg("order_id"), py::arg("ts_ns") = 0)
      .def(
          "on_order_filled",
          [](flox::LiveQueuePositionEstimator& e, uint64_t order_id,
             double cumulative_fill, int64_t ts_ns)
          {
            e.onOrderFilled(order_id, flox::Quantity::fromDouble(cumulative_fill), ts_ns);
          },
          py::arg("order_id"), py::arg("cumulative_fill"), py::arg("ts_ns") = 0)
      .def(
          "on_trade",
          [](flox::LiveQueuePositionEstimator& e, uint32_t symbol, double price,
             double qty, int64_t ts_ns)
          {
            e.onTrade(symbol, flox::Price::fromDouble(price),
                      flox::Quantity::fromDouble(qty), ts_ns);
          },
          py::arg("symbol"), py::arg("price"), py::arg("qty"), py::arg("ts_ns") = 0)
      .def(
          "on_level_update",
          [](flox::LiveQueuePositionEstimator& e, uint32_t symbol, uint8_t side,
             double price, double new_qty, int64_t ts_ns)
          {
            e.onLevelUpdate(symbol, static_cast<flox::Side>(side),
                            flox::Price::fromDouble(price),
                            flox::Quantity::fromDouble(new_qty), ts_ns);
          },
          py::arg("symbol"), py::arg("side"), py::arg("price"), py::arg("new_qty"),
          py::arg("ts_ns") = 0)
      .def(
          "snapshot",
          [](const flox::LiveQueuePositionEstimator& e, uint64_t order_id,
             int64_t now_ns) -> py::object
          {
            auto s = e.snapshot(order_id, now_ns);
            if (!s.has_value())
            {
              return py::none();
            }
            py::dict d;
            d["order_id"] = s->orderId;
            d["queue_ahead_est"] = s->queueAheadEst.toDouble();
            d["total"] = s->total.toDouble();
            d["confidence"] = s->confidence;
            d["last_update_ns"] = s->lastUpdateNs;
            return d;
          },
          py::arg("order_id"), py::arg("now_ns") = 0)
      .def("tracked_order_count", &flox::LiveQueuePositionEstimator::trackedOrderCount);
}

}  // namespace flox_py
