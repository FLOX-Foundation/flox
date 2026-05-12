/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/readers/binary_log_reader.h"
#include "flox/replay/aggregator.h"
#include "flox/replay/ops/compression.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace flox::replay
{

namespace
{

// Extract the first non-zero timestamp from a decompressed block buffer.
int64_t firstTimestampInBlock(const std::vector<std::byte>& data)
{
  size_t offset = 0;
  while (offset + sizeof(FrameHeader) <= data.size())
  {
    FrameHeader frame;
    std::memcpy(&frame, data.data() + offset, sizeof(frame));
    offset += sizeof(FrameHeader);
    if (offset + frame.size > data.size())
    {
      break;
    }

    int64_t ts = 0;
    auto type = static_cast<EventType>(frame.type);
    if (type == EventType::Trade && frame.size >= sizeof(TradeRecord))
    {
      TradeRecord tr;
      std::memcpy(&tr, data.data() + offset, sizeof(TradeRecord));
      ts = tr.exchange_ts_ns;
    }
    else if ((type == EventType::BookSnapshot || type == EventType::BookDelta) &&
             frame.size >= sizeof(BookRecordHeader))
    {
      BookRecordHeader bh;
      std::memcpy(&bh, data.data() + offset, sizeof(BookRecordHeader));
      ts = bh.exchange_ts_ns;
    }
    if (ts > 0)
    {
      return ts;
    }

    offset += frame.size;
  }
  return 0;
}

// Extract the last non-zero timestamp from a decompressed block buffer.
int64_t lastTimestampInBlock(const std::vector<std::byte>& data)
{
  int64_t last = 0;
  size_t offset = 0;
  while (offset + sizeof(FrameHeader) <= data.size())
  {
    FrameHeader frame;
    std::memcpy(&frame, data.data() + offset, sizeof(frame));
    offset += sizeof(FrameHeader);
    if (offset + frame.size > data.size())
    {
      break;
    }

    int64_t ts = 0;
    auto type = static_cast<EventType>(frame.type);
    if (type == EventType::Trade && frame.size >= sizeof(TradeRecord))
    {
      TradeRecord tr;
      std::memcpy(&tr, data.data() + offset, sizeof(TradeRecord));
      ts = tr.exchange_ts_ns;
    }
    else if ((type == EventType::BookSnapshot || type == EventType::BookDelta) &&
             frame.size >= sizeof(BookRecordHeader))
    {
      BookRecordHeader bh;
      std::memcpy(&bh, data.data() + offset, sizeof(BookRecordHeader));
      ts = bh.exchange_ts_ns;
    }
    if (ts > 0)
    {
      last = ts;
    }

    offset += frame.size;
  }
  return last;
}

// Decompress a single block at a known file position.
//
// Returns {} on any inconsistency (truncated block, implausible header sizes,
// failed decompression). Important for live tapes: the actively-written tail
// block can have a header on disk before its payload is finished, leaving
// `compressed_size` pointing past EOF or — worse, if the header bytes weren't
// yet written and contain prior garbage — to a value in the gigabytes that
// would OOM the recovery path.
std::vector<std::byte> decompressBlock(std::FILE* f, long data_pos,
                                       const CompressedBlockHeader& block,
                                       CompressionType comp_type)
{
  if (std::fseek(f, 0, SEEK_END) != 0)
  {
    return {};
  }
  long file_end = std::ftell(f);
  if (file_end < 0 || data_pos < 0 || data_pos > file_end)
  {
    return {};
  }
  if (block.compressed_size == 0 ||
      block.compressed_size > static_cast<uint32_t>(file_end - data_pos))
  {
    return {};
  }

  std::vector<std::byte> compressed(block.compressed_size);
  if (std::fseek(f, data_pos, SEEK_SET) != 0)
  {
    return {};
  }
  if (std::fread(compressed.data(), 1, block.compressed_size, f) != block.compressed_size)
  {
    return {};
  }
  return Compressor::decompress(comp_type, std::span<const std::byte>(compressed),
                                block.original_size);
}

// For compressed segments where the writer did not finalize the segment header
// (event_count/first_event_ns/last_event_ns are all 0), recover these fields by:
//   - scanning all CompressedBlockHeaders to sum event_count (no decompression)
//   - decompressing only the necessary blocks for timestamps
//
// The last block of an active segment is often truncated (the writer flushes the
// block header before completing the compressed data write). We therefore iterate
// backwards from the end to find the last block that decompresses successfully.
void recoverCompressedSegmentMeta(std::FILE* f, const SegmentHeader& hdr,
                                  uint32_t& out_event_count,
                                  int64_t& out_first_ns, int64_t& out_last_ns)
{
  out_event_count = 0;
  out_first_ns = 0;
  out_last_ns = 0;

  if (std::fseek(f, 0, SEEK_END) != 0)
  {
    return;
  }
  const long file_end = std::ftell(f);
  if (file_end < 0)
  {
    return;
  }

  const long stop_at =
      std::min<long>(file_end, (hdr.hasIndex() && hdr.index_offset > 0)
                                   ? static_cast<long>(hdr.index_offset)
                                   : std::numeric_limits<long>::max());

  if (std::fseek(f, sizeof(SegmentHeader), SEEK_SET) != 0)
  {
    return;
  }

  // Scan all block headers: count events, collect positions of non-empty blocks.
  // Stops cleanly on the live-tape tail: a partially-written block header is
  // either invalid (caught by `block.isValid()`) or claims a `compressed_size`
  // past EOF (caught below). Without the EOF check, a stale `compressed_size`
  // in unflushed header bytes can lead recoverCompressedSegmentMeta to allocate
  // gigabytes when the recovery path later tries to read the block.
  struct BlockLoc
  {
    long data_pos;
    CompressedBlockHeader hdr;
  };
  std::vector<BlockLoc> blocks;

  for (;;)
  {
    long pos = std::ftell(f);
    if (pos < 0 || pos >= stop_at)
    {
      break;
    }

    CompressedBlockHeader block;
    if (std::fread(&block, sizeof(block), 1, f) != 1 || !block.isValid())
    {
      break;
    }

    long data_pos = std::ftell(f);
    if (data_pos < 0 ||
        static_cast<long>(block.compressed_size) > stop_at - data_pos)
    {
      // Block claims to extend past the file (or past the index trailer) —
      // tail of a live-writing segment. Don't include it in the event count
      // or the decompression list.
      break;
    }
    out_event_count += block.event_count;

    if (block.event_count > 0)
    {
      blocks.push_back({data_pos, block});
    }

    if (std::fseek(f, data_pos + static_cast<long>(block.compressed_size), SEEK_SET) != 0)
    {
      break;
    }
  }

  if (blocks.empty())
  {
    return;
  }

  auto comp_type = hdr.compressionType();

  // first_ns: try blocks forward until one decompresses.
  for (const auto& bl : blocks)
  {
    auto data = decompressBlock(f, bl.data_pos, bl.hdr, comp_type);
    if (!data.empty())
    {
      int64_t ts = firstTimestampInBlock(data);
      if (ts > 0)
      {
        out_first_ns = ts;
        break;
      }
    }
  }

  // last_ns: try blocks backward — the last block is often truncated.
  for (int i = static_cast<int>(blocks.size()) - 1; i >= 0; --i)
  {
    auto data = decompressBlock(f, blocks[i].data_pos, blocks[i].hdr, comp_type);
    if (!data.empty())
    {
      int64_t ts = lastTimestampInBlock(data);
      if (ts > 0)
      {
        out_last_ns = ts;
        break;
      }
    }
  }
}

}  // namespace

BinaryLogReader::BinaryLogReader(ReaderConfig config) : _config(std::move(config)) {}

BinaryLogReader::~BinaryLogReader() = default;

BinaryLogReader::BinaryLogReader(BinaryLogReader&& other) noexcept
    : _config(std::move(other._config)),
      _stats(other._stats),
      _segments(std::move(other._segments)),
      _scanned(other._scanned)
{
}

BinaryLogReader& BinaryLogReader::operator=(BinaryLogReader&& other) noexcept
{
  if (this != &other)
  {
    _config = std::move(other._config);
    _stats = other._stats;
    _segments = std::move(other._segments);
    _scanned = other._scanned;
  }
  return *this;
}

bool BinaryLogReader::scanSegments()
{
  if (_scanned)
  {
    return true;
  }

  namespace fs = std::filesystem;

  if (!fs::exists(_config.data_dir))
  {
    return false;
  }

  std::vector<std::filesystem::path> paths;
  for (const auto& entry : fs::directory_iterator(_config.data_dir))
  {
    if (entry.is_regular_file() && entry.path().extension() == ".floxlog")
    {
      paths.push_back(entry.path());
    }
  }

  // Sort by filename (which is timestamp-based)
  std::sort(paths.begin(), paths.end());

  // Read segment headers to build metadata
  for (const auto& path : paths)
  {
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f)
    {
      continue;
    }

    SegmentHeader header;
    if (std::fread(&header, sizeof(header), 1, f) == 1 && header.isValid())
    {
      SegmentInfo info;
      info.path = path;
      info.first_event_ns = header.first_event_ns;
      info.last_event_ns = header.last_event_ns;
      info.event_count = header.event_count;
      info.has_index = header.hasIndex();
      info.index_offset = header.index_offset;

      if (header.event_count == 0 && header.isCompressed())
      {
        recoverCompressedSegmentMeta(f, header,
                                     info.event_count,
                                     info.first_event_ns,
                                     info.last_event_ns);
      }

      _segments.push_back(std::move(info));
    }
    std::fclose(f);
  }

  _scanned = true;
  return true;
}

