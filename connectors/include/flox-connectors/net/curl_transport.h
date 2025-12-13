/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox-connectors/net/curl_session_pool.h"

#include <flox/net/abstract_transport.h>
#include <flox/util/base/move_only_function.h>

#include <string_view>
#include <vector>

namespace flox
{

class CurlTransport : public ITransport
{
 public:
  explicit CurlTransport(std::size_t poolSize = 4);
  ~CurlTransport() override;

  void post(std::string_view url, std::string_view body,
            const std::vector<std::pair<std::string_view, std::string_view>>& headers,
            MoveOnlyFunction<void(std::string_view)> onSuccess,
            MoveOnlyFunction<void(std::string_view)> onError) override;

 private:
  CurlSessionPool _pool;
};

}  // namespace flox
