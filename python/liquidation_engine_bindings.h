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

#include "backtest_bindings.h"
#include "flox/backtest/account.h"
#include "flox/backtest/liquidation_engine.h"

namespace py = pybind11;

namespace flox_py
{

inline void bindLiquidationEngine(py::module_& m)
{
  py::enum_<flox::AdlRanking>(m, "AdlRanking")
      .value("PnlRatio", flox::AdlRanking::PnlRatio)
      .value("Binance", flox::AdlRanking::Binance)
      .value("Bybit", flox::AdlRanking::Bybit)
      .value("PositionSize", flox::AdlRanking::PositionSize)
      .export_values();
  py::enum_<flox::LiquidationEngine::MarkImpactModel>(m, "MarkImpactModel")
      .value("None_", flox::LiquidationEngine::MarkImpactModel::None)
      .value("BookAnchored", flox::LiquidationEngine::MarkImpactModel::BookAnchored)
      .value("BookOnly", flox::LiquidationEngine::MarkImpactModel::BookOnly)
      .export_values();
  py::class_<flox::LiquidationEngine>(m, "LiquidationEngine")
      .def(py::init<>())
      .def("add_tier", [](flox::LiquidationEngine& self, double min_notional, double mm_fraction)
           {
             self.addTier(min_notional, mm_fraction);
             return &self; }, py::arg("min_notional"), py::arg("mm_fraction"), py::return_value_policy::reference_internal)
      .def("set_insurance_fund_capital", &flox::LiquidationEngine::setInsuranceFundCapital, py::arg("capital"))
      .def("insurance_fund_balance", &flox::LiquidationEngine::insuranceFundBalance)
      .def("set_adl_enabled", &flox::LiquidationEngine::setAdlEnabled, py::arg("enabled"))
      .def("adl_enabled", &flox::LiquidationEngine::adlEnabled)
      .def("set_adl_ranking", [](flox::LiquidationEngine& self, py::object ranking)
           {
             if (py::isinstance<py::str>(ranking))
             {
               self.setAdlRankingByName(ranking.cast<std::string>());
             }
             else
             {
               self.setAdlRanking(ranking.cast<flox::AdlRanking>());
             } },
           "Set the ADL queue ordering. Accepts an AdlRanking enum or a "
           "string name (pnl_ratio, binance, bybit, position_size).",
           py::arg("ranking"))
      .def("adl_ranking", &flox::LiquidationEngine::adlRanking)
      .def("set_liquidation_slippage_bps", &flox::LiquidationEngine::setLiquidationSlippageBps, py::arg("bps"))
      .def("open_position", [](flox::LiquidationEngine& self, uint64_t account_id, uint32_t symbol, double quantity, double entry_price, double equity)
           { self.openPosition(flox::LeveragedPosition{
                 .accountId = account_id, .symbol = symbol, .quantity = quantity, .entryPrice = entry_price, .equity = equity}); }, py::arg("account_id"), py::arg("symbol"), py::arg("quantity"), py::arg("entry_price"), py::arg("equity"))
      .def("close_position", &flox::LiquidationEngine::closePosition, py::arg("account_id"), py::arg("symbol"))
      .def("on_mark", [](flox::LiquidationEngine& self, uint32_t symbol, double mark_price)
           {
             auto outcome = self.onMark(symbol, mark_price);
             py::dict d;
             d["liquidated"] = outcome.liquidated;
             d["adl_closed_out"] = outcome.adlClosedOut;
             d["insurance_fund_delta"] = outcome.insuranceFundDelta;
             d["liquidations_count"] = outcome.liquidationsCount;
             d["insurance_payments_count"] = outcome.insurancePaymentsCount;
             d["adl_closeouts_count"] = outcome.adlCloseoutsCount;
             return d; }, py::arg("symbol"), py::arg("mark_price"))
      .def("liquidations_count", &flox::LiquidationEngine::liquidationsCount)
      .def("insurance_payments_count", &flox::LiquidationEngine::insurancePaymentsCount)
      .def("adl_closeouts_count", &flox::LiquidationEngine::adlCloseoutsCount)
      .def("position_count", [](const flox::LiquidationEngine& self)
           { return self.positions().size(); })
      .def("set_executor", [](flox::LiquidationEngine& self, py::object py_exec)
           {
             if (py_exec.is_none())
             {
               self.setExecutor(nullptr);
               return;
             }
             auto* w = py_exec.cast<PySimulatedExecutor*>();
             self.setExecutor(&w->executor()); },
           "Attach a SimulatedExecutor so liquidation orders route "
           "through it as market orders. Pass None to detach.",
           py::arg("executor"), py::keep_alive<1, 2>())
      .def("deficits_paid_by_fund", [](const flox::LiquidationEngine& self)
           { return self.deficitsPaidByFund(); })
      .def("deficits_paid_by_adl", [](const flox::LiquidationEngine& self)
           { return self.deficitsPaidByAdl(); })
      .def("cascade_sizes_per_tick", [](const flox::LiquidationEngine& self)
           { return self.cascadeSizesPerTick(); })
      .def("fund_balance_history", [](const flox::LiquidationEngine& self)
           { return self.fundBalanceHistory(); })
      .def("ticks_to_first_adl", &flox::LiquidationEngine::ticksToFirstAdl)
      .def("reset_stats", &flox::LiquidationEngine::resetStats)
      .def("set_mark_impact_model", [](flox::LiquidationEngine& self, py::object model, double weight)
           {
             if (py::isinstance<py::str>(model))
             {
               self.setMarkImpactModelByName(model.cast<std::string>(), weight);
             }
             else
             {
               self.setMarkImpactModel(
                   model.cast<flox::LiquidationEngine::MarkImpactModel>(), weight);
             } },
           "Configure mark-impact feedback after liquidations. Accepts a "
           "MarkImpactModel enum or a string name (none, book_anchored, "
           "book_only). `weight` is the blend factor used by book_anchored.",
           py::arg("model"), py::arg("weight") = 0.3)
      .def("mark_impact_model", &flox::LiquidationEngine::markImpactModel)
      .def("mark_impact_weight", &flox::LiquidationEngine::markImpactWeight)
      .def("set_max_cascade_depth", &flox::LiquidationEngine::setMaxCascadeDepth, py::arg("depth"))
      .def("max_cascade_depth", &flox::LiquidationEngine::maxCascadeDepth)
      .def("attach_account", &flox::LiquidationEngine::attachAccount, py::arg("account"), py::keep_alive<1, 2>(),
           "Attach an Account so its positions participate in cross-margin "
           "MM checks. Multiple accounts may be attached.")
      .def("detach_account", &flox::LiquidationEngine::detachAccount, py::arg("account_id"))
      .def("account_count", [](const flox::LiquidationEngine& self)
           { return self.accounts().size(); })
      .def_static("binance_um_futures", &flox::LiquidationEngine::binance_um_futures)
      .def_static("bybit_linear", &flox::LiquidationEngine::bybit_linear)
      .def_static("okx_swap", &flox::LiquidationEngine::okx_swap);
}

}  // namespace flox_py
