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

#include "flox/backtest/rate_limit_policy.h"

namespace py = pybind11;

namespace flox_py
{

inline void bindRateLimitPolicy(py::module_& m)
{
  // Note: no .export_values() here — `Account` collides with the
  // top-level `flox.Account` class added in T037. Access via
  // `flox.RateLimitEndpointFamily.{Trading,MarketData,Account}`.
  py::enum_<flox::RateLimitPolicy::EndpointFamily>(m, "RateLimitEndpointFamily")
      .value("Trading", flox::RateLimitPolicy::EndpointFamily::Trading)
      .value("MarketData", flox::RateLimitPolicy::EndpointFamily::MarketData)
      .value("Account", flox::RateLimitPolicy::EndpointFamily::Account);
  py::class_<flox::RateLimitPolicy>(m, "RateLimitPolicy")
      .def(py::init<>())
      .def("add_bucket", &flox::RateLimitPolicy::addBucket, py::arg("name"),
           py::arg("window_ns"), py::arg("capacity"), py::arg("submit_weight") = 1,
           py::arg("cancel_weight") = 1, py::arg("replace_weight") = 2,
           py::arg("family") = flox::RateLimitPolicy::EndpointFamily::Trading,
           py::arg("query_weight") = 1)
      .def("add_family_bucket", [](flox::RateLimitPolicy& p, flox::RateLimitPolicy::EndpointFamily family, const std::string& name, int64_t window_ns, uint32_t capacity, uint32_t query_weight)
           { p.addFamilyBucket(family, name, window_ns, capacity, query_weight); }, py::arg("family"), py::arg("name"), py::arg("window_ns"), py::arg("capacity"), py::arg("query_weight") = 1)
      .def("set_ban", &flox::RateLimitPolicy::setBan, py::arg("after_consecutive_rejects"), py::arg("ban_duration_ns"))
      .def("try_consume", [](flox::RateLimitPolicy& p, const std::string& action, int64_t now_ns)
           {
             flox::RateLimitPolicy::ActionKind a =
                 flox::RateLimitPolicy::ActionKind::Submit;
             if (action == "cancel")
             {
               a = flox::RateLimitPolicy::ActionKind::Cancel;
             }
             else if (action == "replace")
             {
               a = flox::RateLimitPolicy::ActionKind::Replace;
             }
             else if (action == "query_account")
             {
               a = flox::RateLimitPolicy::ActionKind::QueryAccount;
             }
             else if (action == "query_market_data")
             {
               a = flox::RateLimitPolicy::ActionKind::QueryMarketData;
             }
             return p.tryConsume(a, now_ns); }, py::arg("action"), py::arg("now_ns"))
      .def("bucket_states", [](flox::RateLimitPolicy& p, int64_t now_ns)
           {
             py::list out;
             for (const auto& s : p.bucketStates(now_ns))
             {
               py::dict d;
               d["name"] = s.name;
               d["endpoint_family"] = s.endpointFamily;
               d["window_ns"] = s.windowNs;
               d["used"] = s.used;
               d["capacity"] = s.capacity;
               out.append(d);
             }
             return out; }, py::arg("now_ns"))
      .def("ban_until_ns", &flox::RateLimitPolicy::banUntilNs)
      .def("consecutive_rejects", &flox::RateLimitPolicy::consecutiveRejects)
      .def("bucket_count", &flox::RateLimitPolicy::bucketCount)
      .def_static("binance_um_futures", &flox::RateLimitPolicy::binance_um_futures)
      .def_static("bybit_linear", &flox::RateLimitPolicy::bybit_linear)
      .def_static("okx_swap", &flox::RateLimitPolicy::okx_swap)
      .def_static("deribit", &flox::RateLimitPolicy::deribit);
}

}  // namespace flox_py
