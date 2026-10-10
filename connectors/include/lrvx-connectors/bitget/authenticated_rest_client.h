/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <lrvx/net/abstract_transport.h>
#include <lrvx/util/base/move_only_function.h>

#include <string>
#include <string_view>

namespace lrvx
{

class BitgetAuthenticatedRestClient
{
 public:
  BitgetAuthenticatedRestClient(std::string apiKey, std::string apiSecret, std::string passphrase,
                                std::string endpoint, ITransport* transport);

  void post(std::string_view path, std::string_view body,
            MoveOnlyFunction<void(std::string_view)> onSuccess,
            MoveOnlyFunction<void(std::string_view)> onError);

 private:
  std::string _apiKey;
  std::string _apiSecret;
  std::string _passphrase;
  std::string _endpoint;
  ITransport* _transport;
};

}  // namespace lrvx
