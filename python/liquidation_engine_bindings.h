/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "backtest_bindings.h"
#include "lrvx/backtest/account.h"
#include "lrvx/backtest/liquidation_engine.h"

namespace py = pybind11;

namespace lrvx
{

inline void bindLiquidationEngine(py::module_& m)
{
  py::enum_<lrvx::AdlRanking>(m, "AdlRanking")
      .value("PnlRatio", lrvx::AdlRanking::PnlRatio)
      .value("Binance", lrvx::AdlRanking::Binance)
      .value("Bybit", lrvx::AdlRanking::Bybit)
      .value("PositionSize", lrvx::AdlRanking::PositionSize)
      .export_values();
  py::enum_<lrvx::LiquidationEngine::MarkImpactModel>(m, "MarkImpactModel")
      .value("None_", lrvx::LiquidationEngine::MarkImpactModel::None)
      .value("BookAnchored", lrvx::LiquidationEngine::MarkImpactModel::BookAnchored)
      .value("BookOnly", lrvx::LiquidationEngine::MarkImpactModel::BookOnly)
      .export_values();
  py::class_<lrvx::LiquidationEngine>(m, "LiquidationEngine")
      .def(py::init<>())
      .def("add_tier", [](lrvx::LiquidationEngine& self, double min_notional, double mm_fraction)
           {
             self.addTier(min_notional, mm_fraction);
             return &self; }, py::arg("min_notional"), py::arg("mm_fraction"), py::return_value_policy::reference_internal)
      .def("set_insurance_fund_capital", &lrvx::LiquidationEngine::setInsuranceFundCapital, py::arg("capital"))
      .def("insurance_fund_balance", &lrvx::LiquidationEngine::insuranceFundBalance)
      .def("set_adl_enabled", &lrvx::LiquidationEngine::setAdlEnabled, py::arg("enabled"))
      .def("adl_enabled", &lrvx::LiquidationEngine::adlEnabled)
      .def("set_adl_ranking", [](lrvx::LiquidationEngine& self, py::object ranking)
           {
             if (py::isinstance<py::str>(ranking))
             {
               self.setAdlRankingByName(ranking.cast<std::string>());
             }
             else
             {
               self.setAdlRanking(ranking.cast<lrvx::AdlRanking>());
             } },
           "Set the ADL queue ordering. Accepts an AdlRanking enum or a "
           "string name (pnl_ratio, binance, bybit, position_size).",
           py::arg("ranking"))
      .def("adl_ranking", &lrvx::LiquidationEngine::adlRanking)
      .def("set_liquidation_slippage_bps", &lrvx::LiquidationEngine::setLiquidationSlippageBps, py::arg("bps"))
      // contract_multiplier / is_long_option mirror Account::openPosition's
      // binding (account_bindings.h). Without them every position opened
      // through this entry point -- the only way to register an orphan
      // (accountless) leveraged position -- got the LeveragedPosition
      // default of 1.0, so the contract-multiplier scaling that
      // onMarkOnce/walkIsolatedAccount apply to the realized close (options
      // at 100x, ES at 50x) could never be exercised from Python: every
      // orphan leg looked like a 1x perp no matter what the caller intended.
      .def("open_position", [](lrvx::LiquidationEngine& self, uint64_t account_id, uint32_t symbol, double quantity, double entry_price, double equity, double contract_multiplier, bool is_long_option)
           { self.openPosition(lrvx::LeveragedPosition{
                 .accountId = account_id, .symbol = symbol, .quantity = lrvx::Quantity::fromDouble(quantity), .entryPrice = lrvx::Price::fromDouble(entry_price), .equity = lrvx::Volume::fromDouble(equity), .contractMultiplier = lrvx::Quantity::fromDouble(contract_multiplier), .isLongOption = is_long_option}); }, py::arg("account_id"), py::arg("symbol"), py::arg("quantity"), py::arg("entry_price"), py::arg("equity"), py::arg("contract_multiplier") = 1.0, py::arg("is_long_option") = false)
      .def("close_position", &lrvx::LiquidationEngine::closePosition, py::arg("account_id"), py::arg("symbol"))
      .def("on_mark", [](lrvx::LiquidationEngine& self, uint32_t symbol, double mark_price)
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
      .def("on_marks", [](lrvx::LiquidationEngine& self, const std::vector<std::pair<uint32_t, double>>& marks, int64_t ts_ns)
           {
             std::vector<std::pair<lrvx::SymbolId, double>> casted;
             casted.reserve(marks.size());
             for (const auto& [s, p] : marks)
             {
               casted.emplace_back(s, p);
             }
             auto outcome = self.onMarks(casted, ts_ns);
             py::dict d;
             d["liquidated"] = outcome.liquidated;
             d["adl_closed_out"] = outcome.adlClosedOut;
             d["insurance_fund_delta"] = outcome.insuranceFundDelta;
             d["liquidations_count"] = outcome.liquidationsCount;
             d["insurance_payments_count"] = outcome.insurancePaymentsCount;
             d["adl_closeouts_count"] = outcome.adlCloseoutsCount;
             return d; },
           "Atomically update every attached account's mark for each "
           "(symbol, price) pair, then walk liquidations once per symbol. "
           "Returns aggregated outcome.",
           py::arg("marks"), py::arg("ts_ns") = int64_t{0})
      .def("liquidations_count", &lrvx::LiquidationEngine::liquidationsCount)
      .def("insurance_payments_count", &lrvx::LiquidationEngine::insurancePaymentsCount)
      .def("adl_closeouts_count", &lrvx::LiquidationEngine::adlCloseoutsCount)
      .def("position_count", [](const lrvx::LiquidationEngine& self)
           { return self.positions().size(); })
      .def("set_executor", [](lrvx::LiquidationEngine& self, py::object py_exec)
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
      .def("deficits_paid_by_fund", [](const lrvx::LiquidationEngine& self)
           { return self.deficitsPaidByFund(); })
      .def("deficits_paid_by_adl", [](const lrvx::LiquidationEngine& self)
           { return self.deficitsPaidByAdl(); })
      .def("cascade_sizes_per_tick", [](const lrvx::LiquidationEngine& self)
           { return self.cascadeSizesPerTick(); })
      .def("fund_balance_history", [](const lrvx::LiquidationEngine& self)
           { return self.fundBalanceHistory(); })
      .def("ticks_to_first_adl", &lrvx::LiquidationEngine::ticksToFirstAdl)
      .def("reset_stats", &lrvx::LiquidationEngine::resetStats)
      .def("set_mark_impact_model", [](lrvx::LiquidationEngine& self, py::object model, double weight)
           {
             if (py::isinstance<py::str>(model))
             {
               self.setMarkImpactModelByName(model.cast<std::string>(), weight);
             }
             else
             {
               self.setMarkImpactModel(
                   model.cast<lrvx::LiquidationEngine::MarkImpactModel>(), weight);
             } },
           "Configure mark-impact feedback after liquidations. Accepts a "
           "MarkImpactModel enum or a string name (none, book_anchored, "
           "book_only). `weight` is the blend factor used by book_anchored.",
           py::arg("model"), py::arg("weight") = 0.3)
      .def("mark_impact_model", &lrvx::LiquidationEngine::markImpactModel)
      .def("mark_impact_weight", &lrvx::LiquidationEngine::markImpactWeight)
      .def("set_max_cascade_depth", &lrvx::LiquidationEngine::setMaxCascadeDepth, py::arg("depth"))
      .def("max_cascade_depth", &lrvx::LiquidationEngine::maxCascadeDepth)
      .def("attach_account", &lrvx::LiquidationEngine::attachAccount, py::arg("account"), py::keep_alive<1, 2>(),
           "Attach an Account so its positions participate in cross-margin "
           "MM checks. Multiple accounts may be attached.")
      .def("detach_account", &lrvx::LiquidationEngine::detachAccount, py::arg("account_id"))
      .def("account_count", [](const lrvx::LiquidationEngine& self)
           { return self.accounts().size(); })
      .def_static("binance_um_futures", &lrvx::LiquidationEngine::binance_um_futures)
      .def_static("bybit_linear", &lrvx::LiquidationEngine::bybit_linear)
      .def_static("okx_swap", &lrvx::LiquidationEngine::okx_swap);
}

}  // namespace lrvx
