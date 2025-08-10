/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox-connectors/net/curl_transport.h"

#include <string>
#include <string_view>
#include <vector>

namespace flox
{

namespace
{
size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
  auto* str = static_cast<std::string*>(userdata);
  str->append(ptr, size * nmemb);
  return size * nmemb;
}
}  // namespace

CurlTransport::CurlTransport(std::size_t poolSize) : _pool(poolSize) {}

CurlTransport::~CurlTransport() = default;

void CurlTransport::post(std::string_view url, std::string_view body,
                         const std::vector<std::pair<std::string_view, std::string_view>>& headers,
                         std::move_only_function<void(std::string_view)> onSuccess,
                         std::move_only_function<void(std::string_view)> onError)
{
  CURL* h = _pool.acquire();
  curl_easy_reset(h);

  curl_easy_setopt(h, CURLOPT_URL, std::string(url).c_str());
  curl_easy_setopt(h, CURLOPT_POST, 1L);
  curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.data());
  curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, body.size());

  curl_easy_setopt(h, CURLOPT_FORBID_REUSE, 0L);
  curl_easy_setopt(h, CURLOPT_FRESH_CONNECT, 0L);
  curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(h, CURLOPT_TCP_KEEPIDLE, 30L);
  curl_easy_setopt(h, CURLOPT_TCP_KEEPINTVL, 15L);

  curl_easy_setopt(h, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);

  std::string response;
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, writeCallback);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &response);

  struct curl_slist* hdrs = nullptr;
  hdrs = curl_slist_append(hdrs, "Connection: keep-alive");
  for (const auto& [k, v] : headers)
  {
    std::string hv;
    hv.reserve(k.size() + v.size() + 3);
    hv.append(k);
    hv.append(": ");
    hv.append(v);
    hdrs = curl_slist_append(hdrs, hv.c_str());
  }
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);

  CURLcode res = curl_easy_perform(h);

  curl_slist_free_all(hdrs);
  _pool.release(h);

  if (res == CURLE_OK)
  {
    if (onSuccess)
    {
      onSuccess(response);
    }
  }
  else
  {
    if (onError)
    {
      onError(curl_easy_strerror(res));
    }
  }
}

}  // namespace flox
