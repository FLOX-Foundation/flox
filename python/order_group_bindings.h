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

#include "flox/execution/order_group.h"

namespace py = pybind11;

namespace flox_py
{

inline void bindOrderGroup(py::module_& m)
{
  py::enum_<flox::OrderGroupPolicy>(m, "OrderGroupPolicy")
      .value("BEST_EFFORT", flox::OrderGroupPolicy::BestEffort)
      .value("ALL_OR_NOTHING", flox::OrderGroupPolicy::AllOrNothing)
      .value("ONE_SIDED", flox::OrderGroupPolicy::OneSided)
      .export_values();

  py::enum_<flox::LegState>(m, "LegState")
      .value("PENDING", flox::LegState::Pending)
      .value("SUBMITTED", flox::LegState::Submitted)
      .value("PARTIALLY_FILLED", flox::LegState::PartiallyFilled)
      .value("FILLED", flox::LegState::Filled)
      .value("CANCELLED", flox::LegState::Cancelled)
      .value("FAILED", flox::LegState::Failed)
      .export_values();

  py::enum_<flox::OrderGroupState>(m, "OrderGroupState")
      .value("PENDING", flox::OrderGroupState::Pending)
      .value("SUBMITTED", flox::OrderGroupState::Submitted)
      .value("PARTIALLY_FILLED", flox::OrderGroupState::PartiallyFilled)
      .value("FILLED", flox::OrderGroupState::Filled)
      .value("CANCELLED", flox::OrderGroupState::Cancelled)
      .value("REVERTING", flox::OrderGroupState::Reverting)
      .value("FAILED", flox::OrderGroupState::Failed)
      .export_values();

  py::class_<flox::OrderGroup>(m, "OrderGroup")
      .def(py::init([](uint64_t parent_signal_id, flox::OrderGroupPolicy policy)
                    { return new flox::OrderGroup(parent_signal_id, policy); }),
           py::arg("parent_signal_id") = 0,
           py::arg("policy") = flox::OrderGroupPolicy::BestEffort)
      .def("parent_signal_id", &flox::OrderGroup::parentSignalId)
      .def("policy", &flox::OrderGroup::policy)
      .def("add_market_leg", [](flox::OrderGroup& g, uint32_t symbol, uint8_t side, double qty)
           { return g.addMarketLeg(symbol, side, flox::Quantity::fromDouble(qty)); }, py::arg("symbol"), py::arg("side"), py::arg("qty"))
      .def("add_limit_leg", [](flox::OrderGroup& g, uint32_t symbol, uint8_t side, double price, double qty)
           { return g.addLimitLeg(symbol, side, flox::Price::fromDouble(price),
                                  flox::Quantity::fromDouble(qty)); }, py::arg("symbol"), py::arg("side"), py::arg("price"), py::arg("qty"))
      .def("leg_count", &flox::OrderGroup::legCount)
      .def("leg_state", [](const flox::OrderGroup& g, size_t idx)
           { return g.leg(idx).state; }, py::arg("leg_index"))
      .def("leg_filled", [](const flox::OrderGroup& g, size_t idx)
           { return g.leg(idx).filledQty.toDouble(); }, py::arg("leg_index"))
      .def("leg_order_id", [](const flox::OrderGroup& g, size_t idx)
           { return g.leg(idx).orderId; }, py::arg("leg_index"))
      .def("record_submit", &flox::OrderGroup::recordSubmit, py::arg("leg_index"), py::arg("order_id"))
      .def("record_fill", [](flox::OrderGroup& g, size_t idx, double cumulative_qty)
           { g.recordFill(idx, flox::Quantity::fromDouble(cumulative_qty)); }, py::arg("leg_index"), py::arg("cumulative_qty"))
      .def("record_cancel", &flox::OrderGroup::recordCancel, py::arg("leg_index"))
      .def("record_failure", &flox::OrderGroup::recordFailure, py::arg("leg_index"))
      .def("state", &flox::OrderGroup::state)
      .def("recommended_actions", [](const flox::OrderGroup& g)
           {
        py::list out;
        for (const auto& a : g.recommendedActions())
        {
          py::dict d;
          d["kind"] = a.kind == flox::OrderGroupAction::Kind::CancelLeg ? "cancel" : "revert";
          d["leg_index"] = a.legIndex;
          if (a.kind == flox::OrderGroupAction::Kind::CancelLeg)
          {
            d["order_id"] = a.orderId;
          }
          else
          {
            d["symbol"] = a.symbol;
            d["side"] = a.side;
            d["qty"] = a.qty.toDouble();
          }
          out.append(d);
        }
        return out; });
}

}  // namespace flox_py
