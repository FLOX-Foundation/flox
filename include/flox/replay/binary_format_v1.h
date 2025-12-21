/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <span>

namespace flox::replay
{

inline constexpr uint32_t kMagic = 0x584F4C46;
inline constexpr uint16_t kFormatVersion = 1;

enum class EventType : uint8_t
{
  Trade = 1,
  BookSnapshot = 2,
  BookDelta = 3
};

namespace SegmentFlags
{
inline constexpr uint8_t HasIndex = 0x01;
inline constexpr uint8_t Compressed = 0x02;
inline constexpr uint8_t Encrypted = 0x04;
}  // namespace SegmentFlags

enum class CompressionType : uint8_t
{
  None = 0,
  LZ4 = 1,
};

struct alignas(8) SegmentHeader
{
  uint32_t magic{kMagic};
  uint16_t version{kFormatVersion};
  uint8_t flags{0};
  uint8_t exchange_id{0};
  int64_t created_ns{0};
  int64_t first_event_ns{0};
  int64_t last_event_ns{0};
  uint32_t event_count{0};
  uint32_t symbol_count{0};
  uint64_t index_offset{0};
  uint8_t compression{0};
  uint8_t reserved[15]{};

  bool isValid() const noexcept { return magic == kMagic && version == kFormatVersion; }
  bool hasIndex() const noexcept { return (flags & SegmentFlags::HasIndex) && index_offset > 0; }
  bool isCompressed() const noexcept { return (flags & SegmentFlags::Compressed) != 0; }
  CompressionType compressionType() const noexcept { return static_cast<CompressionType>(compression); }
};
static_assert(sizeof(SegmentHeader) == 64, "SegmentHeader must be 64 bytes");

struct FrameHeader
{
  uint32_t size{0};
  uint32_t crc32{0};
  uint8_t type{0};
  uint8_t rec_version{1};
  uint16_t flags{0};
};
static_assert(sizeof(FrameHeader) == 12, "FrameHeader must be 12 bytes");

struct alignas(8) TradeRecord
{
  int64_t exchange_ts_ns{0};
  int64_t recv_ts_ns{0};
  int64_t price_raw{0};
  int64_t qty_raw{0};
  uint64_t trade_id{0};
  uint32_t symbol_id{0};
  uint8_t side{0};
  uint8_t instrument{0};
  uint16_t exchange_id{0};
};
static_assert(sizeof(TradeRecord) == 48, "TradeRecord must be 48 bytes");

struct alignas(8) BookLevel
{
  int64_t price_raw{0};
  int64_t qty_raw{0};
};
static_assert(sizeof(BookLevel) == 16, "BookLevel must be 16 bytes");

struct alignas(8) BookRecordHeader
{
  int64_t exchange_ts_ns{0};
  int64_t recv_ts_ns{0};
  int64_t seq{0};
  uint32_t symbol_id{0};
  uint16_t bid_count{0};
  uint16_t ask_count{0};
  uint8_t type{0};
  uint8_t instrument{0};
  uint16_t exchange_id{0};
  uint32_t _pad{0};
};
static_assert(sizeof(BookRecordHeader) == 40, "BookRecordHeader must be 40 bytes");

class Crc32
{
 public:
  static uint32_t compute(const void* data, size_t size) noexcept
  {
    initTable();
    uint32_t crc = 0xFFFFFFFFu;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i)
    {
      crc = _table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
  }

  static uint32_t compute(std::span<const std::byte> data) noexcept
  {
    return compute(data.data(), data.size());
  }

 private:
  static inline uint32_t _table[256]{};
  static inline bool _initialized{false};

  static void initTable() noexcept
  {
    if (_initialized)
    {
      return;
    }
    for (uint32_t i = 0; i < 256; ++i)
    {
      uint32_t c = i;
      for (int j = 0; j < 8; ++j)
      {
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      _table[i] = c;
    }
    _initialized = true;
  }
};

inline constexpr size_t bookRecordSize(uint16_t bid_count, uint16_t ask_count) noexcept
{
  return sizeof(BookRecordHeader) + (bid_count + ask_count) * sizeof(BookLevel);
}

inline constexpr uint32_t kBlockMagic = 0x4B4C4246;

struct alignas(8) CompressedBlockHeader
{
  uint32_t magic{kBlockMagic};
  uint32_t compressed_size{0};
  uint32_t original_size{0};
  uint16_t event_count{0};
  uint16_t flags{0};

  bool isValid() const noexcept { return magic == kBlockMagic; }
};
static_assert(sizeof(CompressedBlockHeader) == 16, "CompressedBlockHeader must be 16 bytes");

inline constexpr uint32_t kIndexMagic = 0x58444E49;
inline constexpr uint16_t kIndexVersion = 1;

struct alignas(8) IndexEntry
{
  int64_t timestamp_ns{0};
  uint64_t file_offset{0};
};
static_assert(sizeof(IndexEntry) == 16, "IndexEntry must be 16 bytes");

struct alignas(8) SegmentIndexHeader
{
  uint32_t magic{kIndexMagic};
  uint16_t version{kIndexVersion};
  uint16_t interval{0};
  uint32_t entry_count{0};
  uint32_t crc32{0};
  int64_t first_ts_ns{0};
  int64_t last_ts_ns{0};

  bool isValid() const noexcept { return magic == kIndexMagic && version == kIndexVersion; }
};
static_assert(sizeof(SegmentIndexHeader) == 32, "SegmentIndexHeader must be 32 bytes");

inline constexpr uint32_t kGlobalIndexMagic = 0x58444947;
inline constexpr uint16_t kGlobalIndexVersion = 1;

struct alignas(8) GlobalIndexSegment
{
  int64_t first_event_ns{0};
  int64_t last_event_ns{0};
  uint32_t event_count{0};
  uint32_t flags{0};
  uint64_t file_size{0};
  uint64_t filename_offset{0};
  uint64_t _reserved{0};
};
static_assert(sizeof(GlobalIndexSegment) == 48, "GlobalIndexSegment must be 48 bytes");

struct alignas(8) GlobalIndexHeader
{
  uint32_t magic{kGlobalIndexMagic};
  uint16_t version{kGlobalIndexVersion};
  uint16_t flags{0};
  int64_t created_ns{0};
  int64_t first_event_ns{0};
  int64_t last_event_ns{0};
  uint32_t segment_count{0};
  uint32_t crc32{0};
  uint64_t total_events{0};
  uint64_t string_table_offset{0};
  uint8_t reserved[8]{};

  bool isValid() const noexcept
  {
    return magic == kGlobalIndexMagic && version == kGlobalIndexVersion;
  }
};
static_assert(sizeof(GlobalIndexHeader) == 64, "GlobalIndexHeader must be 64 bytes");

inline constexpr uint16_t kDefaultIndexInterval = 1000;

}  // namespace flox::replay