bool BinaryLogReader::passesFilter(const ReplayEvent& event) const
{
  // Time filter
  if (_config.from_ns && event.timestamp_ns < *_config.from_ns)
  {
    return false;
  }
  if (_config.to_ns && event.timestamp_ns > *_config.to_ns)
  {
    return false;
  }

  // Symbol filter
  if (!_config.symbols.empty())
  {
    uint32_t symbol_id = (event.type == EventType::Trade) ? event.trade.symbol_id
                                                          : event.book_header.symbol_id;
    if (_config.symbols.find(symbol_id) == _config.symbols.end())
    {
      return false;
    }
  }

  return true;
}

bool BinaryLogReader::readSegment(const std::filesystem::path& path, EventCallback& callback)
{
  BinaryLogIterator iter(path);
  if (!iter.isValid())
  {
    return false;
  }

  ++_stats.files_read;

  if (iter.header().isSorted())
  {
    // Segment is guaranteed monotonic — stream directly without buffering.
    ReplayEvent event;
    while (iter.next(event))
    {
      if (!passesFilter(event))
      {
        continue;
      }
      ++_stats.events_read;
      if (event.type == EventType::Trade)
      {
        ++_stats.trades_read;
      }
      else
      {
        ++_stats.book_updates_read;
      }
      if (!callback(event))
      {
        return false;
      }
    }
    return true;
  }

  // Legacy path: segment has no ordering guarantee — buffer, sort, deliver.
  std::vector<ReplayEvent> events;
  ReplayEvent event;
  while (iter.next(event))
  {
    if (passesFilter(event))
    {
      events.push_back(std::move(event));
    }
  }

  std::stable_sort(events.begin(), events.end(),
                   [](const ReplayEvent& a, const ReplayEvent& b)
                   { return a.timestamp_ns < b.timestamp_ns; });

  for (const auto& e : events)
  {
    ++_stats.events_read;
    if (e.type == EventType::Trade)
    {
      ++_stats.trades_read;
    }
    else
    {
      ++_stats.book_updates_read;
    }
    if (!callback(e))
    {
      return false;
    }
  }

  return true;
}

