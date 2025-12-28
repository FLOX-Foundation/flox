/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/writers/binary_log_writer.h"
#include "flox/replay/ops/compression.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>

namespace flox::replay
{

BinaryLogWriter::BinaryLogWriter(WriterConfig config) : _config(std::move(config))
{
  _buffer.reserve(_config.buffer_size);
  std::filesystem::create_directories(_config.output_dir);

  // Initialize metadata from config if provided
  if (_config.metadata)
  {
    _metadata = *_config.metadata;
  }
}

BinaryLogWriter::~BinaryLogWriter() { close(); }

BinaryLogWriter::BinaryLogWriter(BinaryLogWriter&& other) noexcept
    : _config(std::move(other._config)),
      _stats(other._stats),
      _file(other._file),
      _current_path(std::move(other._current_path)),
      _buffer(std::move(other._buffer)),
      _segment_header(other._segment_header),
      _segment_bytes(other._segment_bytes),
      _header_written(other._header_written),
      _index_entries(std::move(other._index_entries)),
      _events_since_last_index(other._events_since_last_index),
      _block_buffer(std::move(other._block_buffer)),
      _compress_buffer(std::move(other._compress_buffer)),
      _block_event_count(other._block_event_count),
      _block_first_timestamp(other._block_first_timestamp)
{
  other._file = nullptr;
}

BinaryLogWriter& BinaryLogWriter::operator=(BinaryLogWriter&& other) noexcept
{
  if (this != &other)
  {
    close();
    _config = std::move(other._config);
    _stats = other._stats;
    _file = other._file;
    _current_path = std::move(other._current_path);
    _buffer = std::move(other._buffer);
    _segment_header = other._segment_header;
    _segment_bytes = other._segment_bytes;
    _header_written = other._header_written;
    _index_entries = std::move(other._index_entries);
    _events_since_last_index = other._events_since_last_index;
    _block_buffer = std::move(other._block_buffer);
    _compress_buffer = std::move(other._compress_buffer);
    _block_event_count = other._block_event_count;
    _block_first_timestamp = other._block_first_timestamp;
    other._file = nullptr;
  }
  return *this;
}

std::filesystem::path BinaryLogWriter::generateSegmentPath()
{
  ++_segment_number;

  // First segment: use output_filename if provided
  if (_segment_number == 1 && !_config.output_filename.empty())
  {
    return _config.output_dir / _config.output_filename;
  }

  // Use rotation callback if provided
  if (_config.rotation_callback)
  {
    return _config.rotation_callback(_config.rotation_user_data, _config.output_dir,
                                     _segment_number);
  }

  // Default: timestamp-based naming
  using namespace std::chrono;
  auto now = system_clock::now();
  auto ns = duration_cast<nanoseconds>(now.time_since_epoch()).count();
  return _config.output_dir / (std::to_string(ns) + ".floxlog");
}

bool BinaryLogWriter::ensureOpen()
{
  if (_file)
  {
    return true;
  }

  _current_path = generateSegmentPath();
  _file = std::fopen(_current_path.string().c_str(), "wb");
  if (!_file)
  {
    return false;
  }

  // Initialize segment header
  _segment_header = SegmentHeader{};
  _segment_header.exchange_id = _config.exchange_id;

  auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
  _segment_header.created_ns = now_ns;

  // Set compression info in header
  if (isCompressed())
  {
    _segment_header.flags |= SegmentFlags::Compressed;
    _segment_header.compression = static_cast<uint8_t>(_config.compression);
  }

  // Write placeholder header (will be updated on close)
  if (std::fwrite(&_segment_header, sizeof(_segment_header), 1, _file) != 1)
  {
    std::fclose(_file);
    _file = nullptr;
    return false;
  }

  _segment_bytes = sizeof(SegmentHeader);
  _header_written = true;
  ++_stats.segments_created;

  // Reset index state for new segment
  _index_entries.clear();
  _events_since_last_index = 0;

  // Reset block state for new segment
  _block_buffer.clear();
  _block_event_count = 0;
  _block_first_timestamp = 0;

  return true;
}

bool BinaryLogWriter::maybeRotate(size_t needed_bytes)
{
  if (!_file)
  {
    return true;
  }

  if (_segment_bytes + needed_bytes > _config.max_segment_bytes)
  {
    closeInternal();
    return ensureOpen();
  }
  return true;
}

bool BinaryLogWriter::writeFrame(EventType type, const void* payload, size_t size)
{
  std::lock_guard lock(_mutex);

  const size_t frame_size = sizeof(FrameHeader) + size;
  if (!maybeRotate(frame_size))
  {
    return false;
  }
  if (!ensureOpen())
  {
    return false;
  }

  // Record position before writing for index
  uint64_t frame_offset = _segment_bytes;

  FrameHeader header{};
  header.size = static_cast<uint32_t>(size);
  header.crc32 = Crc32::compute(payload, size);
  header.type = static_cast<uint8_t>(type);
  header.rec_version = 1;

  if (std::fwrite(&header, sizeof(header), 1, _file) != 1)
  {
    return false;
  }
  if (std::fwrite(payload, 1, size, _file) != size)
  {
    return false;
  }

  _segment_bytes += frame_size;
  _stats.bytes_written += frame_size;
  ++_stats.events_written;

  // Update segment header timestamps
  int64_t event_ts = 0;
  if (type == EventType::Trade)
  {
    event_ts = reinterpret_cast<const TradeRecord*>(payload)->exchange_ts_ns;
  }
  else
  {
    event_ts = reinterpret_cast<const BookRecordHeader*>(payload)->exchange_ts_ns;
  }

  if (_segment_header.first_event_ns == 0)
  {
    _segment_header.first_event_ns = event_ts;
  }
  _segment_header.last_event_ns = event_ts;
  ++_segment_header.event_count;

  // Build index: add entry at interval or for first event
  if (_config.create_index)
  {
    if (_index_entries.empty() || _events_since_last_index >= _config.index_interval)
    {
      _index_entries.push_back(IndexEntry{.timestamp_ns = event_ts, .file_offset = frame_offset});
      _events_since_last_index = 0;
    }
    ++_events_since_last_index;
  }

  return true;
}

bool BinaryLogWriter::writeFrameToBlock(EventType type, const void* payload, size_t size,
                                        int64_t timestamp)
{
  // Build frame in block buffer (without writing to file yet)
  FrameHeader header{};
  header.size = static_cast<uint32_t>(size);
  header.crc32 = Crc32::compute(payload, size);
  header.type = static_cast<uint8_t>(type);
  header.rec_version = 1;

  // Reserve space and append frame header + payload
  size_t old_size = _block_buffer.size();
  _block_buffer.resize(old_size + sizeof(FrameHeader) + size);
  std::memcpy(_block_buffer.data() + old_size, &header, sizeof(header));
  std::memcpy(_block_buffer.data() + old_size + sizeof(header), payload, size);

  // Track first timestamp in block (for index)
  if (_block_event_count == 0)
  {
    _block_first_timestamp = timestamp;
  }
  ++_block_event_count;

  // Update segment header timestamps
  if (_segment_header.first_event_ns == 0)
  {
    _segment_header.first_event_ns = timestamp;
  }
  _segment_header.last_event_ns = timestamp;
  ++_segment_header.event_count;
  ++_stats.events_written;

  // Flush block when it reaches index_interval events
  if (_block_event_count >= _config.index_interval)
  {
    return flushBlock();
  }

  return true;
}

bool BinaryLogWriter::flushBlock()
{
  if (_block_buffer.empty() || _block_event_count == 0)
  {
    return true;
  }
  if (!_file)
  {
    return false;
  }

  // Record position before writing block (for index)
  uint64_t block_offset = _segment_bytes;

  // Compress the block
  size_t max_compressed = Compressor::maxCompressedSize(_config.compression, _block_buffer.size());
  if (max_compressed == 0)
  {
    return false;  // Compression not available
  }

  _compress_buffer.resize(max_compressed);
  size_t compressed_size =
      Compressor::compress(_config.compression, _block_buffer.data(), _block_buffer.size(),
                           _compress_buffer.data(), max_compressed);

  if (compressed_size == 0)
  {
    return false;  // Compression failed
  }

  // Build and write block header
  CompressedBlockHeader block_header{};
  block_header.compressed_size = static_cast<uint32_t>(compressed_size);
  block_header.original_size = static_cast<uint32_t>(_block_buffer.size());
  block_header.event_count = _block_event_count;

  if (std::fwrite(&block_header, sizeof(block_header), 1, _file) != 1)
  {
    return false;
  }
  if (std::fwrite(_compress_buffer.data(), 1, compressed_size, _file) != compressed_size)
  {
    return false;
  }

  size_t total_written = sizeof(CompressedBlockHeader) + compressed_size;
  _segment_bytes += total_written;
  _stats.bytes_written += total_written;
  _stats.uncompressed_bytes += _block_buffer.size();
  _stats.compressed_bytes += compressed_size;
  ++_stats.blocks_written;

  // Add index entry for this block (points to block header, not individual frames)
  if (_config.create_index)
  {
    _index_entries.push_back(
        IndexEntry{.timestamp_ns = _block_first_timestamp, .file_offset = block_offset});
  }

  // Reset block state
  _block_buffer.clear();
  _block_event_count = 0;
  _block_first_timestamp = 0;

  return true;
}

bool BinaryLogWriter::writeTrade(const TradeRecord& trade)
{
  std::lock_guard lock(_mutex);

  if (!ensureOpen())
  {
    return false;
  }

  bool ok;
  if (isCompressed())
  {
    ok = writeFrameToBlock(EventType::Trade, &trade, sizeof(trade), trade.exchange_ts_ns);
  }
  else
  {
    // Uncompressed path - use existing writeFrame (but unlock first since writeFrame locks)
    // Need to refactor: extract unlocked version
    const size_t frame_size = sizeof(FrameHeader) + sizeof(trade);
    if (!maybeRotate(frame_size))
    {
      return false;
    }

    uint64_t frame_offset = _segment_bytes;

    FrameHeader header{};
    header.size = static_cast<uint32_t>(sizeof(trade));
    header.crc32 = Crc32::compute(&trade, sizeof(trade));
    header.type = static_cast<uint8_t>(EventType::Trade);
    header.rec_version = 1;

    if (std::fwrite(&header, sizeof(header), 1, _file) != 1)
    {
      return false;
    }
    if (std::fwrite(&trade, sizeof(trade), 1, _file) != 1)
    {
      return false;
    }

    _segment_bytes += frame_size;
    _stats.bytes_written += frame_size;
    ++_stats.events_written;

    if (_segment_header.first_event_ns == 0)
    {
      _segment_header.first_event_ns = trade.exchange_ts_ns;
    }
    _segment_header.last_event_ns = trade.exchange_ts_ns;
    ++_segment_header.event_count;

    if (_config.create_index)
    {
      if (_index_entries.empty() || _events_since_last_index >= _config.index_interval)
      {
        _index_entries.push_back(
            IndexEntry{.timestamp_ns = trade.exchange_ts_ns, .file_offset = frame_offset});
        _events_since_last_index = 0;
      }
      ++_events_since_last_index;
    }
    ok = true;
  }

  if (ok)
  {
    ++_stats.trades_written;
  }
  return ok;
}

bool BinaryLogWriter::writeBook(const BookRecordHeader& hdr, std::span<const BookLevel> bids,
                                std::span<const BookLevel> asks)
{
  std::lock_guard lock(_mutex);

  if (!ensureOpen())
  {
    return false;
  }

  // Build the complete payload
  const size_t payload_size = sizeof(BookRecordHeader) + bids.size_bytes() + asks.size_bytes();

  std::vector<std::byte> payload(payload_size);
  std::memcpy(payload.data(), &hdr, sizeof(hdr));
  if (!bids.empty())
  {
    std::memcpy(payload.data() + sizeof(hdr), bids.data(), bids.size_bytes());
  }
  if (!asks.empty())
  {
    std::memcpy(payload.data() + sizeof(hdr) + bids.size_bytes(), asks.data(), asks.size_bytes());
  }

  EventType type = (hdr.type == 0) ? EventType::BookSnapshot : EventType::BookDelta;

  bool ok;
  if (isCompressed())
  {
    ok = writeFrameToBlock(type, payload.data(), payload_size, hdr.exchange_ts_ns);
  }
  else
  {
    // Uncompressed path
    const size_t frame_size = sizeof(FrameHeader) + payload_size;
    if (!maybeRotate(frame_size))
    {
      return false;
    }

    uint64_t frame_offset = _segment_bytes;

    FrameHeader frame_hdr{};
    frame_hdr.size = static_cast<uint32_t>(payload_size);
    frame_hdr.crc32 = Crc32::compute(payload.data(), payload_size);
    frame_hdr.type = static_cast<uint8_t>(type);
    frame_hdr.rec_version = 1;

    if (std::fwrite(&frame_hdr, sizeof(frame_hdr), 1, _file) != 1)
    {
      return false;
    }
    if (std::fwrite(payload.data(), 1, payload_size, _file) != payload_size)
    {
      return false;
    }

    _segment_bytes += frame_size;
    _stats.bytes_written += frame_size;
    ++_stats.events_written;

    if (_segment_header.first_event_ns == 0)
    {
      _segment_header.first_event_ns = hdr.exchange_ts_ns;
    }
    _segment_header.last_event_ns = hdr.exchange_ts_ns;
    ++_segment_header.event_count;

    if (_config.create_index)
    {
      if (_index_entries.empty() || _events_since_last_index >= _config.index_interval)
      {
        _index_entries.push_back(
            IndexEntry{.timestamp_ns = hdr.exchange_ts_ns, .file_offset = frame_offset});
        _events_since_last_index = 0;
      }
      ++_events_since_last_index;
    }
    ok = true;
  }

  if (ok)
  {
    ++_stats.book_updates_written;
  }
  return ok;
}

void BinaryLogWriter::flush()
{
  std::lock_guard lock(_mutex);
  if (_file)
  {
    std::fflush(_file);
  }
}

void BinaryLogWriter::updateSegmentHeader()
{
  if (!_file || !_header_written)
  {
    return;
  }

  // Seek to beginning and rewrite header with updated stats
  long current_pos = std::ftell(_file);
  std::fseek(_file, 0, SEEK_SET);
  std::fwrite(&_segment_header, sizeof(_segment_header), 1, _file);
  std::fseek(_file, current_pos, SEEK_SET);
}

void BinaryLogWriter::writeIndex()
{
  if (!_file || !_config.create_index || _index_entries.empty())
  {
    return;
  }

  // Record where index starts
  uint64_t index_offset = _segment_bytes;

  // Build index header
  SegmentIndexHeader idx_header{};
  idx_header.interval = _config.index_interval;
  idx_header.entry_count = static_cast<uint32_t>(_index_entries.size());
  idx_header.first_ts_ns = _index_entries.front().timestamp_ns;
  idx_header.last_ts_ns = _index_entries.back().timestamp_ns;

  // Compute CRC of index entries
  idx_header.crc32 =
      Crc32::compute(_index_entries.data(), _index_entries.size() * sizeof(IndexEntry));

  // Write index header
  if (std::fwrite(&idx_header, sizeof(idx_header), 1, _file) != 1)
  {
    return;
  }

  // Write index entries
  if (std::fwrite(_index_entries.data(), sizeof(IndexEntry), _index_entries.size(), _file) !=
      _index_entries.size())
  {
    return;
  }

  _segment_bytes += sizeof(SegmentIndexHeader) + _index_entries.size() * sizeof(IndexEntry);

  // Update segment header with index info
  _segment_header.index_offset = index_offset;
  _segment_header.flags |= SegmentFlags::HasIndex;
}

void BinaryLogWriter::closeInternal()
{
  if (_file)
  {
    // Flush any remaining events in the block buffer (for compressed mode)
    if (isCompressed() && _block_event_count > 0)
    {
      flushBlock();
    }

    // Write index before closing (must be done before updateSegmentHeader)
    writeIndex();
    updateSegmentHeader();
    if (_config.sync_on_rotate)
    {
      std::fflush(_file);
    }
    std::fclose(_file);
    _file = nullptr;
    _header_written = false;
    _segment_bytes = 0;
  }
}

void BinaryLogWriter::close()
{
  std::lock_guard lock(_mutex);
  closeInternal();

  // Write metadata.json if metadata is set
  if (_metadata)
  {
    // Update recording_end timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &time_t_now);
#else
    gmtime_r(&time_t_now, &tm_utc);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_utc);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch())
                  .count() %
              1000;
    char iso_buf[80];
    std::snprintf(iso_buf, sizeof(iso_buf), "%s.%03dZ", buf, static_cast<int>(ms));
    _metadata->recording_end = iso_buf;

