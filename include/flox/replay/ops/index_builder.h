/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/replay/binary_format_v1.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace flox::replay
{

struct IndexBuilderConfig
{
  uint16_t index_interval{kDefaultIndexInterval};
  bool verify_crc{true};
  bool backup_original{false};
};

struct IndexBuildResult
{
  bool success{false};
  std::string error;
  uint32_t events_scanned{0};
  uint32_t index_entries_created{0};
};

class IndexBuilder
{
 public:
  explicit IndexBuilder(IndexBuilderConfig config = {});

  IndexBuildResult buildForSegment(const std::filesystem::path& segment_path);
  std::vector<IndexBuildResult> buildForDirectory(const std::filesystem::path& dir);

  static bool hasIndex(const std::filesystem::path& segment_path);
  static bool removeIndex(const std::filesystem::path& segment_path);

 private:
  IndexBuilderConfig _config;
};

struct GlobalIndexBuildResult
{
  bool success{false};
  std::string error;
  uint32_t segments_indexed{0};
  uint64_t total_events{0};
};

class GlobalIndexBuilder
{
 public:
  static GlobalIndexBuildResult build(const std::filesystem::path& data_dir,
                                      const std::filesystem::path& output_path = {});

  static std::optional<std::vector<GlobalIndexSegment>> load(const std::filesystem::path& index_path);
};

}  // namespace flox::replay
