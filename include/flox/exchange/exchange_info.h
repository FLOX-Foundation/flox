/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include "flox/common.h"

#include <array>
#include <cstring>
#include <string_view>

namespace flox
{

struct ExchangeInfo
{
  static constexpr size_t kMaxNameLength = 16;

  std::array<char, kMaxNameLength> name{};
  VenueType type{VenueType::CentralizedExchange};

  std::string_view nameView() const
  {
    return {name.data(), strnlen(name.data(), kMaxNameLength)};
  }

  void setName(std::string_view n)
  {
    size_t len = std::min(n.size(), kMaxNameLength - 1);
    std::memcpy(name.data(), n.data(), len);
    name[len] = '\0';
  }
};

}  // namespace flox
