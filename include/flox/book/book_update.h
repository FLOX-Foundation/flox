/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <chrono>
#include <memory_resource>
#include <vector>
#include "flox/common.h"

namespace flox
{

enum class BookUpdateType
{
  SNAPSHOT,
  DELTA
};

struct BookLevel
{
  Price price{};
  Quantity quantity{};
  BookLevel() = default;
  BookLevel(Price p, Quantity q) : price(p), quantity(q) {}
};

struct BookUpdate
{
  SymbolId symbol{};
  BookUpdateType type{};
  std::pmr::vector<BookLevel> bids;
  std::pmr::vector<BookLevel> asks;
  std::chrono::steady_clock::time_point timestamp{};

  BookUpdate(std::pmr::memory_resource* res) : bids(res), asks(res) {}
};

}  // namespace flox
