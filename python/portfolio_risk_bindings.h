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

#include <memory>
#include <optional>
#include <string>

#include "flox/risk/portfolio_risk.h"

namespace py = pybind11;

namespace flox_py
{

inline py::dict accountToDict(const flox::risk::StrategyAccount& a)
{
  py::dict d;
  d["name"] = a.name;
  d["realized_pnl"] = a.realized_pnl;
  d["unrealized_pnl"] = a.unrealized_pnl;
  d["fees"] = a.fees;
  d["gross_exposure"] = a.gross_exposure;
  d["net_exposure"] = a.net_exposure;
  d["trade_count"] = a.trade_count;
  d["daily_pnl"] = a.dailyPnl();
  return d;
}

inline py::dict breachToDict(const flox::risk::Breach& b)
{
  py::dict d;
  d["rule"] = b.rule;
  d["value"] = b.value;
  d["limit"] = b.limit;
  d["detail"] = b.detail;
  return d;
}

inline py::dict snapshotToDict(const flox::risk::PortfolioSnapshot& s)
{
  py::dict d;
  d["total_realized_pnl"] = s.total_realized_pnl;
  d["total_unrealized_pnl"] = s.total_unrealized_pnl;
  d["total_fees"] = s.total_fees;
  d["total_daily_pnl"] = s.total_daily_pnl;
  d["total_gross_exposure"] = s.total_gross_exposure;
  d["total_net_exposure"] = s.total_net_exposure;
  d["total_trade_count"] = s.total_trade_count;
  d["current_equity"] = s.current_equity;
  d["peak_equity"] = s.peak_equity;
  d["drawdown_pct"] = s.drawdown_pct;
  d["kill_switch_active"] = s.kill_switch_active;
  py::list breaches;
  for (const auto& b : s.active_breaches)
  {
    breaches.append(breachToDict(b));
  }
  d["active_breaches"] = breaches;
  py::list accounts;
  for (const auto& a : s.accounts)
  {
    accounts.append(accountToDict(a));
  }
  d["accounts"] = accounts;
  return d;
}

inline void bindPortfolioRisk(py::module_& m)
{
  using namespace flox::risk;

  py::class_<RiskRules>(m, "_PortfolioRiskRules", "Internal C++-backed risk rules struct.")
      .def(py::init<>())
      .def_readwrite("max_drawdown_pct", &RiskRules::max_drawdown_pct)
      .def_readwrite("max_daily_loss", &RiskRules::max_daily_loss)
      .def_readwrite("max_gross_exposure", &RiskRules::max_gross_exposure)
      .def_readwrite("max_concentration_pct", &RiskRules::max_concentration_pct);

  py::class_<PortfolioRiskAggregator>(m, "_PortfolioRiskAggregator",
                                      "C++-backed aggregator. Public Python users go through "
                                      "flox_py.portfolio_risk.PortfolioRiskAggregator which preserves "
                                      "the existing dataclass surface.")
      .def(py::init<RiskRules, double>(),
           py::arg("rules") = RiskRules{},
           py::arg("initial_equity") = 0.0)
      .def("update", [](PortfolioRiskAggregator& self, const std::string& name, double realized_pnl, double unrealized_pnl, double fees, double gross_exposure, double net_exposure, uint64_t trade_count, uint8_t field_mask)
           {
             StrategyAccount fields;
             fields.realized_pnl = realized_pnl;
             fields.unrealized_pnl = unrealized_pnl;
             fields.fees = fees;
             fields.gross_exposure = gross_exposure;
             fields.net_exposure = net_exposure;
             fields.trade_count = trade_count;
             self.update(name, fields, field_mask); }, py::arg("name"), py::arg("realized_pnl") = 0.0, py::arg("unrealized_pnl") = 0.0, py::arg("fees") = 0.0, py::arg("gross_exposure") = 0.0, py::arg("net_exposure") = 0.0, py::arg("trade_count") = 0, py::arg("field_mask") = 0x3F)
      .def("remove", &PortfolioRiskAggregator::remove, py::arg("name"))
      .def("reset_kill_switch", &PortfolioRiskAggregator::resetKillSwitch)
      .def("check_order", [](const PortfolioRiskAggregator& self, const std::string& strategy, double notional, const std::string& side) -> py::object
           {
             auto opt = self.checkOrder(strategy, notional, side);
             if (!opt.has_value()){ return py::none();
}
             return breachToDict(*opt); }, py::arg("strategy"), py::arg("notional"), py::arg("side"))
      .def("snapshot", [](const PortfolioRiskAggregator& self)
           { return snapshotToDict(self.snapshot()); });
}

}  // namespace flox_py