bool BinaryLogReader::readSegmentFrom(const SegmentInfo& segment, int64_t start_ts_ns,
                                      EventCallback& callback)
{
  BinaryLogIterator iter(segment.path);
  if (!iter.isValid())
  {
    return false;
  }

  ++_stats.files_read;

  // Use index if available to seek to starting position
  if (segment.has_index)
  {
    iter.loadIndex();
    if (iter.hasIndex())
    {
      iter.seekToTimestamp(start_ts_ns);
    }
  }

  ReplayEvent event;

  if (iter.header().isSorted())
  {
    while (iter.next(event))
    {
      if (event.timestamp_ns < start_ts_ns)
      {
        continue;
      }
      if (!passesFilter(event))
      {
        continue;
      }
      ++_stats.events_read;
      event.type == EventType::Trade ? ++_stats.trades_read : ++_stats.book_updates_read;
      if (!callback(event))
      {
        return false;
      }
    }
    return true;
  }

  // Legacy path: buffer, sort, deliver.
  std::vector<ReplayEvent> events;
  while (iter.next(event))
  {
    if (event.timestamp_ns < start_ts_ns)
    {
      continue;
    }
    if (passesFilter(event))
    {
      events.push_back(std::move(event));
    }
  }

  std::stable_sort(events.begin(), events.end(),
                   [](const ReplayEvent& a, const ReplayEvent& b)
                   { return a.timestamp_ns < b.timestamp_ns; });

  for (const auto& e : events)
  {
    ++_stats.events_read;
    e.type == EventType::Trade ? ++_stats.trades_read : ++_stats.book_updates_read;
    if (!callback(e))
    {
      return false;
    }
  }

  return true;
}

