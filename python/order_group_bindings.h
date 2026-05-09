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
      .def("mark_action_dispatched", [](flox::OrderGroup& g, size_t idx, const std::string& kind)
           {
            auto k = (kind == "cancel") ? flox::OrderGroupAction::Kind::CancelLeg
                                        : flox::OrderGroupAction::Kind::RevertLeg;
            g.markActionDispatched(idx, k); }, py::arg("leg_index"), py::arg("kind"))
      .def("auto_dispatch", [](flox::OrderGroup& g, py::object strategy)
           {
            // Dispatch every not-yet-dispatched recommended action
            // through the strategy's emit helpers. Marks each action
            // as dispatched so subsequent calls don't re-fire it.
            size_t fired = 0;
            for (const auto& a : g.recommendedActions())
            {
              if (a.kind == flox::OrderGroupAction::Kind::CancelLeg)
              {
                strategy.attr("emit_cancel")(a.orderId);
              }
              else
              {
                if (a.side == 0)
                {
                  strategy.attr("emit_market_buy")(a.symbol, a.qty.toDouble());
                }
                else
                {
                  strategy.attr("emit_market_sell")(a.symbol, a.qty.toDouble());
                }
              }
              g.markActionDispatched(a.legIndex, a.kind);
              ++fired;
            }
            return fired; }, py::arg("strategy"))
      .def("set_risk_limits", [](flox::OrderGroup& g, double max_gross_notional, double max_concentration_pct, double max_leg_qty)
           {
            flox::GroupRiskLimits limits;
            limits.maxGrossNotional = flox::Quantity::fromDouble(max_gross_notional);
            limits.maxConcentrationPct = max_concentration_pct;
            limits.maxLegQty = flox::Quantity::fromDouble(max_leg_qty);
            g.setRiskLimits(limits); }, py::arg("max_gross_notional") = 0.0, py::arg("max_concentration_pct") = 0.0, py::arg("max_leg_qty") = 0.0)
      .def("precheck_submission", [](const flox::OrderGroup& g, double equity, const std::vector<double>& market_ref_prices)
           {
            std::vector<flox::Price> prices;
            prices.reserve(market_ref_prices.size());
            for (double p : market_ref_prices){
              prices.push_back(flox::Price::fromDouble(p));
}
            auto breach = g.precheckSubmission(equity, prices);
            py::dict d;
            d["denied"] = breach.denied;
            d["rule"] = breach.rule;
            d["detail"] = breach.detail;
            return d; }, py::arg("equity") = 0.0, py::arg("market_ref_prices") = std::vector<double>{})
      .def("set_pair_latency_budget_ns", &flox::OrderGroup::setPairLatencyBudgetNs, py::arg("budget_ns"))
      .def("pair_latency_decision", [](const flox::OrderGroup& g, int64_t leader_submit_ts_ns, int64_t leader_ack_ts_ns, bool ack_received)
           {
            auto d = g.pairLatencyDecision(leader_submit_ts_ns, leader_ack_ts_ns,
                                            ack_received);
            switch (d)
            {
              case flox::PairLatencyDecision::Wait: return std::string("wait");
              case flox::PairLatencyDecision::SubmitFollower:
                return std::string("submit_follower");
              case flox::PairLatencyDecision::CancelLeader:
                return std::string("cancel_leader");
            }
            return std::string("wait"); }, py::arg("leader_submit_ts_ns"), py::arg("leader_ack_ts_ns"), py::arg("ack_received") = false)
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
