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

#include "flox/feed/multi_feed_clock.h"

namespace py = pybind11;

namespace flox_py
{

inline void bindFeedClock(py::module_& m)
{
  py::enum_<flox::FeedClockPolicy>(m, "FeedClockPolicy")
      .value("WAIT_FOR_ALL", flox::FeedClockPolicy::WaitForAll)
      .value("FIRE_ON_ANY", flox::FeedClockPolicy::FireOnAny)
      .value("LEADER_FOLLOWER", flox::FeedClockPolicy::LeaderFollower)
      .export_values();

  py::class_<flox::MultiFeedClock>(m, "MultiFeedClock")
      .def(py::init([](std::vector<uint32_t> symbols, flox::FeedClockPolicy policy,
                       int64_t timeout_ms, uint32_t leader_symbol,
                       int64_t staleness_budget_ms)
                    {
            std::vector<flox::SymbolId> sv(symbols.begin(), symbols.end());
            return new flox::MultiFeedClock(std::move(sv), policy, timeout_ms,
                                            static_cast<flox::SymbolId>(leader_symbol),
                                            staleness_budget_ms); }),
           py::arg("symbols"), py::arg("policy") = flox::FeedClockPolicy::WaitForAll,
           py::arg("timeout_ms") = 200, py::arg("leader_symbol") = 0,
           py::arg("staleness_budget_ms") = 200)
      .def("symbol_count", &flox::MultiFeedClock::symbolCount)
      .def("policy", &flox::MultiFeedClock::policy)
      .def("reset", &flox::MultiFeedClock::reset)
      .def("tick", [](flox::MultiFeedClock& c, int64_t ts_ns, uint32_t symbol)
           {
             auto snap = c.tick(ts_ns, static_cast<flox::SymbolId>(symbol));
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

}  // namespace flox_py