bool BinaryLogReader::forEach(EventCallback callback)
{
  if (!scanSegments())
  {
    return false;
  }

  for (const auto& segment : _segments)
  {
    if (!readSegment(segment.path, callback))
    {
      return false;
    }
  }

  return true;
}

bool BinaryLogReader::run(std::span<IAggregator* const> aggregators)
{
  // Empty span: do not even scan / decompress. This is the contract
  // that lets a `run([])` call cost effectively zero in the framework
  // tests that exercise the no-op path.
  if (aggregators.empty())
  {
    return true;
  }

  // Route through streamForEach, not forEach: for segments without
  // the Sorted flag (external writers like md_collector) forEach
  // buffers the entire segment into memory before dispatching, which
  // blew peak RSS to 4.29 GB on an 11.78M-event 467 MB Bitget tape.
  // streamForEach iterates BinaryLogIterator::next() directly, O(1)
  // memory per segment.
  const bool walked = streamForEach(
      [&aggregators](const ReplayEvent& ev)
      {
        for (auto* agg : aggregators)
        {
          if (agg != nullptr)
          {
            agg->onEvent(ev);
          }
        }
        return true;
      });

  for (auto* agg : aggregators)
  {
    if (agg != nullptr)
    {
      agg->finalize();
    }
  }

  return walked;
}

bool BinaryLogReader::streamForEach(EventCallback callback)
{
  if (!scanSegments())
  {
    return false;
  }

  for (const auto& segment : _segments)
  {
    if (!readSegmentStreaming(segment.path, callback))
    {
      return false;
    }
  }

  return true;
}

bool BinaryLogReader::streamForEachFrom(int64_t start_ts_ns, EventCallback callback)
{
  if (!scanSegments())
  {
    return false;
  }

  auto it = std::lower_bound(_segments.begin(), _segments.end(), start_ts_ns,
                             [](const SegmentInfo& seg, int64_t ts)
                             { return seg.last_event_ns < ts; });

  for (; it != _segments.end(); ++it)
  {
    if (!readSegmentStreamingFrom(*it, start_ts_ns, callback))
    {
      return false;
    }
    // After first segment, deliver remaining segments from beginning.
    start_ts_ns = 0;
  }

  return true;
}

bool BinaryLogReader::readSegmentStreaming(const std::filesystem::path& path,
                                           EventCallback& callback)
{
  BinaryLogIterator iter(path);
  if (!iter.isValid())
  {
    return false;
  }

  ++_stats.files_read;

  // Pure streaming, regardless of the segment's Sorted flag. Events
  // arrive in BinaryLogIterator order (writer order); for tapes
  // produced by external writers without the Sorted flag the caller
  // must tolerate writer-order arrival.
  ReplayEvent event;
  while (iter.next(event))
  {
    if (!passesFilter(event))
    {
      continue;
    }
    ++_stats.events_read;
    if (event.type == EventType::Trade)
    {
      ++_stats.trades_read;
    }
    else
    {
      ++_stats.book_updates_read;
    }
    if (!callback(event))
    {
      return false;
    }
  }
  return true;
}

