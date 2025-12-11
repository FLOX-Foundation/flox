/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/ops/partitioner.h"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace flox::replay
{

Partitioner::Partitioner(const SegmentManifest& manifest) : _manifest(manifest) {}

Partitioner::Partitioner(const std::filesystem::path& data_dir)
    : _manifest(getOrBuildManifest(data_dir))
{
}

std::vector<Partition> Partitioner::partitionByTime(
    uint32_t num_partitions,
    int64_t warmup_ns) const
{
  if (num_partitions == 0 || _manifest.empty())
  {
    return {};
  }

  std::vector<Partition> partitions;
  partitions.reserve(num_partitions);

  int64_t first_ts = _manifest.firstTimestamp();
  int64_t last_ts = _manifest.lastTimestamp();
  int64_t total_duration = last_ts - first_ts;
  int64_t slice_duration = total_duration / num_partitions;

  for (uint32_t i = 0; i < num_partitions; ++i)
  {
    Partition p;
    p.partition_id = i;
    p.from_ns = first_ts + i * slice_duration;
    p.to_ns = (i == num_partitions - 1) ? last_ts : (first_ts + (i + 1) * slice_duration);
    p.warmup_from_ns = std::max(first_ts, p.from_ns - warmup_ns);

    assignSegmentsToPartition(p);
    estimatePartitionStats(p);

    partitions.push_back(std::move(p));
  }

  return partitions;
}

std::vector<Partition> Partitioner::partitionByDuration(
    int64_t slice_duration_ns,
    int64_t warmup_ns) const
{
  if (slice_duration_ns <= 0 || _manifest.empty())
  {
    return {};
  }

  std::vector<Partition> partitions;

  int64_t first_ts = _manifest.firstTimestamp();
  int64_t last_ts = _manifest.lastTimestamp();
  int64_t current = first_ts;
  uint32_t id = 0;

  while (current < last_ts)
  {
    Partition p;
    p.partition_id = id++;
    p.from_ns = current;
    p.to_ns = std::min(current + slice_duration_ns, last_ts);
    p.warmup_from_ns = std::max(first_ts, p.from_ns - warmup_ns);

    assignSegmentsToPartition(p);
    estimatePartitionStats(p);

    partitions.push_back(std::move(p));
    current += slice_duration_ns;
  }

  return partitions;
}

std::vector<Partition> Partitioner::partitionByCalendar(
    CalendarUnit unit,
    int64_t warmup_ns) const
{
  // Convert unit to nanoseconds
  int64_t slice_ns;
  switch (unit)
  {
    case CalendarUnit::Hour:
      slice_ns = 3600LL * 1'000'000'000LL;
      break;
    case CalendarUnit::Day:
      slice_ns = 24LL * 3600LL * 1'000'000'000LL;
      break;
    case CalendarUnit::Week:
      slice_ns = 7LL * 24LL * 3600LL * 1'000'000'000LL;
      break;
    case CalendarUnit::Month:
      slice_ns = 30LL * 24LL * 3600LL * 1'000'000'000LL;  // Approximate
      break;
    default:
      slice_ns = 24LL * 3600LL * 1'000'000'000LL;
  }

  return partitionByDuration(slice_ns, warmup_ns);
}

std::vector<Partition> Partitioner::partitionBySymbol(uint32_t num_partitions) const
{
  if (num_partitions == 0 || _manifest.empty())
  {
    return {};
  }

  const auto& all_symbols = _manifest.symbols();
  if (all_symbols.empty())
  {
    return {};
  }

  std::vector<uint32_t> symbols_vec(all_symbols.begin(), all_symbols.end());
  std::vector<Partition> partitions;
  partitions.reserve(num_partitions);

  uint32_t symbols_per_partition = (symbols_vec.size() + num_partitions - 1) / num_partitions;

  for (uint32_t i = 0; i < num_partitions; ++i)
  {
    Partition p;
    p.partition_id = i;
    p.from_ns = _manifest.firstTimestamp();
    p.to_ns = _manifest.lastTimestamp();
    p.warmup_from_ns = p.from_ns;

    // Assign symbols to this partition
    size_t start = i * symbols_per_partition;
    size_t end = std::min(start + symbols_per_partition, symbols_vec.size());

    for (size_t j = start; j < end; ++j)
    {
      p.symbols.insert(symbols_vec[j]);
    }

    if (!p.symbols.empty())
    {
      p.segments = _manifest.segments();  // All segments (filter at read time)
      estimatePartitionStats(p);
      partitions.push_back(std::move(p));
    }
  }

  return partitions;
}

std::vector<Partition> Partitioner::partitionPerSymbol() const
{
  const auto& all_symbols = _manifest.symbols();

  std::vector<Partition> partitions;
  partitions.reserve(all_symbols.size());

  uint32_t id = 0;
  for (uint32_t symbol : all_symbols)
  {
    Partition p;
    p.partition_id = id++;
    p.from_ns = _manifest.firstTimestamp();
    p.to_ns = _manifest.lastTimestamp();
    p.warmup_from_ns = p.from_ns;
    p.symbols = {symbol};
    p.segments = _manifest.segments();

    // Rough estimate: events / symbols
    p.estimated_events = _manifest.totalEvents() / all_symbols.size();
    p.estimated_bytes = _manifest.totalBytes() / all_symbols.size();

    partitions.push_back(std::move(p));
  }

  return partitions;
}

std::vector<Partition> Partitioner::partitionByEventCount(uint32_t num_partitions) const
{
  if (num_partitions == 0 || _manifest.empty())
  {
    return {};
  }

  uint64_t total_events = _manifest.totalEvents();
  uint64_t events_per_partition = total_events / num_partitions;

  std::vector<Partition> partitions;
  partitions.reserve(num_partitions);

  const auto& segments = _manifest.segments();
  uint64_t current_events = 0;
  size_t seg_start = 0;
  uint32_t id = 0;

  for (size_t i = 0; i < segments.size(); ++i)
  {
    current_events += segments[i].event_count;

    bool is_last = (i == segments.size() - 1);
    bool threshold_reached = (current_events >= events_per_partition);
    bool is_last_partition = (id == num_partitions - 1);

    if ((threshold_reached && !is_last_partition) || is_last)
    {
      Partition p;
      p.partition_id = id++;
      p.from_ns = segments[seg_start].first_event_ns;
      p.to_ns = segments[i].last_event_ns;
      p.warmup_from_ns = p.from_ns;

      for (size_t j = seg_start; j <= i; ++j)
      {
        p.segments.push_back(segments[j]);
        p.estimated_events += segments[j].event_count;
      }

      partitions.push_back(std::move(p));
      seg_start = i + 1;
      current_events = 0;
    }
  }

  return partitions;
}

Partition Partitioner::createPartition(
    int64_t from_ns,
    int64_t to_ns,
    int64_t warmup_ns,
    const std::set<uint32_t>& symbols) const
{
  Partition p;
  p.partition_id = 0;
  p.from_ns = from_ns;
  p.to_ns = to_ns;
  p.warmup_from_ns = from_ns - warmup_ns;
  p.symbols = symbols;

  assignSegmentsToPartition(p);
  estimatePartitionStats(p);

  return p;
}

uint64_t Partitioner::estimateEventsInRange(int64_t from_ns, int64_t to_ns) const
{
  uint64_t total = 0;
  for (const auto& seg : _manifest.segments())
  {
    if (seg.first_event_ns <= to_ns && seg.last_event_ns >= from_ns)
    {
      // Rough estimate based on overlap
      int64_t seg_duration = seg.last_event_ns - seg.first_event_ns;
      if (seg_duration > 0)
      {
        int64_t overlap_start = std::max(from_ns, seg.first_event_ns);
        int64_t overlap_end = std::min(to_ns, seg.last_event_ns);
        double overlap_ratio = static_cast<double>(overlap_end - overlap_start) / seg_duration;
        total += static_cast<uint64_t>(seg.event_count * overlap_ratio);
      }
      else
      {
        total += seg.event_count;
      }
    }
  }
  return total;
}

void Partitioner::assignSegmentsToPartition(Partition& partition) const
{
  partition.segments = _manifest.segmentsInRange(partition.warmup_from_ns, partition.to_ns);
}

void Partitioner::estimatePartitionStats(Partition& partition) const
{
  partition.estimated_events = estimateEventsInRange(partition.warmup_from_ns, partition.to_ns);

  partition.estimated_bytes = 0;
  for (const auto& seg : partition.segments)
  {
    if (std::filesystem::exists(seg.path))
    {
      partition.estimated_bytes += std::filesystem::file_size(seg.path);
    }
  }
}

std::vector<std::byte> serializePartition(const Partition& partition)
{
  std::vector<std::byte> data;

  auto write = [&data](const void* ptr, size_t size)
  {
    const auto* bytes = reinterpret_cast<const std::byte*>(ptr);
    data.insert(data.end(), bytes, bytes + size);
  };

  // Header
  write(&partition.partition_id, sizeof(partition.partition_id));
  write(&partition.from_ns, sizeof(partition.from_ns));
  write(&partition.to_ns, sizeof(partition.to_ns));
  write(&partition.warmup_from_ns, sizeof(partition.warmup_from_ns));
  write(&partition.estimated_events, sizeof(partition.estimated_events));
  write(&partition.estimated_bytes, sizeof(partition.estimated_bytes));

  // Symbols
  uint32_t symbol_count = static_cast<uint32_t>(partition.symbols.size());
  write(&symbol_count, sizeof(symbol_count));
  for (uint32_t sym : partition.symbols)
  {
    write(&sym, sizeof(sym));
  }

  // Segment paths
  uint32_t segment_count = static_cast<uint32_t>(partition.segments.size());
  write(&segment_count, sizeof(segment_count));
  for (const auto& seg : partition.segments)
  {
    std::string path_str = seg.path.string();
    uint32_t path_len = static_cast<uint32_t>(path_str.size());
    write(&path_len, sizeof(path_len));
    write(path_str.data(), path_len);
    write(&seg.first_event_ns, sizeof(seg.first_event_ns));
    write(&seg.last_event_ns, sizeof(seg.last_event_ns));
    write(&seg.event_count, sizeof(seg.event_count));
  }

  return data;
}

std::optional<Partition> deserializePartition(const std::vector<std::byte>& data)
{
  if (data.size() < sizeof(Partition::partition_id))
  {
    return std::nullopt;
  }

  size_t offset = 0;
  auto read = [&data, &offset](void* ptr, size_t size) -> bool
  {
    if (offset + size > data.size())
    {
      return false;
    }
    std::memcpy(ptr, data.data() + offset, size);
    offset += size;
    return true;
  };

  Partition p;

  if (!read(&p.partition_id, sizeof(p.partition_id)))
  {
    return std::nullopt;
  }
  if (!read(&p.from_ns, sizeof(p.from_ns)))
  {
    return std::nullopt;
  }
  if (!read(&p.to_ns, sizeof(p.to_ns)))
  {
    return std::nullopt;
  }
  if (!read(&p.warmup_from_ns, sizeof(p.warmup_from_ns)))
  {
    return std::nullopt;
  }
  if (!read(&p.estimated_events, sizeof(p.estimated_events)))
  {
    return std::nullopt;
  }
  if (!read(&p.estimated_bytes, sizeof(p.estimated_bytes)))
  {
    return std::nullopt;
  }

  uint32_t symbol_count;
  if (!read(&symbol_count, sizeof(symbol_count)))
  {
    return std::nullopt;
  }
  for (uint32_t i = 0; i < symbol_count; ++i)
  {
    uint32_t sym;
    if (!read(&sym, sizeof(sym)))
    {
      return std::nullopt;
    }
    p.symbols.insert(sym);
  }

  uint32_t segment_count;
  if (!read(&segment_count, sizeof(segment_count)))
  {
    return std::nullopt;
  }
  p.segments.reserve(segment_count);

  for (uint32_t i = 0; i < segment_count; ++i)
  {
    uint32_t path_len;
    if (!read(&path_len, sizeof(path_len)))
    {
      return std::nullopt;
    }

    std::string path_str(path_len, '\0');
    if (!read(path_str.data(), path_len))
    {
      return std::nullopt;
    }

    SegmentInfo seg;
    seg.path = path_str;
    if (!read(&seg.first_event_ns, sizeof(seg.first_event_ns)))
    {
      return std::nullopt;
    }
    if (!read(&seg.last_event_ns, sizeof(seg.last_event_ns)))
    {
      return std::nullopt;
    }
    if (!read(&seg.event_count, sizeof(seg.event_count)))
    {
      return std::nullopt;
    }

    p.segments.push_back(std::move(seg));
  }

  return p;
}

std::string partitionToJson(const Partition& partition)
{
  std::ostringstream ss;
  ss << "{\n";
  ss << "  \"partition_id\": " << partition.partition_id << ",\n";
  ss << "  \"from_ns\": " << partition.from_ns << ",\n";
  ss << "  \"to_ns\": " << partition.to_ns << ",\n";
  ss << "  \"warmup_from_ns\": " << partition.warmup_from_ns << ",\n";
  ss << "  \"estimated_events\": " << partition.estimated_events << ",\n";
  ss << "  \"estimated_bytes\": " << partition.estimated_bytes << ",\n";
  ss << "  \"symbols\": [";

  bool first = true;
  for (uint32_t sym : partition.symbols)
  {
    if (!first)
    {
      ss << ", ";
    }
    ss << sym;
    first = false;
  }
  ss << "],\n";

  ss << "  \"segments\": [\n";
  for (size_t i = 0; i < partition.segments.size(); ++i)
  {
    const auto& seg = partition.segments[i];
    ss << "    {\"path\": \"" << seg.path.string() << "\", ";
    ss << "\"events\": " << seg.event_count << "}";
    if (i < partition.segments.size() - 1)
    {
      ss << ",";
    }
    ss << "\n";
  }
  ss << "  ]\n";
  ss << "}";

  return ss.str();
}

}  // namespace flox::replay
