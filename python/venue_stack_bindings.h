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

#include "flox/backtest/venue_stack.h"

namespace py = pybind11;

namespace flox_py
{

inline void bindVenueStack(py::module_& m)
{
  py::class_<flox::VenueStack>(m, "VenueStack",
                               "Single-call venue-realistic backtest stack.\n\n"
                               "Use one of the factory methods to obtain a fully-wired "
                               "stack (executor + account + liquidation + fees + funding + "
                               "rate limits + venue availability). The bare "
                               "SimulatedExecutor() constructor remains for unit tests of "
                               "the executor itself; for backtests of real strategies "
                               "always go through a venue factory below.")
      .def_static("binance_um_futures", &flox::VenueStack::binance_um_futures,
                  py::arg("account_id"), py::arg("equity"))
      .def_static("bybit_linear", &flox::VenueStack::bybit_linear,
                  py::arg("account_id"), py::arg("equity"))
      .def_static("okx_swap", &flox::VenueStack::okx_swap, py::arg("account_id"),
                  py::arg("equity"))
      .def_static("deribit", &flox::VenueStack::deribit, py::arg("account_id"),
                  py::arg("equity"))
      .def_static("from_venue", &flox::VenueStack::fromVenue, py::arg("name"),
                  py::arg("account_id"), py::arg("equity"),
                  "Dispatcher by venue name. Accepts binance_um_futures, "
                  "bybit_linear, okx_swap, deribit (case-insensitive). "
                  "Returns an empty stack on unknown name; check via "
                  "venue_name().")
      // Sub-accessors. py::return_value_policy::reference_internal so
      // each returned subsystem stays tied to the parent VenueStack's
      // lifetime (the stack owns them).
      .def("executor", &flox::VenueStack::executor,
           py::return_value_policy::reference_internal)
      .def("account", &flox::VenueStack::account,
           py::return_value_policy::reference_internal)
      .def("liquidation", &flox::VenueStack::liquidation,
           py::return_value_policy::reference_internal)
      .def("fees", &flox::VenueStack::fees,
           py::return_value_policy::reference_internal)
      .def("funding", &flox::VenueStack::funding,
           py::return_value_policy::reference_internal)
      .def("venue", &flox::VenueStack::venue,
           py::return_value_policy::reference_internal)
      .def("clock", &flox::VenueStack::clock,
           py::return_value_policy::reference_internal)
      .def("venue_name", &flox::VenueStack::venueName);
}

}  // namespace flox_py
