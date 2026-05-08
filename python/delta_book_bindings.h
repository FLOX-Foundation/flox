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

#include <utility>
#include <vector>

#include "flox/replay/delta_book.h"

namespace py = pybind11;

namespace flox_py
{

inline py::list levelsToList(const std::vector<flox::replay::BookLevel>& levels)
{
  py::list out;
  for (const auto& l : levels)
  {
    py::dict d;
    d["price_raw"] = l.price_raw;
    d["qty_raw"] = l.qty_raw;
    out.append(d);
  }
  return out;
}

inline std::vector<flox::replay::BookLevel> listToLevels(const py::list& levels)
{
  std::vector<flox::replay::BookLevel> out;
  out.reserve(levels.size());
  for (const auto& obj : levels)
  {
    auto d = obj.cast<py::dict>();
    flox::replay::BookLevel l{};
    l.price_raw = d["price_raw"].cast<int64_t>();
    l.qty_raw = d["qty_raw"].cast<int64_t>();
    out.push_back(l);
  }
  return out;
}

inline void bindDeltaBook(py::module_& m)
{
  using flox::replay::DeltaBookEncoder;
  using flox::replay::DeltaBookReplayer;

  py::class_<DeltaBookEncoder>(m, "DeltaBookEncoder",
                               "Encode a stream of L2 snapshots as anchor snapshots plus deltas. "
                               "anchor_every=0 emits snapshots only; the existing tape format "
                               "stores either type, so callers can safely persist the output.")
      .def(py::init<uint32_t>(), py::arg("anchor_every") = 100)
      .def(
          "encode",
          [](DeltaBookEncoder& self, uint32_t symbol_id,
             const py::list& bids, const py::list& asks)
          {
            auto out = self.encode(symbol_id, listToLevels(bids), listToLevels(asks));
            py::dict result;
            result["is_delta"] = out.is_delta;
            result["bids"] = levelsToList(out.bids);
            result["asks"] = levelsToList(out.asks);
            return result;
          },
          py::arg("symbol_id"), py::arg("bids"), py::arg("asks"),
          "Feed the encoder a full snapshot. Returns a dict with "
          "is_delta / bids / asks. The bids and asks lists carry "
          "BookLevel-shaped dicts with price_raw + qty_raw.")
      .def("reset", &DeltaBookEncoder::reset, py::arg("symbol_id"))
      .def("reset_all", &DeltaBookEncoder::resetAll);

  py::class_<DeltaBookReplayer>(m, "DeltaBookReplayer",
                                "Reverse of DeltaBookEncoder. Maintains current book state per "
                                "symbol; given a stream of (type, bids, asks) tuples returns "
                                "the reconstructed full snapshot at each step.")
      .def(py::init<>())
      .def(
          "apply",
          [](DeltaBookReplayer& self, uint8_t type, uint32_t symbol_id,
             const py::list& bids, const py::list& asks)
          {
            auto snap = self.apply(type, symbol_id, listToLevels(bids), listToLevels(asks));
            py::dict result;
            result["bids"] = levelsToList(snap.bids);
            result["asks"] = levelsToList(snap.asks);
            return result;
          },
          py::arg("type"), py::arg("symbol_id"),
          py::arg("bids"), py::arg("asks"),
          "Apply one event. type=0 snapshot, type=1 delta. Returns "
          "the reconstructed full snapshot for the symbol.")
      .def("reset", &DeltaBookReplayer::reset, py::arg("symbol_id"))
      .def("reset_all", &DeltaBookReplayer::resetAll);
}

}  // namespace flox_py
