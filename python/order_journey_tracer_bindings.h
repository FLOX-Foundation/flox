/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "flox/common.h"
#include "flox/execution/events/order_event.h"
#include "flox/execution/order_journey_tracer.h"

namespace py = pybind11;

namespace
{
struct PyOrderTraceRow
{
  uint64_t order_id;
  uint32_t seq;
  uint8_t status;
  uint8_t is_maker;
  uint8_t _pad[2];
  int64_t ts_ns;
  double fill_qty;
  double fill_price;
  double queue_ahead;
  double queue_total;
  int64_t submitted_at_ns;
  int64_t accepted_at_ns;
  int64_t first_fill_at_ns;
  int64_t last_fill_at_ns;
  int64_t canceled_at_ns;
  int64_t rejected_at_ns;
  int64_t triggered_at_ns;
  int64_t expired_at_ns;
};

inline PyOrderTraceRow toPyRow(const flox::OrderTraceRecord& r)
{
  PyOrderTraceRow out{};
  out.order_id = r.orderId;
  out.seq = r.seq;
  out.status = r.status;
  out.is_maker = r.isMaker;
  out.ts_ns = r.tsNs;
  out.fill_qty = static_cast<double>(r.fillQtyRaw) / 1e8;
  out.fill_price = static_cast<double>(r.fillPriceRaw) / 1e8;
  out.queue_ahead = static_cast<double>(r.queueAheadRaw) / 1e8;
  out.queue_total = static_cast<double>(r.queueTotalRaw) / 1e8;
  out.submitted_at_ns = r.timestamps.submittedAtNs;
  out.accepted_at_ns = r.timestamps.acceptedAtNs;
  out.first_fill_at_ns = r.timestamps.firstFillAtNs;
  out.last_fill_at_ns = r.timestamps.lastFillAtNs;
  out.canceled_at_ns = r.timestamps.canceledAtNs;
  out.rejected_at_ns = r.timestamps.rejectedAtNs;
  out.triggered_at_ns = r.timestamps.triggeredAtNs;
  out.expired_at_ns = r.timestamps.expiredAtNs;
  return out;
}
}  // namespace

inline void registerOrderJourneyTracer(py::module_& m)
{
  PYBIND11_NUMPY_DTYPE(PyOrderTraceRow, order_id, seq, status, is_maker, ts_ns,
                       fill_qty, fill_price, queue_ahead, queue_total,
                       submitted_at_ns, accepted_at_ns, first_fill_at_ns,
                       last_fill_at_ns, canceled_at_ns, rejected_at_ns,
                       triggered_at_ns, expired_at_ns);

  py::class_<flox::OrderJourneyTracer>(m, "OrderJourneyTracer")
      .def(py::init([](size_t max_orders, size_t max_records_per_order,
                       double sample_rate, uint64_t sample_salt)
                    {
                      flox::OrderJourneyTracer::Config cfg{};
                      cfg.maxOrders = max_orders;
                      cfg.maxRecordsPerOrder = max_records_per_order;
                      cfg.sampleRate = sample_rate;
                      cfg.sampleSalt = sample_salt;
                      return std::make_unique<flox::OrderJourneyTracer>(cfg); }),
           py::arg("max_orders") = 1'000'000, py::arg("max_records_per_order") = 64,
           py::arg("sample_rate") = 1.0,
           py::arg("sample_salt") = 0x9E3779B97F4A7C15ULL)
      .def(
          "result",
          [](const flox::OrderJourneyTracer& self)
          {
            auto rows = self.result();
            py::array_t<PyOrderTraceRow> arr(rows.size());
            auto* out = arr.mutable_data();
            for (size_t i = 0; i < rows.size(); ++i)
            {
              out[i] = toPyRow(rows[i]);
            }
            return arr;
          },
          "Return the full trace as a numpy structured array, one row per event.")
      .def(
          "journey",
          [](const flox::OrderJourneyTracer& self, uint64_t order_id)
          {
            auto rows = self.journey(order_id);
            py::array_t<PyOrderTraceRow> arr(rows.size());
            auto* out = arr.mutable_data();
            for (size_t i = 0; i < rows.size(); ++i)
            {
              out[i] = toPyRow(rows[i]);
            }
            return arr;
          },
          py::arg("order_id"),
          "Return the trace for one order as a numpy structured array.")
      .def("order_count", &flox::OrderJourneyTracer::orderCount)
      .def("record_count", &flox::OrderJourneyTracer::recordCount)
      .def("median_ack_latency_ns", &flox::OrderJourneyTracer::medianAckLatencyNs)
      .def("median_time_to_first_fill_ns",
           &flox::OrderJourneyTracer::medianTimeToFirstFillNs)
      .def("maker_fill_ratio", &flox::OrderJourneyTracer::makerFillRatio)
      .def("cancel_race_loss_rate", &flox::OrderJourneyTracer::cancelRaceLossRate)
      .def("clear", &flox::OrderJourneyTracer::clear);
}
