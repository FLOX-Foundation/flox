/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <memory_resource>
#include <vector>

#include "flox/common.h"
#include "flox/util/base/time.h"

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
  InstrumentType instrument = InstrumentType::Spot;
  BookUpdateType type{};
  std::pmr::vector<BookLevel> bids;
  std::pmr::vector<BookLevel> asks;

  UnixNanos exchangeTsNs{0};
  UnixNanos systemTsNs{0};

  std::optional<Price> strike;
  std::optional<TimePoint> expiry;
  std::optional<OptionType> optionType;

  BookUpdate(std::pmr::memory_resource* res) : bids(res), asks(res) {}
};

}  // namespace flox
