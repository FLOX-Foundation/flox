/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/replay/ops/manifest.h"

#include <cstdint>
#include <set>
#include <vector>

namespace flox::replay
{

struct Partition
{
  uint32_t partition_id{0};

  int64_t from_ns{0};
  int64_t to_ns{0};
  int64_t warmup_from_ns{0};

  std::vector<SegmentInfo> segments;
  std::set<uint32_t> symbols;

  uint64_t estimated_events{0};
  uint64_t estimated_bytes{0};

  int64_t warmupDuration() const { return from_ns - warmup_from_ns; }
  int64_t processingDuration() const { return to_ns - from_ns; }
  int64_t totalDuration() const { return to_ns - warmup_from_ns; }

  bool hasWarmup() const { return warmup_from_ns < from_ns; }
  bool hasSymbolFilter() const { return !symbols.empty(); }
};

class Partitioner
{
 public:
  explicit Partitioner(const SegmentManifest& manifest);
  explicit Partitioner(const std::filesystem::path& data_dir);

  std::vector<Partition> partitionByTime(uint32_t num_partitions, int64_t warmup_ns = 0) const;

  std::vector<Partition> partitionByDuration(int64_t slice_duration_ns, int64_t warmup_ns = 0) const;

  enum class CalendarUnit
  {
    Hour,
    Day,
    Week,
    Month
  };
  std::vector<Partition> partitionByCalendar(CalendarUnit unit, int64_t warmup_ns = 0) const;

  std::vector<Partition> partitionBySymbol(uint32_t num_partitions) const;
  std::vector<Partition> partitionPerSymbol() const;

  std::vector<Partition> partitionByEventCount(uint32_t num_partitions) const;

  Partition createPartition(int64_t from_ns, int64_t to_ns, int64_t warmup_ns = 0,
                            const std::set<uint32_t>& symbols = {}) const;

  const SegmentManifest& manifest() const { return _manifest; }
  uint64_t estimateEventsInRange(int64_t from_ns, int64_t to_ns) const;
  int64_t totalDuration() const { return _manifest.lastTimestamp() - _manifest.firstTimestamp(); }

 private:
  SegmentManifest _manifest;

  void assignSegmentsToPartition(Partition& partition) const;
  void estimatePartitionStats(Partition& partition) const;
};

inline std::vector<Partition> partitionByTime(const std::filesystem::path& data_dir,
                                              uint32_t num_partitions, int64_t warmup_ns = 0)
{
  Partitioner partitioner(data_dir);
  return partitioner.partitionByTime(num_partitions, warmup_ns);
}

inline std::vector<Partition> partitionBySymbol(const std::filesystem::path& data_dir,
                                                uint32_t num_partitions)
{
  Partitioner partitioner(data_dir);
  return partitioner.partitionBySymbol(num_partitions);
}

std::vector<std::byte> serializePartition(const Partition& partition);
std::optional<Partition> deserializePartition(const std::vector<std::byte>& data);
std::string partitionToJson(const Partition& partition);

}  // namespace flox::replay
