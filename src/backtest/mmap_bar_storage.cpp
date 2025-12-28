/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/mmap_bar_storage.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace flox
{

MmapBarStorage::MmapBarStorage(const std::filesystem::path& symbolDir) : _symbolDir(symbolDir)
{
  if (!std::filesystem::exists(symbolDir))
  {
    throw std::runtime_error("Symbol directory not found: " + symbolDir.string());
  }

  for (const auto& entry : std::filesystem::directory_iterator(symbolDir))
  {
    if (!entry.is_regular_file())
    {
      continue;
    }

    std::string filename = entry.path().filename().string();
    if (filename.find("bars_") != 0 || filename.find(".bin") == std::string::npos)
    {
      continue;
    }

    size_t underscorePos = filename.find('_');
    size_t dotPos = filename.find(".bin");
    if (underscorePos == std::string::npos || dotPos == std::string::npos)
    {
      continue;
    }

    std::string tfStr = filename.substr(underscorePos + 1, dotPos - underscorePos - 1);

    if (tfStr.back() != 's')
    {
      continue;
    }

    int seconds = std::stoi(tfStr.substr(0, tfStr.size() - 1));
    TimeframeId tf = TimeframeId::time(std::chrono::seconds(seconds));

    openTimeframe(entry.path(), tf);
  }

  if (_files.empty())
  {
    throw std::runtime_error("No bars_*.bin files found in: " + symbolDir.string());
  }
}

MmapBarStorage::~MmapBarStorage()
{
  closeAll();
}

MmapBarStorage::MmapBarStorage(MmapBarStorage&& other) noexcept
    : _symbolDir(std::move(other._symbolDir)), _files(std::move(other._files))
{
  other._files.clear();
}

MmapBarStorage& MmapBarStorage::operator=(MmapBarStorage&& other) noexcept
{
  if (this != &other)
  {
    closeAll();
    _symbolDir = std::move(other._symbolDir);
    _files = std::move(other._files);
    other._files.clear();
  }
  return *this;
}

void MmapBarStorage::openTimeframe(const std::filesystem::path& path, TimeframeId tf)
{
  MmapFile mf;
  mf.timeframe = tf;

#ifdef _WIN32
  mf.hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (mf.hFile == INVALID_HANDLE_VALUE)
  {
    throw std::runtime_error("Failed to open: " + path.string());
  }

  LARGE_INTEGER fileSize;
  if (!GetFileSizeEx(mf.hFile, &fileSize))
  {
    CloseHandle(mf.hFile);
    throw std::runtime_error("Failed to get file size: " + path.string());
  }
  mf.size = static_cast<size_t>(fileSize.QuadPart);

  if (mf.size < sizeof(uint64_t))
  {
    CloseHandle(mf.hFile);
    throw std::runtime_error("File too small: " + path.string());
  }

  mf.hMapping = CreateFileMappingW(mf.hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (mf.hMapping == nullptr)
  {
    CloseHandle(mf.hFile);
    throw std::runtime_error("CreateFileMapping failed: " + path.string());
  }

  mf.addr = MapViewOfFile(mf.hMapping, FILE_MAP_READ, 0, 0, 0);
  if (mf.addr == nullptr)
  {
    CloseHandle(mf.hMapping);
    CloseHandle(mf.hFile);
    throw std::runtime_error("MapViewOfFile failed: " + path.string());
  }
#else
  mf.fd = open(path.c_str(), O_RDONLY);
  if (mf.fd == -1)
  {
    throw std::runtime_error("Failed to open: " + path.string());
  }

  struct stat sb;
  if (fstat(mf.fd, &sb) == -1)
  {
    close(mf.fd);
    throw std::runtime_error("Failed to stat: " + path.string());
  }
  mf.size = sb.st_size;

  if (mf.size < sizeof(uint64_t))
  {
    close(mf.fd);
    throw std::runtime_error("File too small: " + path.string());
  }

  mf.addr = mmap(nullptr, mf.size, PROT_READ, MAP_PRIVATE, mf.fd, 0);
  if (mf.addr == MAP_FAILED)
  {
    close(mf.fd);
    throw std::runtime_error("mmap failed: " + path.string());
  }
#endif

  mf.barCount = *static_cast<const uint64_t*>(mf.addr);

  size_t expectedSize = sizeof(uint64_t) + mf.barCount * sizeof(Bar);
  if (mf.size != expectedSize)
  {
#ifdef _WIN32
    UnmapViewOfFile(mf.addr);
    CloseHandle(mf.hMapping);
    CloseHandle(mf.hFile);
#else
    munmap(mf.addr, mf.size);
    close(mf.fd);
#endif
    throw std::runtime_error("File size mismatch: " + path.string());
  }

  _files[tf] = mf;
}

void MmapBarStorage::closeAll()
{
  for (auto& [tf, mf] : _files)
  {
#ifdef _WIN32
    if (mf.addr != nullptr)
    {
      UnmapViewOfFile(mf.addr);
    }
    if (mf.hMapping != nullptr)
    {
      CloseHandle(mf.hMapping);
    }
    if (mf.hFile != INVALID_HANDLE_VALUE)
    {
      CloseHandle(mf.hFile);
    }
#else
    if (mf.addr != nullptr && mf.addr != MAP_FAILED)
    {
      munmap(mf.addr, mf.size);
    }
    if (mf.fd != -1)
    {
      close(mf.fd);
    }
#endif
  }
  _files.clear();
}

const Bar* MmapBarStorage::getBar(TimeframeId tf, size_t index) const
{
  auto it = _files.find(tf);
  if (it == _files.end())
  {
    return nullptr;
  }

  const auto& mf = it->second;
  if (index >= mf.barCount)
  {
    return nullptr;
  }

  const Bar* bars = reinterpret_cast<const Bar*>(static_cast<const char*>(mf.addr) + sizeof(uint64_t));
  return &bars[index];
}

const Bar* MmapBarStorage::findBar(TimeframeId tf, TimePoint time, char mode) const
{
  auto span = getBars(tf);
  if (span.empty())
  {
    return nullptr;
  }

  auto compare = [](const Bar& bar, TimePoint t)
  { return bar.endTime < t; };

  auto it = std::lower_bound(span.begin(), span.end(), time, compare);

  if (mode == 'e')
  {
    if (it != span.end() && it->endTime == time)
    {
      return &(*it);
    }
    return nullptr;
  }
  else if (mode == 'b')
  {
    if (it == span.begin())
    {
      return nullptr;
    }
    return &(*(--it));
  }
  else
  {
    if (it == span.end())
    {
      return nullptr;
    }
    return &(*it);
  }
}

std::span<const Bar> MmapBarStorage::getBars(TimeframeId tf) const
{
  auto it = _files.find(tf);
  if (it == _files.end())
  {
    return {};
  }

  const auto& mf = it->second;
  const Bar* bars = reinterpret_cast<const Bar*>(static_cast<const char*>(mf.addr) + sizeof(uint64_t));
  return std::span<const Bar>(bars, mf.barCount);
}

size_t MmapBarStorage::barCount(TimeframeId tf) const
{
  auto it = _files.find(tf);
  if (it == _files.end())
  {
    return 0;
  }
  return it->second.barCount;
}

size_t MmapBarStorage::totalBars() const
{
  size_t total = 0;
  for (const auto& [tf, mf] : _files)
  {
    total += mf.barCount;
  }
  return total;
}

std::pair<TimePoint, TimePoint> MmapBarStorage::timeRange() const
{
  TimePoint minTime = TimePoint::max();
  TimePoint maxTime = TimePoint::min();

  for (const auto& [tf, mf] : _files)
  {
    auto span = getBars(tf);
    if (!span.empty())
    {
      if (span.front().endTime < minTime)
      {
        minTime = span.front().endTime;
      }
      if (span.back().endTime > maxTime)
      {
        maxTime = span.back().endTime;
      }
    }
  }

  return {minTime, maxTime};
}

std::vector<TimeframeId> MmapBarStorage::timeframes() const
{
  std::vector<TimeframeId> result;
  result.reserve(_files.size());
  for (const auto& [tf, _] : _files)
  {
    result.push_back(tf);
  }
  return result;
}

}  // namespace flox
