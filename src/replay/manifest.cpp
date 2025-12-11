/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/ops/manifest.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace flox::replay
{

std::optional<SegmentManifest> SegmentManifest::load(const std::filesystem::path& manifest_path)
{
  std::ifstream file(manifest_path, std::ios::binary);
  if (!file)
  {
    return std::nullopt;
  }

  ManifestHeader header;
  file.read(reinterpret_cast<char*>(&header), sizeof(header));
  if (!file || !header.isValid())
  {
    return std::nullopt;
  }

  SegmentManifest manifest;
  manifest._data_dir = manifest_path.parent_path();
  manifest._total_events = header.total_events;
  manifest._total_bytes = header.total_bytes;
  manifest._first_ts = header.first_timestamp_ns;
  manifest._last_ts = header.last_timestamp_ns;

  // Read segments
  manifest._segments.reserve(header.segment_count);
  for (uint64_t i = 0; i < header.segment_count; ++i)
  {
    ManifestSegmentEntry entry;
    file.read(reinterpret_cast<char*>(&entry), sizeof(entry));
    if (!file)
    {
      return std::nullopt;
    }

    SegmentInfo info;
    info.path = manifest._data_dir / entry.filename;
    info.first_event_ns = entry.first_event_ns;
    info.last_event_ns = entry.last_event_ns;
    info.event_count = static_cast<uint32_t>(entry.event_count);
    info.has_index = entry.hasIndex();
    manifest._segments.push_back(std::move(info));
  }

  // Read symbols
  for (uint32_t i = 0; i < header.symbol_count; ++i)
  {
    uint32_t symbol_id;
    file.read(reinterpret_cast<char*>(&symbol_id), sizeof(symbol_id));
    if (!file)
    {
      return std::nullopt;
    }
    manifest._symbols.insert(symbol_id);
  }

  manifest._build_time = std::filesystem::last_write_time(manifest_path);
  return manifest;
}

SegmentManifest SegmentManifest::build(const std::filesystem::path& data_dir)
{
  SegmentManifest manifest;
  manifest._data_dir = data_dir;

  // Use BinaryLogReader to scan
  ReaderConfig config{.data_dir = data_dir};
  BinaryLogReader reader(config);

  // Force scanning by calling summary()
  reader.summary();

  // Get summary with symbols
  auto summary = BinaryLogReader::inspectWithSymbols(data_dir);

  manifest._segments = reader.segments();
  manifest._symbols = summary.symbols;
  manifest._total_events = summary.total_events;
  manifest._total_bytes = summary.total_bytes;
  manifest._first_ts = summary.first_event_ns;
  manifest._last_ts = summary.last_event_ns;
  manifest._build_time = std::filesystem::file_time_type::clock::now();

  // Sort segments by timestamp
  std::sort(manifest._segments.begin(), manifest._segments.end(),
            [](const SegmentInfo& a, const SegmentInfo& b)
            {
              return a.first_event_ns < b.first_event_ns;
            });

  return manifest;
}

SegmentManifest SegmentManifest::buildAndSave(const std::filesystem::path& data_dir)
{
  auto manifest = build(data_dir);
  manifest.save();
  return manifest;
}

bool SegmentManifest::save(const std::filesystem::path& manifest_path) const
{
  std::ofstream file(manifest_path, std::ios::binary);
  if (!file)
  {
    return false;
  }

  ManifestHeader header;
  header.segment_count = _segments.size();
  header.total_events = _total_events;
  header.total_bytes = _total_bytes;
  header.first_timestamp_ns = _first_ts;
  header.last_timestamp_ns = _last_ts;
  header.symbol_count = static_cast<uint32_t>(_symbols.size());

  file.write(reinterpret_cast<const char*>(&header), sizeof(header));

  // Write segments
  for (const auto& seg : _segments)
  {
    ManifestSegmentEntry entry{};
    auto relative = std::filesystem::relative(seg.path, _data_dir).string();
    std::strncpy(entry.filename, relative.c_str(), sizeof(entry.filename) - 1);
    entry.first_event_ns = seg.first_event_ns;
    entry.last_event_ns = seg.last_event_ns;
    entry.event_count = seg.event_count;

    if (std::filesystem::exists(seg.path))
    {
      entry.file_size = std::filesystem::file_size(seg.path);
    }

    if (seg.has_index)
    {
      entry.flags |= ManifestSegmentEntry::kFlagHasIndex;
    }

    file.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
  }

  // Write symbols
  for (uint32_t symbol_id : _symbols)
  {
    file.write(reinterpret_cast<const char*>(&symbol_id), sizeof(symbol_id));
  }

  return file.good();
}

bool SegmentManifest::save() const
{
  return save(manifestPath(_data_dir));
}

std::vector<SegmentInfo> SegmentManifest::segmentsInRange(int64_t from_ns, int64_t to_ns) const
{
  std::vector<SegmentInfo> result;

  for (const auto& seg : _segments)
  {
    // Segment overlaps if: seg.first <= to && seg.last >= from
    if (seg.first_event_ns <= to_ns && seg.last_event_ns >= from_ns)
    {
      result.push_back(seg);
    }
  }

  return result;
}

std::vector<SegmentInfo> SegmentManifest::segmentsWithSymbols(
    const std::set<uint32_t>& /*symbols*/) const
{
  // Without per-segment symbol info, return all segments
  // In future: could store symbol bitmap per segment
  return _segments;
}

bool SegmentManifest::isUpToDate() const
{
  if (_segments.empty())
  {
    return false;
  }

  // Check if any segment files are newer than manifest
  for (const auto& seg : _segments)
  {
    if (!std::filesystem::exists(seg.path))
    {
      return false;  // Segment deleted
    }

    auto seg_time = std::filesystem::last_write_time(seg.path);
    if (seg_time > _build_time)
    {
      return false;  // Segment modified
    }
  }

  // Check for new files
  for (const auto& entry : std::filesystem::directory_iterator(_data_dir))
  {
    if (entry.path().extension() == ".floxlog")
    {
      auto found = std::find_if(_segments.begin(), _segments.end(),
                                [&entry](const SegmentInfo& s)
                                { return s.path == entry.path(); });

      if (found == _segments.end())
      {
        return false;  // New segment added
      }
    }
  }

  return true;
}

SegmentManifest getOrBuildManifest(const std::filesystem::path& data_dir)
{
  auto mpath = manifestPath(data_dir);

  if (std::filesystem::exists(mpath))
  {
    auto manifest = SegmentManifest::load(mpath);
    if (manifest && manifest->isUpToDate())
    {
      return std::move(*manifest);
    }
  }

  // Build and save
  return SegmentManifest::buildAndSave(data_dir);
}

}  // namespace flox::replay
