/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox-connectors/bybit/authenticated_rest_client.h"

#include <openssl/hmac.h>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace flox
{

AuthenticatedRestClient::AuthenticatedRestClient(std::string apiKey,
                                                 std::string apiSecret,
                                                 std::string endpoint,
                                                 ITransport* transport)
    : _apiKey(std::move(apiKey)),
      _apiSecret(std::move(apiSecret)),
      _endpoint(std::move(endpoint)),
      _transport(transport) {}

void AuthenticatedRestClient::post(std::string_view path,
                                   std::string_view body,
                                   std::move_only_function<void(std::string_view)> onSuccess,
                                   std::move_only_function<void(std::string_view)> onError)
{
  using namespace std::chrono;

  const auto ts = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
  char tsBuf[32];
  const auto tsLen = std::snprintf(tsBuf, sizeof(tsBuf), "%lld", static_cast<long long>(ts));
  const std::string_view tsStr{tsBuf, static_cast<std::size_t>(tsLen)};
  constexpr std::string_view recv{"10000"};

  std::string toSign;
  toSign.reserve(tsStr.size() + _apiKey.size() + recv.size() + body.size());
  toSign.append(tsStr).append(_apiKey).append(recv);
  toSign.append(body);

  unsigned char hash[EVP_MAX_MD_SIZE];
  unsigned int hashLen = 0;
  HMAC(EVP_sha256(),
       _apiSecret.data(), static_cast<int>(_apiSecret.size()),
       reinterpret_cast<const unsigned char*>(toSign.data()), toSign.size(),
       hash, &hashLen);

  char hex[EVP_MAX_MD_SIZE * 2 + 1];
  for (unsigned i = 0; i < hashLen; ++i)
  {
    std::sprintf(hex + i * 2, "%02x", hash[i]);
  }
  hex[hashLen * 2] = '\0';

  std::vector<std::pair<std::string_view, std::string_view>> headers;
  headers.reserve(6);
  headers.emplace_back("Content-Type", "application/json");
  headers.emplace_back("X-BAPI-API-KEY", _apiKey);
  headers.emplace_back("X-BAPI-SIGN", std::string_view{hex, hashLen * 2});
  headers.emplace_back("X-BAPI-SIGN-TYPE", "2");
  headers.emplace_back("X-BAPI-TIMESTAMP", tsStr);
  headers.emplace_back("X-BAPI-RECV-WINDOW", recv);

  std::string url = _endpoint;
  url.append(path);

  _transport->post(url, body, headers, std::move(onSuccess), std::move(onError));
}

}  // namespace flox
