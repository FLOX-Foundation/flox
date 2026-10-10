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

#include "lrvx/backtest/simulated_clock.h"
#include "lrvx/backtest/venue_stack.h"

namespace py = pybind11;

namespace lrvx
{

inline void bindVenueStack(py::module_& m)
{
  // VenueStack::clock() was bound without its return type ever being
  // registered, so every call raised "Unregistered type" -- a method that
  // could not succeed. Register it: driving the clock is the point of holding
  // a stack from Python (RL envs, paper harnesses), where nothing else
  // advances simulated time.
  py::class_<lrvx::SimulatedClock>(
      m, "SimulatedClock",
      "Simulated time source shared by a VenueStack. Monotonic: advance_to()\n"
      "with a timestamp at or before the current one is a no-op. Use reset()\n"
      "to move backwards.")
      // UnixNanos is a strong type with no implicit integer path -- by design,
      // and pybind rightly refuses to guess. Python keeps seeing plain ints;
      // the domain typing is a C++ compile-time guarantee, not a Python API.
      .def("now_ns", [](const lrvx::SimulatedClock& c)
           { return c.nowNs().raw(); }, "Current simulated time in nanoseconds.")
      .def("advance_to", [](lrvx::SimulatedClock& c, int64_t ns)
           { c.advanceTo(lrvx::UnixNanos::fromRaw(ns)); }, py::arg("ns"), "Advance to `ns`. Ignored if `ns` is not ahead of the current time.")
      .def("reset", [](lrvx::SimulatedClock& c, int64_t ns)
           { c.reset(lrvx::UnixNanos::fromRaw(ns)); }, py::arg("ns") = 0, "Set the clock to `ns`, backwards included.");

  py::class_<lrvx::VenueStack>(m, "VenueStack",
                               "Single-call venue-realistic backtest stack.\n\n"
                               "Use one of the factory methods to obtain a fully-wired "
                               "stack (executor + account + liquidation + fees + funding + "
                               "rate limits + venue availability). The bare "
                               "SimulatedExecutor() constructor remains for unit tests of "
                               "the executor itself; for backtests of real strategies "
                               "always go through a venue factory below.")
      .def_static("binance_um_futures", &lrvx::VenueStack::binance_um_futures,
                  py::arg("account_id"), py::arg("equity"))
      .def_static("bybit_linear", &lrvx::VenueStack::bybit_linear,
                  py::arg("account_id"), py::arg("equity"))
      .def_static("okx_swap", &lrvx::VenueStack::okx_swap, py::arg("account_id"),
                  py::arg("equity"))
      .def_static("deribit", &lrvx::VenueStack::deribit, py::arg("account_id"),
                  py::arg("equity"))
      .def_static("from_venue", &lrvx::VenueStack::fromVenue, py::arg("name"),
                  py::arg("account_id"), py::arg("equity"),
                  "Dispatcher by venue name. Accepts binance_um_futures, "
                  "bybit_linear, okx_swap, deribit (case-insensitive). "
                  "Returns an empty stack on unknown name; check via "
                  "venue_name().")
      // Sub-accessors. py::return_value_policy::reference_internal so
      // each returned subsystem stays tied to the parent VenueStack's
      // lifetime (the stack owns them).
      .def("executor", &lrvx::VenueStack::executor,
           py::return_value_policy::reference_internal)
      .def("account", &lrvx::VenueStack::account,
           py::return_value_policy::reference_internal)
      .def("liquidation", &lrvx::VenueStack::liquidation,
           py::return_value_policy::reference_internal)
      .def("fees", &lrvx::VenueStack::fees,
           py::return_value_policy::reference_internal)
      .def("funding", &lrvx::VenueStack::funding,
           py::return_value_policy::reference_internal)
      .def("venue", &lrvx::VenueStack::venue,
           py::return_value_policy::reference_internal)
      .def("clock", &lrvx::VenueStack::clock,
           py::return_value_policy::reference_internal)
      .def("venue_name", &lrvx::VenueStack::venueName);
}

}  // namespace lrvx
