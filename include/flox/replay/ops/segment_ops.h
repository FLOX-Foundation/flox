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
#include "flox/replay/readers/binary_log_reader.h"
#include "flox/replay/writers/binary_log_writer.h"

#include <filesystem>
#include <functional>
#include <set>
#include <vector>

namespace flox::replay
{

struct MergeResult
{
  bool success{false};
  std::filesystem::path output_path;
  uint32_t segments_merged{0};
  uint64_t events_written{0};
  uint64_t bytes_written{0};
  std::vector<std::string> errors;
};

struct SplitResult
{
  bool success{false};
  std::vector<std::filesystem::path> output_paths;
  uint32_t segments_created{0};
  uint64_t events_written{0};
  std::vector<std::string> errors;
};

struct ExportResult
{
  bool success{false};
  std::filesystem::path output_path;
  uint64_t events_exported{0};
  uint64_t bytes_written{0};
  std::vector<std::string> errors;
};

struct MergeConfig
{
  std::filesystem::path output_dir;
  std::string output_name;
  bool create_index{true};
  uint16_t index_interval{kDefaultIndexInterval};
  CompressionType compression{CompressionType::None};
  bool preserve_timestamps{true};
  bool sort_by_timestamp{true};
  uint64_t max_output_size{0};
};

enum class SplitMode
{
  ByTime,
  ByEventCount,
  BySize,
  BySymbol,
};

struct SplitConfig
{
  std::filesystem::path output_dir;
  SplitMode mode{SplitMode::ByTime};

  int64_t time_interval_ns{3600LL * 1000000000LL};
  uint64_t events_per_file{1000000};
  uint64_t bytes_per_file{256ull << 20};

  bool create_index{true};
  uint16_t index_interval{kDefaultIndexInterval};
  CompressionType compression{CompressionType::None};
};

enum class ExportFormat
{
  CSV,
  JSON,
  JSONLines,
  Binary,
};

struct ExportConfig
{
  std::filesystem::path output_path;
  ExportFormat format{ExportFormat::CSV};

  std::optional<int64_t> from_ts;
  std::optional<int64_t> to_ts;
  std::set<uint32_t> symbols;
  bool trades_only{false};
  bool books_only{false};

  char delimiter{','};
  bool include_header{true};

  bool pretty_print{false};
  int indent{2};

  CompressionType compression{CompressionType::None};
  bool create_index{true};
  uint16_t index_interval{kDefaultIndexInterval};
};

class SegmentOps
{
 public:
  using ProgressCallback = std::function<void(uint64_t, uint64_t)>;

  static MergeResult merge(const std::vector<std::filesystem::path>& input_paths,
                           const MergeConfig& config);
  static MergeResult mergeDirectory(const std::filesystem::path& input_dir,
                                    const MergeConfig& config);
  static MergeResult merge(const std::vector<std::filesystem::path>& input_paths,
                           const MergeConfig& config, ProgressCallback progress);

  static SplitResult split(const std::filesystem::path& input_path, const SplitConfig& config);
  static SplitResult splitDirectory(const std::filesystem::path& input_dir,
                                    const SplitConfig& config);
  static SplitResult split(const std::filesystem::path& input_path, const SplitConfig& config,
                           ProgressCallback progress);

  static ExportResult exportData(const std::filesystem::path& input_path,
                                 const ExportConfig& config);
  static ExportResult exportDirectory(const std::filesystem::path& input_dir,
                                      const ExportConfig& config);
  static ExportResult exportData(const std::filesystem::path& input_path,
                                 const ExportConfig& config, ProgressCallback progress);

  static bool recompress(const std::filesystem::path& input_path,
                         const std::filesystem::path& output_path, CompressionType new_compression);

  static uint64_t filter(const std::filesystem::path& input_path,
                         const std::filesystem::path& output_path,
                         const std::function<bool(const ReplayEvent&)>& predicate,
                         const WriterConfig& output_config);

  static uint64_t extractSymbols(const std::filesystem::path& input_path,
                                 const std::filesystem::path& output_path,
                                 const std::set<uint32_t>& symbols, const WriterConfig& config);

  static uint64_t extractTimeRange(const std::filesystem::path& input_path,
                                   const std::filesystem::path& output_path, int64_t from_ns,
                                   int64_t to_ns, const WriterConfig& config);

 private:
  static std::string formatCSVEvent(const ReplayEvent& event, char delimiter);
  static std::string formatJSONEvent(const ReplayEvent& event, bool pretty, int indent);
  static std::filesystem::path generateSplitPath(const std::filesystem::path& output_dir,
                                                 uint32_t index, SplitMode mode,
                                                 int64_t boundary_value);
};

inline MergeResult quickMerge(const std::filesystem::path& input_dir,
                              const std::filesystem::path& output_dir)
{
  MergeConfig config{.output_dir = output_dir};
  return SegmentOps::mergeDirectory(input_dir, config);
}

inline ExportResult quickExportCSV(const std::filesystem::path& input_path,
                                   const std::filesystem::path& output_path)
{
  ExportConfig config{.output_path = output_path, .format = ExportFormat::CSV};
  return SegmentOps::exportData(input_path, config);
}

inline SplitResult quickSplitByHour(const std::filesystem::path& input_path,
                                    const std::filesystem::path& output_dir)
{
  SplitConfig config{.output_dir = output_dir,
                     .mode = SplitMode::ByTime,
                     .time_interval_ns = 3600LL * 1000000000LL};
  return SegmentOps::split(input_path, config);
}

}  // namespace flox::replay
