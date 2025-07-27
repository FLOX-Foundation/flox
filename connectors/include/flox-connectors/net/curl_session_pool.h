#pragma once

#include <curl/curl.h>

#include <mutex>
#include <vector>

namespace flox
{

class CurlSessionPool
{
 public:
  explicit CurlSessionPool(std::size_t size = 4);
  ~CurlSessionPool();

  CURL* acquire();
  void release(CURL* h);

 private:
  std::vector<CURL*> _pool;
  std::mutex _mutex;
};

}  // namespace flox
