#pragma once

#include <curl/curl.h>

#include <cstddef>
#include <mutex>
#include <vector>

namespace flox
{

class CurlSessionPool
{
 public:
  explicit CurlSessionPool(std::size_t size = 4, std::size_t maxSize = 32);
  ~CurlSessionPool();

  CurlSessionPool(const CurlSessionPool&) = delete;
  CurlSessionPool& operator=(const CurlSessionPool&) = delete;

  CURL* acquire();
  void release(CURL* h);

 private:
  std::vector<CURL*> _pool;
  std::mutex _mutex;
  std::size_t _maxSize;
  std::size_t _totalCreated{0};
};

}  // namespace flox