bool BinaryLogReader::readSegmentStreamingFrom(const SegmentInfo& segment,
                                               int64_t start_ts_ns,
                                               EventCallback& callback)
{
  BinaryLogIterator iter(segment.path);
  if (!iter.isValid())
  {
    return false;
  }

  ++_stats.files_read;

  if (segment.has_index)
  {
    iter.loadIndex();
    if (iter.hasIndex())
    {
      iter.seekToTimestamp(start_ts_ns);
    }
  }

  ReplayEvent event;
  while (iter.next(event))
  {
    if (event.timestamp_ns < start_ts_ns)
    {
      continue;
    }
    if (!passesFilter(event))
    {
      continue;
    }
    ++_stats.events_read;
    event.type == EventType::Trade ? ++_stats.trades_read : ++_stats.book_updates_read;
    if (!callback(event))
    {
      return false;
    }
  }
  return true;
}

bool BinaryLogReader::forEachFrom(int64_t start_ts_ns, EventCallback callback)
{
  if (!scanSegments())
  {
    return false;
  }

  // Find first segment that may contain events >= start_ts_ns
  auto it = std::lower_bound(_segments.begin(), _segments.end(), start_ts_ns,
                             [](const SegmentInfo& seg, int64_t ts)
                             {
                               return seg.last_event_ns < ts;
                             });

  for (; it != _segments.end(); ++it)
  {
    if (!readSegmentFrom(*it, start_ts_ns, callback))
    {
      return false;
    }
    // After first segment, read remaining segments from beginning
    start_ts_ns = 0;
  }

  return true;
}

std::optional<std::pair<int64_t, int64_t>> BinaryLogReader::timeRange() const
{
  if (_segments.empty())
  {
    return std::nullopt;
  }

  int64_t min_ts = INT64_MAX;
  int64_t max_ts = INT64_MIN;

  for (const auto& segment : _segments)
  {
    if (segment.first_event_ns > 0)
    {
      min_ts = std::min(min_ts, segment.first_event_ns);
    }
    if (segment.last_event_ns > 0)
    {
      max_ts = std::max(max_ts, segment.last_event_ns);
    }
  }

  if (min_ts == INT64_MAX || max_ts == INT64_MIN)
  {
    return std::nullopt;
  }

  return std::make_pair(min_ts, max_ts);
}

ReaderStats BinaryLogReader::stats() const { return _stats; }

std::vector<std::filesystem::path> BinaryLogReader::segmentFiles() const
{
  std::vector<std::filesystem::path> paths;
  paths.reserve(_segments.size());
  for (const auto& seg : _segments)
  {
    paths.push_back(seg.path);
  }
  return paths;
}

const std::vector<SegmentInfo>& BinaryLogReader::segments() const { return _segments; }

DatasetSummary BinaryLogReader::inspect(const std::filesystem::path& data_dir)
{
  DatasetSummary summary;
  summary.data_dir = data_dir;

  namespace fs = std::filesystem;

  if (!fs::exists(data_dir))
  {
    return summary;
  }

  std::vector<std::filesystem::path> paths;
  for (const auto& entry : fs::directory_iterator(data_dir))
  {
    if (entry.is_regular_file() && entry.path().extension() == ".floxlog")
    {
      paths.push_back(entry.path());
    }
  }

  std::sort(paths.begin(), paths.end());

  for (const auto& path : paths)
  {
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f)
    {
      continue;
    }

    SegmentHeader header;
    if (std::fread(&header, sizeof(header), 1, f) == 1 && header.isValid())
    {
      ++summary.segment_count;

      int64_t first_ns = header.first_event_ns;
      int64_t last_ns = header.last_event_ns;
      uint32_t ev_count = header.event_count;

      if (ev_count == 0 && header.isCompressed())
      {
        recoverCompressedSegmentMeta(f, header, ev_count, first_ns, last_ns);
      }

      summary.total_events += ev_count;

      // Get file size
      std::fseek(f, 0, SEEK_END);
      summary.total_bytes += static_cast<uint64_t>(std::ftell(f));

      // Update time range
      if (first_ns > 0 && (summary.first_event_ns == 0 || first_ns < summary.first_event_ns))
      {
        summary.first_event_ns = first_ns;
      }
      if (last_ns > summary.last_event_ns)
      {
        summary.last_event_ns = last_ns;
      }

      // Index status
      if (header.hasIndex())
      {
        ++summary.segments_with_index;
      }
      else
      {
        ++summary.segments_without_index;
      }
    }
    std::fclose(f);
  }

  return summary;
}

