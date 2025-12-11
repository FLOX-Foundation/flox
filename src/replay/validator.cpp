/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/ops/validator.h"

#include <algorithm>
#include <cstring>

namespace flox::replay
{

SegmentValidator::SegmentValidator(ValidatorConfig config) : _config(std::move(config)) {}

SegmentValidationResult SegmentValidator::validate(const std::filesystem::path& segment_path)
{
  ProgressCallback no_progress = nullptr;
  return validate(segment_path, no_progress);
}

SegmentValidationResult SegmentValidator::validate(const std::filesystem::path& segment_path,
                                                   ProgressCallback progress)
{
  SegmentValidationResult result;
  result.path = segment_path;

  // Check file exists
  if (!std::filesystem::exists(segment_path))
  {
    addIssue(result, IssueType::FileNotFound, IssueSeverity::Critical,
             "File not found: " + segment_path.string());
    result.valid = false;
    return result;
  }

  std::FILE* file = std::fopen(segment_path.string().c_str(), "rb");
  if (!file)
  {
    addIssue(result, IssueType::FileReadError, IssueSeverity::Critical,
             "Cannot open file for reading");
    result.valid = false;
    return result;
  }

  // Get file size
  std::fseek(file, 0, SEEK_END);
  uint64_t file_size = static_cast<uint64_t>(std::ftell(file));
  std::fseek(file, 0, SEEK_SET);

  // Validate header
  if (!validateHeader(file, result))
  {
    std::fclose(file);
    result.valid = false;
    return result;
  }

  // Validate events
  if (_config.scan_all_events)
  {
    bool events_ok;
    if (result.is_compressed)
    {
      events_ok = validateEventsCompressed(file, result, progress);
    }
    else
    {
      events_ok = validateEventsUncompressed(file, result, progress);
    }

    if (!events_ok && _config.stop_on_first_error)
    {
      std::fclose(file);
      result.valid = false;
      return result;
    }
  }

  // Validate index
  if (_config.verify_index && result.has_index)
  {
    validateIndex(file, result);
  }

  std::fclose(file);

  // Check header vs actual data
  if (result.reported_event_count != result.actual_event_count)
  {
    addIssue(result, IssueType::EventCountMismatch, IssueSeverity::Warning,
             "Header reports " + std::to_string(result.reported_event_count) + " events, found " +
                 std::to_string(result.actual_event_count));
  }

  // Determine overall validity
  result.valid = !result.hasCritical();

  return result;
}

bool SegmentValidator::validateHeader(std::FILE* file, SegmentValidationResult& result)
{
  SegmentHeader header;
  if (std::fread(&header, sizeof(header), 1, file) != 1)
  {
    addIssue(result, IssueType::HeaderCorrupted, IssueSeverity::Critical,
             "Cannot read segment header");
    return false;
  }

  // Validate magic
  if (header.magic != kMagic)
  {
    addIssue(result, IssueType::InvalidMagic, IssueSeverity::Critical,
             "Invalid magic number: expected 0x" + std::to_string(kMagic) + ", got 0x" +
                 std::to_string(header.magic));
    return false;
  }

  // Validate version
  if (header.version != kFormatVersion)
  {
    addIssue(result, IssueType::InvalidVersion, IssueSeverity::Critical,
             "Unsupported format version: " + std::to_string(header.version));
    return false;
  }

  result.header_valid = true;
  result.reported_event_count = header.event_count;
  result.reported_first_ts = header.first_event_ns;
  result.reported_last_ts = header.last_event_ns;
  result.is_compressed = header.isCompressed();
  result.compression_type = header.compressionType();
  result.has_index = header.hasIndex();

  return true;
}

bool SegmentValidator::validateEventsUncompressed(std::FILE* file, SegmentValidationResult& result,
                                                  ProgressCallback& progress)
{
  int64_t last_timestamp = 0;
  uint64_t event_index = 0;

  // Get file size for progress reporting
  long start_pos = std::ftell(file);
  std::fseek(file, 0, SEEK_END);
  uint64_t file_size = static_cast<uint64_t>(std::ftell(file));
  std::fseek(file, start_pos, SEEK_SET);

  // Calculate end position (before index if present)
  uint64_t end_pos = file_size;
  if (result.has_index)
  {
    std::fseek(file, 0, SEEK_SET);
    SegmentHeader header;
    std::fread(&header, sizeof(header), 1, file);
    end_pos = header.index_offset;
    std::fseek(file, sizeof(SegmentHeader), SEEK_SET);
  }

  while (true)
  {
    uint64_t current_pos = static_cast<uint64_t>(std::ftell(file));

    // Check if we've reached the index
    if (current_pos >= end_pos)
    {
      break;
    }

    // Report progress
    if (progress)
    {
      progress(current_pos, file_size);
    }

    // Read frame header
    FrameHeader frame;
    if (std::fread(&frame, sizeof(frame), 1, file) != 1)
    {
      if (std::feof(file))
      {
        break;  // Normal EOF
      }
      addIssue(result, IssueType::FrameTruncated, IssueSeverity::Error,
               "Truncated frame header at offset " + std::to_string(current_pos), current_pos);
      return false;
    }

    // Sanity check frame size
    if (frame.size > 10 * 1024 * 1024)
    {
      addIssue(result, IssueType::FrameSizeTooLarge, IssueSeverity::Error,
               "Frame size too large: " + std::to_string(frame.size) + " bytes", current_pos);
      return false;
    }

    // Read payload
    std::vector<std::byte> payload(frame.size);
    if (std::fread(payload.data(), 1, frame.size, file) != frame.size)
    {
      addIssue(result, IssueType::FrameTruncated, IssueSeverity::Error,
               "Truncated frame payload at offset " + std::to_string(current_pos), current_pos);
      return false;
    }

    // Verify CRC
    if (_config.verify_crc)
    {
      uint32_t computed_crc = Crc32::compute(payload.data(), payload.size());
      if (computed_crc != frame.crc32)
      {
        addIssue(result, IssueType::FrameCrcMismatch, IssueSeverity::Error,
                 "CRC mismatch at event " + std::to_string(event_index), current_pos);
        ++result.crc_errors;

        if (_config.stop_on_first_error)
        {
          return false;
        }
      }
    }

    // Parse event to check timestamp
    int64_t event_ts = 0;
    EventType type = static_cast<EventType>(frame.type);

    if (type == EventType::Trade)
    {
      if (frame.size >= sizeof(TradeRecord))
      {
        TradeRecord trade;
        std::memcpy(&trade, payload.data(), sizeof(trade));
        event_ts = trade.exchange_ts_ns;
        ++result.trades_found;
      }
    }
    else if (type == EventType::BookSnapshot || type == EventType::BookDelta)
    {
      if (frame.size >= sizeof(BookRecordHeader))
      {
        BookRecordHeader book;
        std::memcpy(&book, payload.data(), sizeof(book));
        event_ts = book.exchange_ts_ns;
        ++result.book_updates_found;
      }
    }
    else
    {
      addIssue(result, IssueType::FrameTypeUnknown, IssueSeverity::Warning,
               "Unknown frame type: " + std::to_string(frame.type), current_pos);
    }

    // Check timestamp ordering
    if (_config.verify_timestamps && event_ts > 0)
    {
      if (result.actual_event_count == 0)
      {
        result.actual_first_ts = event_ts;
      }
      result.actual_last_ts = event_ts;

      if (last_timestamp > 0)
      {
        if (event_ts < last_timestamp)
        {
          addIssue(result, IssueType::TimestampOutOfOrder, IssueSeverity::Warning,
                   "Timestamp out of order at event " + std::to_string(event_index), current_pos);
          ++result.timestamp_anomalies;
        }
        else if (event_ts - last_timestamp > _config.max_timestamp_jump_ns)
        {
          addIssue(result, IssueType::TimestampJumpTooLarge, IssueSeverity::Warning,
                   "Large timestamp jump at event " + std::to_string(event_index), current_pos);
          ++result.timestamp_anomalies;
        }
      }
      last_timestamp = event_ts;
    }

    ++result.actual_event_count;
    ++event_index;
    result.bytes_scanned = current_pos + sizeof(FrameHeader) + frame.size - sizeof(SegmentHeader);
  }

  return true;
}

bool SegmentValidator::validateEventsCompressed(std::FILE* file, SegmentValidationResult& result,
                                                ProgressCallback& progress)
{
  int64_t last_timestamp = 0;

  // Get file size
  long start_pos = std::ftell(file);
  std::fseek(file, 0, SEEK_END);
  uint64_t file_size = static_cast<uint64_t>(std::ftell(file));
  std::fseek(file, start_pos, SEEK_SET);

  // Calculate end position
  uint64_t end_pos = file_size;
  if (result.has_index)
  {
    std::fseek(file, 0, SEEK_SET);
    SegmentHeader header;
    std::fread(&header, sizeof(header), 1, file);
    end_pos = header.index_offset;
    std::fseek(file, sizeof(SegmentHeader), SEEK_SET);
  }

  std::vector<std::byte> compressed_buffer;
  std::vector<std::byte> decompressed_buffer;

  while (true)
  {
    uint64_t current_pos = static_cast<uint64_t>(std::ftell(file));

    if (current_pos >= end_pos)
    {
      break;
    }

    if (progress)
    {
      progress(current_pos, file_size);
    }

    // Read block header
    CompressedBlockHeader block_header;
    if (std::fread(&block_header, sizeof(block_header), 1, file) != 1)
    {
      if (std::feof(file))
      {
        break;
      }
      addIssue(result, IssueType::FrameTruncated, IssueSeverity::Error,
               "Truncated block header at offset " + std::to_string(current_pos), current_pos);
      return false;
    }

    // Validate block magic
    if (!block_header.isValid())
    {
      addIssue(result, IssueType::BlockMagicInvalid, IssueSeverity::Error,
               "Invalid block magic at offset " + std::to_string(current_pos), current_pos);
      return false;
    }

    // Read compressed data
    compressed_buffer.resize(block_header.compressed_size);
    if (std::fread(compressed_buffer.data(), 1, block_header.compressed_size, file) !=
        block_header.compressed_size)
    {
      addIssue(result, IssueType::FrameTruncated, IssueSeverity::Error,
               "Truncated compressed block at offset " + std::to_string(current_pos), current_pos);
      return false;
    }

    // Decompress
    decompressed_buffer.resize(block_header.original_size);
    size_t decompressed_size =
        Compressor::decompress(result.compression_type, compressed_buffer.data(),
                               block_header.compressed_size, decompressed_buffer.data(),
                               block_header.original_size);

    if (decompressed_size != block_header.original_size)
    {
      addIssue(result, IssueType::BlockDecompressionFailed, IssueSeverity::Error,
               "Decompression failed at offset " + std::to_string(current_pos), current_pos);
      if (_config.stop_on_first_error)
      {
        return false;
      }
      continue;
    }

    // Parse events from decompressed block
    size_t offset = 0;
    uint16_t events_in_block = 0;

    while (offset + sizeof(FrameHeader) <= decompressed_buffer.size() &&
           events_in_block < block_header.event_count)
    {
      FrameHeader frame;
      std::memcpy(&frame, decompressed_buffer.data() + offset, sizeof(frame));
      offset += sizeof(FrameHeader);

      if (offset + frame.size > decompressed_buffer.size())
      {
        addIssue(result, IssueType::FrameTruncated, IssueSeverity::Error,
                 "Truncated frame in block at offset " + std::to_string(current_pos), current_pos);
        break;
      }

      // Verify CRC
      if (_config.verify_crc)
      {
        uint32_t computed_crc = Crc32::compute(decompressed_buffer.data() + offset, frame.size);
        if (computed_crc != frame.crc32)
        {
          addIssue(result, IssueType::FrameCrcMismatch, IssueSeverity::Error,
                   "CRC mismatch in block at offset " + std::to_string(current_pos), current_pos);
          ++result.crc_errors;
        }
      }

      // Parse event for timestamp
      int64_t event_ts = 0;
      EventType type = static_cast<EventType>(frame.type);

      if (type == EventType::Trade && frame.size >= sizeof(TradeRecord))
      {
        TradeRecord trade;
        std::memcpy(&trade, decompressed_buffer.data() + offset, sizeof(trade));
        event_ts = trade.exchange_ts_ns;
        ++result.trades_found;
      }
      else if ((type == EventType::BookSnapshot || type == EventType::BookDelta) &&
               frame.size >= sizeof(BookRecordHeader))
      {
        BookRecordHeader book;
        std::memcpy(&book, decompressed_buffer.data() + offset, sizeof(book));
        event_ts = book.exchange_ts_ns;
        ++result.book_updates_found;
      }

      // Check timestamps
      if (_config.verify_timestamps && event_ts > 0)
      {
        if (result.actual_event_count == 0)
        {
          result.actual_first_ts = event_ts;
        }
        result.actual_last_ts = event_ts;

        if (last_timestamp > 0 && event_ts < last_timestamp)
        {
          ++result.timestamp_anomalies;
        }
        last_timestamp = event_ts;
      }

      offset += frame.size;
      ++result.actual_event_count;
      ++events_in_block;
    }

    result.bytes_scanned = current_pos + sizeof(CompressedBlockHeader) +
                           block_header.compressed_size - sizeof(SegmentHeader);
  }

  return true;
}

bool SegmentValidator::validateIndex(std::FILE* file, SegmentValidationResult& result)
{
  // Seek to index position
  std::fseek(file, 0, SEEK_SET);
  SegmentHeader header;
  if (std::fread(&header, sizeof(header), 1, file) != 1)
  {
    return false;
  }

  if (!header.hasIndex())
  {
    return true;
  }

  std::fseek(file, static_cast<long>(header.index_offset), SEEK_SET);

  // Read index header
  SegmentIndexHeader idx_header;
  if (std::fread(&idx_header, sizeof(idx_header), 1, file) != 1)
  {
    addIssue(result, IssueType::IndexOutOfBounds, IssueSeverity::Warning,
             "Cannot read index header");
    return false;
  }

  if (!idx_header.isValid())
  {
    addIssue(result, IssueType::IndexMagicInvalid, IssueSeverity::Warning, "Invalid index magic");
    return false;
  }

  result.index_entry_count = idx_header.entry_count;

  // Read and verify index entries
  std::vector<IndexEntry> entries(idx_header.entry_count);
  if (std::fread(entries.data(), sizeof(IndexEntry), idx_header.entry_count, file) !=
      idx_header.entry_count)
  {
    addIssue(result, IssueType::IndexOutOfBounds, IssueSeverity::Warning,
             "Truncated index entries");
    return false;
  }

  // Verify CRC
  uint32_t computed_crc = Crc32::compute(entries.data(), entries.size() * sizeof(IndexEntry));
  if (computed_crc != idx_header.crc32)
  {
    addIssue(result, IssueType::IndexCrcMismatch, IssueSeverity::Warning, "Index CRC mismatch");
    return false;
  }

  // Check entries are sorted
  for (size_t i = 1; i < entries.size(); ++i)
  {
    if (entries[i].timestamp_ns < entries[i - 1].timestamp_ns)
    {
      addIssue(result, IssueType::IndexNotSorted, IssueSeverity::Warning,
               "Index entries not sorted at position " + std::to_string(i));
      break;
    }
  }

  result.index_valid = true;
  return true;
}

void SegmentValidator::addIssue(SegmentValidationResult& result, IssueType type,
                                IssueSeverity severity, const std::string& message,
                                uint64_t offset)
{
  result.issues.push_back(ValidationIssue{
      .type = type, .severity = severity, .message = message, .file_offset = offset});

  if (severity == IssueSeverity::Critical || severity == IssueSeverity::Error)
  {
    result.valid = false;
  }
}

DatasetValidator::DatasetValidator(ValidatorConfig config) : _config(std::move(config)) {}

DatasetValidationResult DatasetValidator::validate(const std::filesystem::path& data_dir)
{
  ProgressCallback no_progress = nullptr;
  return validate(data_dir, no_progress);
}

DatasetValidationResult DatasetValidator::validate(const std::filesystem::path& data_dir,
                                                   ProgressCallback progress)
{
  DatasetValidationResult result;
  result.data_dir = data_dir;

  if (!std::filesystem::exists(data_dir))
  {
    result.valid = false;
    return result;
  }

  // Collect segment files
  std::vector<std::filesystem::path> segment_paths;
  for (const auto& entry : std::filesystem::directory_iterator(data_dir))
  {
    if (entry.is_regular_file() && entry.path().extension() == ".floxlog")
    {
      segment_paths.push_back(entry.path());
    }
  }

  std::sort(segment_paths.begin(), segment_paths.end());
  result.total_segments = static_cast<uint32_t>(segment_paths.size());

  // Validate each segment
  SegmentValidator segment_validator(_config);

  for (uint32_t i = 0; i < segment_paths.size(); ++i)
  {
    if (progress)
    {
      progress(i, result.total_segments, segment_paths[i]);
    }

    auto segment_result = segment_validator.validate(segment_paths[i]);
    result.segments.push_back(std::move(segment_result));

    auto& seg = result.segments.back();

    // Update aggregates
    if (seg.valid && !seg.hasErrors())
    {
      ++result.valid_segments;
    }
    else
    {
      ++result.corrupted_segments;
    }

    result.total_events += seg.actual_event_count;

    std::error_code ec;
    auto file_size = std::filesystem::file_size(segment_paths[i], ec);
    if (!ec)
    {
      result.total_bytes += file_size;
    }

    // Update time range
    if (seg.actual_first_ts > 0)
    {
      if (result.first_timestamp == 0 || seg.actual_first_ts < result.first_timestamp)
      {
        result.first_timestamp = seg.actual_first_ts;
      }
    }
    if (seg.actual_last_ts > 0)
    {
      if (seg.actual_last_ts > result.last_timestamp)
      {
        result.last_timestamp = seg.actual_last_ts;
      }
    }

    // Count issues
    for (const auto& issue : seg.issues)
    {
      if (issue.severity == IssueSeverity::Error || issue.severity == IssueSeverity::Critical)
      {
        ++result.total_errors;
      }
      else if (issue.severity == IssueSeverity::Warning)
      {
        ++result.total_warnings;
      }
    }
  }

  result.valid = (result.corrupted_segments == 0);

  return result;
}

SegmentRepairer::SegmentRepairer(RepairConfig config) : _config(std::move(config)) {}

RepairResult SegmentRepairer::repair(const std::filesystem::path& segment_path)
{
  SegmentValidator validator;
  auto validation = validator.validate(segment_path);
  return repair(segment_path, validation);
}

RepairResult SegmentRepairer::repair(const std::filesystem::path& segment_path,
                                     const SegmentValidationResult& validation)
{
  RepairResult result;
  result.path = segment_path;

  if (!std::filesystem::exists(segment_path))
  {
    result.errors.push_back("File not found");
    return result;
  }

  // Create backup if requested
  if (_config.backup_before_repair)
  {
    if (!createBackup(segment_path, result))
    {
      return result;
    }
  }

  // Fix header timestamps if needed
  if (_config.fix_header_timestamps && validation.actual_first_ts > 0)
  {
    if (validation.reported_first_ts != validation.actual_first_ts ||
        validation.reported_last_ts != validation.actual_last_ts)
    {
      fixHeaderTimestamps(segment_path, validation, result);
    }
  }

  // Fix event count if needed
  if (_config.fix_event_count && validation.reported_event_count != validation.actual_event_count)
  {
    fixEventCount(segment_path, validation, result);
  }

  // Rebuild index if needed
  if (_config.rebuild_index && validation.has_index && !validation.index_valid)
  {
    rebuildIndex(segment_path, result);
  }

  result.success = result.errors.empty();
  return result;
}

bool SegmentRepairer::createBackup(const std::filesystem::path& path, RepairResult& result)
{
  result.backup_path = path;
  result.backup_path += _config.backup_suffix;

  std::error_code ec;
  std::filesystem::copy_file(path, result.backup_path,
                             std::filesystem::copy_options::overwrite_existing, ec);

  if (ec)
  {
    result.errors.push_back("Failed to create backup: " + ec.message());
    return false;
  }

  result.backup_created = true;
  result.actions_taken.push_back("Created backup: " + result.backup_path.string());
  return true;
}

bool SegmentRepairer::fixHeaderTimestamps(const std::filesystem::path& path,
                                          const SegmentValidationResult& val, RepairResult& result)
{
  std::FILE* file = std::fopen(path.string().c_str(), "r+b");
  if (!file)
  {
    result.errors.push_back("Cannot open file for writing");
    return false;
  }

  // Read header
  SegmentHeader header;
  if (std::fread(&header, sizeof(header), 1, file) != 1)
  {
    std::fclose(file);
    result.errors.push_back("Cannot read header");
    return false;
  }

  // Update timestamps
  header.first_event_ns = val.actual_first_ts;
  header.last_event_ns = val.actual_last_ts;

  // Write back
  std::fseek(file, 0, SEEK_SET);
  if (std::fwrite(&header, sizeof(header), 1, file) != 1)
  {
    std::fclose(file);
    result.errors.push_back("Cannot write header");
    return false;
  }

  std::fclose(file);
  result.actions_taken.push_back("Fixed header timestamps");
  return true;
}

bool SegmentRepairer::fixEventCount(const std::filesystem::path& path,
                                    const SegmentValidationResult& val, RepairResult& result)
{
  std::FILE* file = std::fopen(path.string().c_str(), "r+b");
  if (!file)
  {
    result.errors.push_back("Cannot open file for writing");
    return false;
  }

  // Read header
  SegmentHeader header;
  if (std::fread(&header, sizeof(header), 1, file) != 1)
  {
    std::fclose(file);
    result.errors.push_back("Cannot read header");
    return false;
  }

  // Update event count
  header.event_count = val.actual_event_count;

  // Write back
  std::fseek(file, 0, SEEK_SET);
  if (std::fwrite(&header, sizeof(header), 1, file) != 1)
  {
    std::fclose(file);
    result.errors.push_back("Cannot write header");
    return false;
  }

  std::fclose(file);
  result.actions_taken.push_back("Fixed event count: " + std::to_string(val.actual_event_count));
  return true;
}

bool SegmentRepairer::rebuildIndex(const std::filesystem::path& path, RepairResult& result)
{
  // Use IndexBuilder to rebuild
  // For now, just note that it would be done
  result.actions_taken.push_back("Index rebuild requested (use IndexBuilder)");
  return true;
}

}  // namespace flox::replay
