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

#include "flox/backtest/venue_availability.h"

namespace py = pybind11;

namespace flox_py
{

inline flox::OnOutage parseOnOutage(const std::string& s)
{
  if (s == "hold")
  {
    return flox::OnOutage::HOLD;
  }
  if (s == "expire_gtc_after")
  {
    return flox::OnOutage::EXPIRE_GTC_AFTER;
  }
  return flox::OnOutage::CANCEL_ALL;
}

inline flox::OutageType parseOutageType(const std::string& s)
{
  if (s == "submit_only_down" || s == "submit_only")
  {
    return flox::OutageType::SubmitOnlyDown;
  }
  if (s == "cancel_only_down" || s == "cancel_only")
  {
    return flox::OutageType::CancelOnlyDown;
  }
  if (s == "slow_degradation")
  {
    return flox::OutageType::SlowDegradation;
  }
  if (s == "stale_book")
  {
    return flox::OutageType::StaleBook;
  }
  if (s == "wrong_side_recovery")
  {
    return flox::OutageType::WrongSideRecovery;
  }
  return flox::OutageType::Total;
}

inline void bindVenueAvailability(py::module_& m)
{
  py::class_<flox::VenueAvailability>(m, "VenueAvailability")
      .def(py::init<>())
      .def(
          "schedule_outage",
          [](flox::VenueAvailability& self, int64_t start_ns, int64_t duration_ns,
             const std::string& policy, int64_t gtc_ttl_ns)
          {
            self.scheduleOutage(start_ns, duration_ns, parseOnOutage(policy), gtc_ttl_ns);
            return &self;
          },
          py::arg("start_ns"), py::arg("duration_ns"),
          py::arg("on_open_orders") = std::string("cancel_all"),
          py::arg("gtc_ttl_ns") = 0,
          py::return_value_policy::reference_internal)
      .def(
          "auto_random_outages",
          [](flox::VenueAvailability& self, double per_day, int64_t mean_duration_ns,
             const std::string& policy, uint64_t seed)
          {
            self.autoRandomOutages(per_day, mean_duration_ns, parseOnOutage(policy), seed);
            return &self;
          },
          py::arg("per_day"), py::arg("mean_duration_ns"),
          py::arg("on_open_orders") = std::string("cancel_all"),
          py::arg("seed") = 0xC0FFEEULL,
          py::return_value_policy::reference_internal)
      .def(
          "schedule_outage_ex",
          [](flox::VenueAvailability& self, int64_t start_ns, int64_t duration_ns,
             const std::string& outage_type, const std::string& policy,
             int64_t gtc_ttl_ns, double degradation_latency_multiplier,
             double wrong_side_recovery_bps)
          {
            self.scheduleOutageEx(start_ns, duration_ns, parseOutageType(outage_type),
                                  parseOnOutage(policy), gtc_ttl_ns,
                                  degradation_latency_multiplier,
                                  wrong_side_recovery_bps);
            return &self;
          },
          py::arg("start_ns"), py::arg("duration_ns"),
          py::arg("outage_type") = std::string("total"),
          py::arg("on_open_orders") = std::string("cancel_all"),
          py::arg("gtc_ttl_ns") = 0,
          py::arg("degradation_latency_multiplier") = 1.0,
          py::arg("wrong_side_recovery_bps") = 0.0,
          py::return_value_policy::reference_internal)
      .def("is_up", &flox::VenueAvailability::isUp, py::arg("now_ns"))
      .def("submits_allowed", &flox::VenueAvailability::submitsAllowed,
           py::arg("now_ns"))
      .def("cancels_allowed", &flox::VenueAvailability::cancelsAllowed,
           py::arg("now_ns"))
      .def("book_updates_allowed", &flox::VenueAvailability::bookUpdatesAllowed,
           py::arg("now_ns"))
      .def("trades_allowed", &flox::VenueAvailability::tradesAllowed,
           py::arg("now_ns"))
      .def("latency_multiplier", &flox::VenueAvailability::latencyMultiplier,
           py::arg("now_ns"))
      .def("consume_wrong_side_recovery_bps",
           &flox::VenueAvailability::consumeWrongSideRecoveryBps);
}

}  // namespace flox_py