    _metadata->save(RecordingMetadata::metadataPath(_config.output_dir));
  }
}

WriterStats BinaryLogWriter::stats() const
{
  std::lock_guard lock(_mutex);
  return _stats;
}

std::filesystem::path BinaryLogWriter::currentSegmentPath() const
{
  std::lock_guard lock(_mutex);
  return _current_path;
}

void BinaryLogWriter::setMetadata(const RecordingMetadata& meta)
{
  std::lock_guard lock(_mutex);
  _metadata = meta;
}

void BinaryLogWriter::addSymbol(const SymbolInfo& symbol)
{
  std::lock_guard lock(_mutex);
  if (!_metadata)
  {
    _metadata = RecordingMetadata{};
  }
  _metadata->symbols.push_back(symbol);
}

void BinaryLogWriter::setHasTrades(bool v)
{
  std::lock_guard lock(_mutex);
  if (!_metadata)
  {
    _metadata = RecordingMetadata{};
  }
  _metadata->has_trades = v;
}

void BinaryLogWriter::setHasBookSnapshots(bool v)
{
  std::lock_guard lock(_mutex);
  if (!_metadata)
  {
    _metadata = RecordingMetadata{};
  }
  _metadata->has_book_snapshots = v;
}

void BinaryLogWriter::setHasBookDeltas(bool v)
{
  std::lock_guard lock(_mutex);
  if (!_metadata)
  {
    _metadata = RecordingMetadata{};
  }
  _metadata->has_book_deltas = v;
}

}  // namespace flox::replay
