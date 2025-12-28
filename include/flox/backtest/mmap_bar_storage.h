/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/aggregator/bar.h"
#include "flox/aggregator/timeframe.h"
#include "flox/common.h"

#include <filesystem>
#include <map>
#include <span>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace flox
{

class MmapBarStorage
{
 public:
  explicit MmapBarStorage(const std::filesystem::path& symbolDir);
  ~MmapBarStorage();

  MmapBarStorage(const MmapBarStorage&) = delete;
  MmapBarStorage& operator=(const MmapBarStorage&) = delete;
  MmapBarStorage(MmapBarStorage&&) noexcept;
  MmapBarStorage& operator=(MmapBarStorage&&) noexcept;

  const Bar* getBar(TimeframeId tf, size_t index) const;
  const Bar* findBar(TimeframeId tf, TimePoint time, char mode = 'e') const;
  std::span<const Bar> getBars(TimeframeId tf) const;
  size_t barCount(TimeframeId tf) const;
  size_t totalBars() const;
  std::pair<TimePoint, TimePoint> timeRange() const;
  std::vector<TimeframeId> timeframes() const;

 private:
  struct MmapFile
  {
#ifdef _WIN32
    HANDLE hFile{INVALID_HANDLE_VALUE};
    HANDLE hMapping{nullptr};
#else
    int fd{-1};
#endif
    void* addr{nullptr};
    size_t size{0};
    size_t barCount{0};
    TimeframeId timeframe;
  };

  void openTimeframe(const std::filesystem::path& path, TimeframeId tf);
  void closeAll();

  std::filesystem::path _symbolDir;
  std::map<TimeframeId, MmapFile> _files;
};

}  // namespace flox