DatasetSummary BinaryLogReader::inspectWithSymbols(const std::filesystem::path& data_dir)
{
  // First get basic info
  DatasetSummary summary = inspect(data_dir);

  // Then scan all events to collect symbols
  ReaderConfig config{.data_dir = data_dir, .verify_crc = false};  // Skip CRC for speed
  BinaryLogReader reader(std::move(config));

  reader.forEach([&summary](const ReplayEvent& event)
                 {
    uint32_t symbol_id = (event.type == EventType::Trade) ? event.trade.symbol_id
                                                          : event.book_header.symbol_id;
    summary.symbols.insert(symbol_id);
    return true; });

  return summary;
}

DatasetSummary BinaryLogReader::summary()
{
  scanSegments();

  DatasetSummary result;
  result.data_dir = _config.data_dir;

  for (const auto& segment : _segments)
  {
    ++result.segment_count;
    result.total_events += segment.event_count;

    // Update time range
    if (result.first_event_ns == 0 || segment.first_event_ns < result.first_event_ns)
    {
      result.first_event_ns = segment.first_event_ns;
    }
    if (segment.last_event_ns > result.last_event_ns)
    {
      result.last_event_ns = segment.last_event_ns;
    }

    // Index status
    if (segment.has_index)
    {
      ++result.segments_with_index;
    }
    else
    {
      ++result.segments_without_index;
    }

    // Get file size
    std::error_code ec;
    auto file_size = std::filesystem::file_size(segment.path, ec);
    if (!ec)
    {
      result.total_bytes += file_size;
    }
  }

  return result;
}

uint64_t BinaryLogReader::count()
{
  scanSegments();

  uint64_t total = 0;
  for (const auto& segment : _segments)
  {
    total += segment.event_count;
  }
  return total;
}

std::set<uint32_t> BinaryLogReader::availableSymbols()
{
  std::set<uint32_t> symbols;

  forEach([&symbols](const ReplayEvent& event)
          {
    uint32_t symbol_id = (event.type == EventType::Trade) ? event.trade.symbol_id
                                                          : event.book_header.symbol_id;
    symbols.insert(symbol_id);
    return true; });

  return symbols;
}

BinaryLogIterator::BinaryLogIterator(const std::filesystem::path& segment_path)
{
  _file = std::fopen(segment_path.string().c_str(), "rb");
  if (!_file)
  {
    return;
  }

  // Read and validate segment header
  if (std::fread(&_header, sizeof(_header), 1, _file) != 1 || !_header.isValid())
  {
    std::fclose(_file);
    _file = nullptr;
    return;
  }

  _payload_buffer.reserve(64 * 1024);  // 64KB initial buffer
}

BinaryLogIterator::~BinaryLogIterator()
{
  if (_file)
  {
    std::fclose(_file);
  }
}

bool BinaryLogIterator::next(ReplayEvent& out)
{
  if (!_file)
  {
    return false;
  }

  if (isCompressed())
  {
    return nextCompressed(out);
  }
  else
  {
    return nextUncompressed(out);
  }
}

bool BinaryLogIterator::nextUncompressed(ReplayEvent& out)
{
  // Check if we've hit the index (stop reading events)
  if (_header.hasIndex())
  {
    long current_pos = std::ftell(_file);
    if (current_pos >= static_cast<long>(_header.index_offset))
    {
      return false;  // Reached index, no more events
    }
  }

  // Read frame header
  FrameHeader frame;
  if (std::fread(&frame, sizeof(frame), 1, _file) != 1)
  {
    return false;  // EOF or error
  }

  // Sanity check
  if (frame.size > 10 * 1024 * 1024)
  {  // Max 10MB per frame
    return false;
  }

  // Read payload
  _payload_buffer.resize(frame.size);
  if (std::fread(_payload_buffer.data(), 1, frame.size, _file) != frame.size)
  {
    return false;
  }

  // Verify CRC
  uint32_t computed_crc = Crc32::compute(_payload_buffer);
  if (computed_crc != frame.crc32)
  {
    return false;  // CRC mismatch
  }

  return parseFrame(static_cast<EventType>(frame.type), _payload_buffer.data(), frame.size, out);
}

