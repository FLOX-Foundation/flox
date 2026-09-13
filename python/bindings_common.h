/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// python/bindings_common.h
//
// Small helpers shared across the pybind11 binding headers, pulled out
// of indicator_bindings.h so aggregator_bindings.h, optimizer_bindings.h,
// profile_bindings.h, book_bindings.h, and composite_book_bindings.h can
// use them too without redefining the same function in flox_py.cpp's
// translation unit (every one of these headers opens an anonymous
// namespace; within one .cpp file those are all the SAME namespace, so
// two headers each defining `checkSameSize` would collide).
//
// The pattern below fixes one recurring defect class found across the
// binding layer: a numpy array argument declared as a bare
// `py::array_t<T>` accepts a non-contiguous view (a `[::2]` slice, a
// structured-array field, a 2D column) without copying it, and code
// that then walks the buffer with a raw pointer and a stride-1
// assumption reads the wrong elements -- or, for a reversed slice,
// reads off the front of the allocation entirely. `py::array_t<T,
// py::array::c_style | py::array::forcecast>` on the parameter type
// fixes that at the pybind11 layer by materialising a contiguous copy
// whenever the caller's array is not already one; checkSameSize below
// closes the companion defect, where a function reads its element
// count from a single one of several same-length input arrays and
// never checks that the others actually match.

#pragma once

#include "flox/error/flox_error.h"

#include <cstddef>

// Deliberately an unnamed namespace, not `namespace flox_py`, to match
// how the binding headers that need this already declare their other
// file-local helpers (see aggregator_bindings.h, indicator_bindings.h).
// An unnamed namespace is unique per translation unit, not per header,
// so as long as every header that wants `checkSameSize` includes this
// one -- and `#pragma once` keeps it a single definition even though
// several headers include it -- plain unqualified `checkSameSize(...)`
// resolves correctly from any of them inside flox_py.cpp, the one .cpp
// file that pulls all of these headers together.
namespace
{

inline void checkSameSize(size_t a, size_t b, const char* msg)
{
  if (a != b)
  {
    throw flox::FloxError("E_LEN_001", msg);
  }
}

}  // namespace
