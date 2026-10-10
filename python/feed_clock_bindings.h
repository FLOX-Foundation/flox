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

#include "lrvx/feed/multi_feed_clock.h"

namespace py = pybind11;

namespace lrvx
{

inline void bindFeedClock(py::module_& m)
{
  py::enum_<lrvx::FeedClockPolicy>(m, "FeedClockPolicy")
      .value("WAIT_FOR_ALL", lrvx::FeedClockPolicy::WaitForAll)
      .value("FIRE_ON_ANY", lrvx::FeedClockPolicy::FireOnAny)
      .value("LEADER_FOLLOWER", lrvx::FeedClockPolicy::LeaderFollower)
      .export_values();

  py::class_<lrvx::MultiFeedClock>(m, "MultiFeedClock")
      .def(py::init([](std::vector<uint32_t> symbols, lrvx::FeedClockPolicy policy,
                       int64_t timeout_ms, uint32_t leader_symbol,
                       int64_t staleness_budget_ms)
                    {
            std::vector<lrvx::SymbolId> sv(symbols.begin(), symbols.end());
            return new lrvx::MultiFeedClock(std::move(sv), policy, timeout_ms,
                                            static_cast<lrvx::SymbolId>(leader_symbol),
                                            staleness_budget_ms); }),
           py::arg("symbols"), py::arg("policy") = lrvx::FeedClockPolicy::WaitForAll,
           py::arg("timeout_ms") = 200, py::arg("leader_symbol") = 0,
           py::arg("staleness_budget_ms") = 200)
      .def("symbol_count", &lrvx::MultiFeedClock::symbolCount)
      .def("policy", &lrvx::MultiFeedClock::policy)
      .def("reset", &lrvx::MultiFeedClock::reset)
      .def("tick", [](lrvx::MultiFeedClock& c, int64_t ts_ns, uint32_t symbol)
           {
             auto snap = c.tick(ts_ns, static_cast<lrvx::SymbolId>(symbol));
             py::dict d;
             d["fired"] = snap.fired;
             d["triggered_by"] = static_cast<uint32_t>(snap.triggeredBy);
             py::dict last;
             py::dict stale;
             for (size_t i = 0; i < snap.symbols.size(); ++i)
             {
               last[py::int_(static_cast<uint32_t>(snap.symbols[i]))] = snap.lastTsNs[i];
               stale[py::int_(static_cast<uint32_t>(snap.symbols[i]))] = snap.stalenessNs[i];
             }
             d["last_ts_ns"] = last;
             d["staleness_ns"] = stale;
             return d; }, py::arg("ts_ns"), py::arg("symbol_id"));
}

}  // namespace lrvx
