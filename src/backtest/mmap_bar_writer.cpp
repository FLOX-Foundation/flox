/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/mmap_bar_writer.h"

#include <stdexcept>

namespace flox
{

static SubscriberId generateWriterId()
{
  static std::atomic<SubscriberId> counter{1000000};
  return counter++;
}

MmapBarWriter::MmapBarWriter(const std::filesystem::path& symbolDir)
    : _symbolDir(symbolDir), _subscriberId(generateWriterId())
{
  if (!std::filesystem::exists(symbolDir))
  {
    std::filesystem::create_directories(symbolDir);
  }
}

MmapBarWriter::~MmapBarWriter()
{
  try
  {
    flush();
  }
  catch (...)
  {
    // Suppress exceptions in destructor
  }
}

void MmapBarWriter::onBar(const BarEvent& event) noexcept
{
  TimeframeId tf{event.barType, event.barTypeParam};

  std::lock_guard lock(_mutex);
  _buffers[tf].push_back(event.bar);
}

void MmapBarWriter::flush()
{
  std::lock_guard lock(_mutex);

  for (auto& [tf, bars] : _buffers)
  {
    if (bars.empty())
    {
      continue;
    }

    auto filename = makeFilename(tf);

    std::vector<Bar> existingBars;
    if (std::filesystem::exists(filename))
    {
      std::ifstream inFile(filename, std::ios::binary);
      if (inFile)
      {
        uint64_t count = 0;
        inFile.read(reinterpret_cast<char*>(&count), sizeof(count));
        existingBars.resize(count);
        inFile.read(reinterpret_cast<char*>(existingBars.data()), count * sizeof(Bar));
      }
    }

    existingBars.insert(existingBars.end(), bars.begin(), bars.end());

    std::ofstream outFile(filename, std::ios::binary | std::ios::trunc);
    if (!outFile)
    {
      throw std::runtime_error("Failed to open file for writing: " + filename.string());
    }

    uint64_t count = existingBars.size();
    outFile.write(reinterpret_cast<const char*>(&count), sizeof(count));
    outFile.write(reinterpret_cast<const char*>(existingBars.data()), count * sizeof(Bar));

    bars.clear();
  }
}

size_t MmapBarWriter::bufferedBars(TimeframeId tf) const
{
  std::lock_guard lock(_mutex);
  auto it = _buffers.find(tf);
  return it != _buffers.end() ? it->second.size() : 0;
}

size_t MmapBarWriter::totalBufferedBars() const
{
  std::lock_guard lock(_mutex);
  size_t total = 0;
  for (const auto& [_, bars] : _buffers)
  {
    total += bars.size();
  }
  return total;
}

void MmapBarWriter::clear()
{
  std::lock_guard lock(_mutex);
  _buffers.clear();
}

void MmapBarWriter::writeBars(TimeframeId tf, std::span<const Bar> bars)
{
  if (bars.empty())
  {
    return;
  }

  auto filename = makeFilename(tf);

  std::ofstream outFile(filename, std::ios::binary | std::ios::trunc);
  if (!outFile)
  {
    throw std::runtime_error("Failed to open file for writing: " + filename.string());
  }

  uint64_t count = bars.size();
  outFile.write(reinterpret_cast<const char*>(&count), sizeof(count));
  outFile.write(reinterpret_cast<const char*>(bars.data()), count * sizeof(Bar));
}

std::filesystem::path MmapBarWriter::makeFilename(TimeframeId tf) const
{
  if (tf.type != BarType::Time)
  {
    throw std::runtime_error("MmapBarWriter currently only supports time-based bars");
  }

  auto seconds = tf.param / 1'000'000'000ULL;
  return _symbolDir / ("bars_" + std::to_string(seconds) + "s.bin");
}

void MmapBarWriter::setMetadata(const std::string& key, const std::string& value)
{
  std::lock_guard lock(_mutex);
  _metadata[key] = value;
}

void MmapBarWriter::setMetadata(const std::map<std::string, std::string>& metadata)
{
  std::lock_guard lock(_mutex);
  for (const auto& [key, value] : metadata)
  {
    _metadata[key] = value;
  }
}

void MmapBarWriter::writeMetadata()
{
  std::lock_guard lock(_mutex);

  if (_metadata.empty())
  {
    return;
  }

  auto metadataPath = _symbolDir / ".symbol_metadata";
  std::ofstream file(metadataPath);

  if (!file)
  {
    throw std::runtime_error("Failed to open metadata file: " + metadataPath.string());
  }

  for (const auto& [key, value] : _metadata)
  {
    file << key << "=" << value << "\n";
  }
}

}  // namespace flox
