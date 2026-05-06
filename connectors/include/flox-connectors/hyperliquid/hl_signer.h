/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <optional>
#include <string>

namespace flox::hl
{

struct HlSig
{
  std::string r, s;
  int v;
};

struct HlSignParams
{
  std::string actionJson;
  long long nonceMs;
  std::string privateKeyHex;
  bool isMainnet{true};

  std::optional<std::string> activePoolJson;
  std::optional<long long> expiresAfterMs;
};

std::optional<HlSig> hl_sign_with_sdk(const HlSignParams& p);

}  // namespace flox::hl
