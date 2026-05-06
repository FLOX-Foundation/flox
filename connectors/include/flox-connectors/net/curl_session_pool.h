#pragma once

#include <curl/curl.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <vector>

namespace flox
{

struct CurlSessionPoolConfig
{
  std::size_t initialSize{4};
  std::size_t maxSize{32};
  int acquireTimeoutMs{5000};  // 5s timeout for waiting

  bool isValid() const { return initialSize > 0 && maxSize >= initialSize && acquireTimeoutMs > 0; }
};

class CurlSessionPool
{
 public:
  explicit CurlSessionPool(std::size_t size = 4, std::size_t maxSize = 32);
  explicit CurlSessionPool(CurlSessionPoolConfig config);
  ~CurlSessionPool();

  CurlSessionPool(const CurlSessionPool&) = delete;
  CurlSessionPool& operator=(const CurlSessionPool&) = delete;

  /// Acquire a CURL handle. Blocks up to acquireTimeoutMs if pool is exhausted.
  /// Returns nullptr on timeout.
  CURL* acquire();

  /// Release a CURL handle back to the pool.
  void release(CURL* h);

  /// Returns current number of handles in pool (not acquired)
  std::size_t available() const;

  /// Returns total handles created
  std::size_t totalCreated() const;

 private:
  std::vector<CURL*> _pool;
  mutable std::mutex _mutex;
  std::condition_variable _cv;
  std::size_t _maxSize;
  std::size_t _totalCreated{0};
  int _acquireTimeoutMs{5000};
};

}  // namespace flox
