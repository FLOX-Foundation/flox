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
  py::class_<flox::RateLimitPolicy>(m, "RateLimitPolicy")
      .def(py::init<>())
      .def("add_bucket", &flox::RateLimitPolicy::addBucket, py::arg("name"),
           py::arg("window_ns"), py::arg("capacity"), py::arg("submit_weight") = 1,
           py::arg("cancel_weight") = 1, py::arg("replace_weight") = 2)
      .def("set_ban", &flox::RateLimitPolicy::setBan,
           py::arg("after_consecutive_rejects"), py::arg("ban_duration_ns"))
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
             return p.tryConsume(a, now_ns); }, py::arg("action"), py::arg("now_ns"))
      .def("bucket_states", [](flox::RateLimitPolicy& p, int64_t now_ns)
           {
             py::list out;
             for (const auto& s : p.bucketStates(now_ns))
             {
               py::dict d;
               d["name"] = s.name;
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
