/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include "lrvx/common.h"

#include <array>
#include <cstring>
#include <string_view>

namespace lrvx
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

}  // namespace lrvx
