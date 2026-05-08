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

#include <optional>
#include <string>

#include "flox/replay/tape_diff.h"

namespace py = pybind11;

namespace flox_py
{

inline py::dict tradeToDict(const flox::replay::TapeDiffTrade& t)
{
  py::dict d;
  d["exchange_ts_ns"] = t.exchange_ts_ns;
  d["symbol_id"] = t.symbol_id;
  d["price_raw"] = t.price_raw;
  d["qty_raw"] = t.qty_raw;
  d["side"] = t.side;
  return d;
}

inline py::object diff_tapes_native(const std::string& left,
                                    const std::string& right,
                                    uint32_t max_mismatches,
                                    int64_t field_tolerance_ns)
{
  flox::replay::TapeDiffOptions opts;
  opts.max_mismatches = max_mismatches;
  opts.field_tolerance_ns = field_tolerance_ns;
  flox::replay::TapeDiffResult r = flox::replay::diffTapes(left, right, opts);

  py::dict out;
  out["left_path"] = r.left_path;
  out["right_path"] = r.right_path;
  out["left_count"] = r.left_count;
  out["right_count"] = r.right_count;
  if (r.first_divergence_index.has_value())
  {
    out["first_divergence_index"] = *r.first_divergence_index;
  }
  else
  {
    out["first_divergence_index"] = py::none();
  }
  out["equal"] = r.equal;

  py::list mismatches;
  for (const auto& m : r.mismatches)
  {
    py::dict entry;
    entry["index"] = m.index;
    entry["left"] = tradeToDict(m.left);
    entry["right"] = tradeToDict(m.right);
    mismatches.append(entry);
  }
  out["mismatches"] = mismatches;
  return out;
}

inline void bindTapeDiff(py::module_& m)
{
  m.def("_tape_diff_native", &diff_tapes_native,
        py::arg("left"),
        py::arg("right"),
        py::arg("max_mismatches") = 16,
        py::arg("field_tolerance_ns") = 0,
        "Internal C++-backed tape diff. Returns a dict the public "
        "wrapper in flox_py.tape converts to a TapeDiff dataclass.");
}

}  // namespace flox_py
