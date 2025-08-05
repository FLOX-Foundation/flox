/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox-connectors/bitget/authenticated_rest_client.h"

#include <openssl/hmac.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace flox
{

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const unsigned char* data, size_t len)
{
  std::string out((len + 2) / 3 * 4, '=');
  size_t out_idx = 0;
  for (size_t i = 0; i < len; i += 3)
  {
    int val =
        (data[i] << 16) + ((i + 1 < len ? data[i + 1] : 0) << 8) + (i + 2 < len ? data[i + 2] : 0);
    out[out_idx++] = b64_table[(val >> 18) & 0x3F];
    out[out_idx++] = b64_table[(val >> 12) & 0x3F];
    out[out_idx++] = b64_table[(val >> 6) & 0x3F];
    out[out_idx++] = b64_table[val & 0x3F];
  }

  // padding correction
  size_t mod = len % 3;
  if (mod)
  {
    out[out.size() - (3 - mod)] = '=';
  }
  if (mod == 1)
  {
    out[out.size() - 2] = '=';
  }
  return out;
}

BitgetAuthenticatedRestClient::BitgetAuthenticatedRestClient(std::string apiKey,
                                                             std::string apiSecret,
                                                             std::string passphrase,
                                                             std::string endpoint,
                                                             ITransport* transport)
    : _apiKey(std::move(apiKey)),
      _apiSecret(std::move(apiSecret)),
      _passphrase(std::move(passphrase)),
      _endpoint(std::move(endpoint)),
      _transport(transport)
{
}

void BitgetAuthenticatedRestClient::post(std::string_view path, std::string_view body,
                                         std::move_only_function<void(std::string_view)> onSuccess,
                                         std::move_only_function<void(std::string_view)> onError)
{
  using namespace std::chrono;

  const auto ts = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
  char tsBuf[32];
  const auto tsLen = std::snprintf(tsBuf, sizeof(tsBuf), "%lld", static_cast<long long>(ts));
  const std::string_view tsStr{tsBuf, static_cast<std::size_t>(tsLen)};

  std::string toSign;
  toSign.reserve(tsStr.size() + 4 + path.size() + body.size());
  toSign.append(tsStr).append("POST").append(path);
  toSign.append(body);

  unsigned char hash[EVP_MAX_MD_SIZE];
  unsigned int hashLen = 0;
  HMAC(EVP_sha256(), _apiSecret.data(), static_cast<int>(_apiSecret.size()),
       reinterpret_cast<const unsigned char*>(toSign.data()), toSign.size(), hash, &hashLen);

  std::string sign = base64_encode(hash, hashLen);

  std::vector<std::pair<std::string_view, std::string_view>> headers;
  headers.reserve(5);
  headers.emplace_back("Content-Type", "application/json");
  headers.emplace_back("ACCESS-KEY", _apiKey);
  headers.emplace_back("ACCESS-SIGN", sign);
  headers.emplace_back("ACCESS-TIMESTAMP", tsStr);
  headers.emplace_back("ACCESS-PASSPHRASE", _passphrase);

  std::string url = _endpoint;
  url.append(path);

  _transport->post(url, body, headers, std::move(onSuccess), std::move(onError));
}

}  // namespace flox