bool BinaryLogIterator::nextCompressed(ReplayEvent& out)
{
  // If we have events remaining in current block, read from it
  while (_block_events_remaining == 0)
  {
    // Need to load next block
    if (!loadNextBlock())
    {
      return false;  // No more blocks
    }
  }

  // Parse next frame from decompressed block data
  if (_block_offset >= _block_data.size())
  {
    return false;  // Block exhausted (shouldn't happen)
  }

  // Read frame header from block
  if (_block_offset + sizeof(FrameHeader) > _block_data.size())
  {
    return false;
  }

  FrameHeader frame;
  std::memcpy(&frame, _block_data.data() + _block_offset, sizeof(frame));
  _block_offset += sizeof(FrameHeader);

  // Sanity check
  if (frame.size > 10 * 1024 * 1024 || _block_offset + frame.size > _block_data.size())
  {
    return false;
  }

  // Verify CRC
  uint32_t computed_crc = Crc32::compute(_block_data.data() + _block_offset, frame.size);
  if (computed_crc != frame.crc32)
  {
    return false;  // CRC mismatch
  }

  bool ok = parseFrame(static_cast<EventType>(frame.type), _block_data.data() + _block_offset,
                       frame.size, out);
  _block_offset += frame.size;
  --_block_events_remaining;

  return ok;
}

bool BinaryLogIterator::loadNextBlock()
{
  // Check if we've hit the index (stop reading)
  if (_header.hasIndex())
  {
    long current_pos = std::ftell(_file);
    if (current_pos >= static_cast<long>(_header.index_offset))
    {
      return false;
    }
  }

  // Read compressed block header
  CompressedBlockHeader block_header;
  if (std::fread(&block_header, sizeof(block_header), 1, _file) != 1)
  {
    return false;  // EOF or error
  }

  if (!block_header.isValid())
  {
    return false;  // Invalid block
  }

  // Sanity check sizes — the 100 MB ceiling is a coarse upper bound for
  // sane blocks. Below it we also bound `compressed_size` by the bytes
  // remaining in the segment region, so a live-writing tape whose tail
  // header was flushed before its payload (or before any payload was
  // ever written) does not allocate up to 100 MB on garbage.
  if (block_header.compressed_size > 100 * 1024 * 1024 ||
      block_header.original_size > 100 * 1024 * 1024)
  {
    return false;
  }

  const long payload_pos = std::ftell(_file);
  long stop_at;
  if (_header.hasIndex() && _header.index_offset > 0)
  {
    stop_at = static_cast<long>(_header.index_offset);
  }
  else
  {
    if (std::fseek(_file, 0, SEEK_END) != 0)
    {
      return false;
    }
    stop_at = std::ftell(_file);
    if (stop_at < 0 || std::fseek(_file, payload_pos, SEEK_SET) != 0)
    {
      return false;
    }
  }
  if (payload_pos < 0 ||
      static_cast<long>(block_header.compressed_size) > stop_at - payload_pos)
  {
    return false;
  }

  // Read compressed data
  _payload_buffer.resize(block_header.compressed_size);
  if (std::fread(_payload_buffer.data(), 1, block_header.compressed_size, _file) !=
      block_header.compressed_size)
  {
    return false;
  }

  // Decompress
  _block_data.resize(block_header.original_size);
  size_t decompressed_size =
      Compressor::decompress(_header.compressionType(), _payload_buffer.data(),
                             block_header.compressed_size, _block_data.data(),
                             block_header.original_size);

  if (decompressed_size != block_header.original_size)
  {
    return false;  // Decompression failed
  }

  _block_offset = 0;
  _block_events_remaining = block_header.event_count;

  return true;
}

