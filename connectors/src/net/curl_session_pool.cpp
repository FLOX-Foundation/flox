/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox-connectors/net/curl_session_pool.h"

#include <stdexcept>

namespace flox
{

CurlSessionPool::CurlSessionPool(std::size_t size, std::size_t maxSize)
    : _maxSize(maxSize), _acquireTimeoutMs(5000)
{
  curl_global_init(CURL_GLOBAL_ALL);
  _pool.reserve(size);
  for (std::size_t i = 0; i < size; ++i)
  {
    CURL* h = curl_easy_init();
    if (!h)
    {
      throw std::runtime_error("curl_easy_init failed");
    }
    _pool.push_back(h);
    ++_totalCreated;
  }
}

CurlSessionPool::CurlSessionPool(CurlSessionPoolConfig config)
    : _maxSize(config.maxSize), _acquireTimeoutMs(config.acquireTimeoutMs)
{
  if (!config.isValid())
  {
    throw std::invalid_argument("Invalid CurlSessionPoolConfig");
  }

  curl_global_init(CURL_GLOBAL_ALL);
  _pool.reserve(config.initialSize);
  for (std::size_t i = 0; i < config.initialSize; ++i)
  {
    CURL* h = curl_easy_init();
    if (!h)
    {
      throw std::runtime_error("curl_easy_init failed");
    }
    _pool.push_back(h);
    ++_totalCreated;
  }
}

CurlSessionPool::~CurlSessionPool()
{
  std::lock_guard lock(_mutex);
  for (auto* h : _pool)
  {
    curl_easy_cleanup(h);
  }
  curl_global_cleanup();
}

CURL* CurlSessionPool::acquire()
{
  std::unique_lock lock(_mutex);

  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(_acquireTimeoutMs);

  while (_pool.empty() && _totalCreated >= _maxSize)
  {
    if (_cv.wait_until(lock, deadline) == std::cv_status::timeout)
    {
      return nullptr;  // Timeout waiting for handle
    }
  }

  if (!_pool.empty())
  {
    CURL* h = _pool.back();
    _pool.pop_back();
    return h;
  }

  if (_totalCreated < _maxSize)
  {
    CURL* h = curl_easy_init();
    if (h)
    {
      ++_totalCreated;
    }
    return h;
  }

  return nullptr;
}

void CurlSessionPool::release(CURL* h)
{
  if (!h)
  {
    return;
  }

  {
    std::lock_guard lock(_mutex);
    if (_pool.size() < _maxSize)
    {
      _pool.push_back(h);
    }
    else
    {
      curl_easy_cleanup(h);
      --_totalCreated;
      return;  // Don't notify if we cleaned up
    }
  }

  _cv.notify_one();
}

std::size_t CurlSessionPool::available() const
{
  std::lock_guard lock(_mutex);
  return _pool.size();
}

std::size_t CurlSessionPool::totalCreated() const
{
  std::lock_guard lock(_mutex);
  return _totalCreated;
}

}  // namespace flox
