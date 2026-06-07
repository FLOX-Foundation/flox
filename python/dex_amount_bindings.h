/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include <pybind11/pybind11.h>

#include "flox/util/int/i256.h"
#include "flox/util/int/u256.h"

#include <string>

namespace flox_py
{

namespace py = pybind11;

// The DEX curves compute in exact 256-bit integers (u256, and i256 for signed
// values). At the Python boundary they are plain `int` -- arbitrary precision, so a
// 256-bit wei amount is lossless. These helpers are the boundary: a Python int is
// formatted to a decimal string, parsed by the exact C++ type, and returned as a
// Python int, which is also how the bound curves (next) accept and return amounts.
inline py::int_ intFromDecimal(const std::string& dec)
{
  return py::reinterpret_steal<py::int_>(PyLong_FromString(dec.c_str(), nullptr, 10));
}

inline std::string decimalFromInt(const py::int_& value)
{
  return py::cast<std::string>(py::str(value));
}

inline void bindDexAmount(py::module_& m)
{
  // Round-trip a Python int through the exact u256, proving the boundary is lossless
  // for any value up to 2^256 - 1.
  m.def("u256_roundtrip", [](const py::int_& value) -> py::int_
        { return intFromDecimal(flox::u256::fromDec(decimalFromInt(value)).toDec()); }, py::arg("value"));

  // The signed variant (i256), carrying the sign.
  m.def("i256_roundtrip", [](const py::int_& value) -> py::int_
        {
          const flox::i256 v = flox::i256::fromDec(decimalFromInt(value));
          const std::string mag = v.magnitude().toDec();
          return intFromDecimal((v.neg && !v.magnitude().isZero()) ? "-" + mag : mag); }, py::arg("value"));

  // Hex (with or without a 0x prefix) -> int, for chain data that arrives in hex.
  m.def("u256_from_hex", [](const std::string& hex) -> py::int_
        { return intFromDecimal(flox::u256::fromHex(hex).toDec()); }, py::arg("hex"));
}

}  // namespace flox_py