bool BinaryLogIterator::parseFrame(EventType type, const std::byte* data, size_t size,
                                   ReplayEvent& out)
{
  out.type = type;

  if (type == EventType::Trade)
  {
    if (size < sizeof(TradeRecord))
    {
      return false;
    }
    std::memcpy(&out.trade, data, sizeof(TradeRecord));
    out.timestamp_ns = out.trade.exchange_ts_ns;
    out.bids.clear();
    out.asks.clear();
    return true;
  }
  else if (type == EventType::BookSnapshot || type == EventType::BookDelta)
  {
    if (size < sizeof(BookRecordHeader))
    {
      return false;
    }
    std::memcpy(&out.book_header, data, sizeof(BookRecordHeader));
    out.timestamp_ns = out.book_header.exchange_ts_ns;

    const size_t levels_offset = sizeof(BookRecordHeader);
    const size_t bid_bytes = out.book_header.bid_count * sizeof(BookLevel);
    const size_t ask_bytes = out.book_header.ask_count * sizeof(BookLevel);

    if (size < levels_offset + bid_bytes + ask_bytes)
    {
      return false;
    }

    out.bids.resize(out.book_header.bid_count);
    out.asks.resize(out.book_header.ask_count);

    if (out.book_header.bid_count > 0)
    {
      std::memcpy(out.bids.data(), data + levels_offset, bid_bytes);
    }
    if (out.book_header.ask_count > 0)
    {
      std::memcpy(out.asks.data(), data + levels_offset + bid_bytes, ask_bytes);
    }
    return true;
  }

  return false;  // Unknown type
}

bool BinaryLogIterator::loadIndex()
{
  if (!_file || !_header.hasIndex())
  {
    return false;
  }

  // Seek to index position
  if (std::fseek(_file, static_cast<long>(_header.index_offset), SEEK_SET) != 0)
  {
    return false;
  }

  // Read index header
  SegmentIndexHeader idx_header;
  if (std::fread(&idx_header, sizeof(idx_header), 1, _file) != 1)
  {
    // Seek back to data start
    std::fseek(_file, sizeof(SegmentHeader), SEEK_SET);
    return false;
  }

  if (!idx_header.isValid() || idx_header.entry_count == 0)
  {
    std::fseek(_file, sizeof(SegmentHeader), SEEK_SET);
    return false;
  }

  // Read index entries
  _index_entries.resize(idx_header.entry_count);
  if (std::fread(_index_entries.data(), sizeof(IndexEntry), idx_header.entry_count, _file) !=
      idx_header.entry_count)
  {
    _index_entries.clear();
    std::fseek(_file, sizeof(SegmentHeader), SEEK_SET);
    return false;
  }

  // Verify CRC
  uint32_t computed_crc =
      Crc32::compute(_index_entries.data(), _index_entries.size() * sizeof(IndexEntry));
  if (computed_crc != idx_header.crc32)
  {
    _index_entries.clear();
    std::fseek(_file, sizeof(SegmentHeader), SEEK_SET);
    return false;
  }

  // Seek back to data start
  std::fseek(_file, sizeof(SegmentHeader), SEEK_SET);
  return true;
}

bool BinaryLogIterator::seekToTimestamp(int64_t target_ts_ns)
{
  if (!_file || _index_entries.empty())
  {
    return false;
  }

  // Binary search for the largest entry with timestamp <= target
  auto it = std::upper_bound(_index_entries.begin(), _index_entries.end(), target_ts_ns,
                             [](int64_t ts, const IndexEntry& entry)
                             {
                               return ts < entry.timestamp_ns;
                             });

  // upper_bound gives first entry > target, we want the one before
  if (it == _index_entries.begin())
  {
    // All entries are > target, start from beginning
    std::fseek(_file, sizeof(SegmentHeader), SEEK_SET);
    return true;
  }

  --it;  // Now points to largest entry <= target

  // Seek to that position
  if (std::fseek(_file, static_cast<long>(it->file_offset), SEEK_SET) != 0)
  {
    return false;
  }

  return true;
}

}  // namespace flox::replay
