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

#include <cstdint>

namespace flox::run
{

inline constexpr uint32_t kRunSegmentMagic = 0x4E555246;  // "FRUN"
inline constexpr uint16_t kRunFormatVersion = 1;

enum class RecordKind : uint8_t
{
  Unknown = 0,
  Signal = 1,
  OrderEvent = 2,
  Fill = 3,
};

enum class FrameType : uint8_t
{
  Signal = 10,
  OrderEvent = 11,
  Fill = 12,
};

namespace RunSegmentFlags
{
inline constexpr uint8_t HasIndex = 0x01;
inline constexpr uint8_t Compressed = 0x02;
inline constexpr uint8_t Sorted = 0x04;
}  // namespace RunSegmentFlags

struct alignas(8) RunSegmentHeader
{
  uint32_t magic{kRunSegmentMagic};
  uint16_t version{kRunFormatVersion};
  uint8_t flags{0};
  uint8_t record_kind{0};
  int64_t created_ns{0};
  int64_t first_event_ns{0};
  int64_t last_event_ns{0};
  uint32_t event_count{0};
  uint32_t _pad0{0};
  uint64_t index_offset{0};
  uint8_t compression{0};
  uint8_t reserved[15]{};

  bool isValid() const noexcept
  {
    return magic == kRunSegmentMagic && version == kRunFormatVersion;
  }
  bool isSorted() const noexcept { return (flags & RunSegmentFlags::Sorted) != 0; }
  bool hasIndex() const noexcept
  {
    return (flags & RunSegmentFlags::HasIndex) != 0 && index_offset > 0;
  }
  bool isCompressed() const noexcept { return (flags & RunSegmentFlags::Compressed) != 0; }
};
static_assert(sizeof(RunSegmentHeader) == 64, "RunSegmentHeader must be 64 bytes");

namespace SignalFlags
{
inline constexpr uint32_t Enter = 0x01;
inline constexpr uint32_t Exit = 0x02;
inline constexpr uint32_t Rebalance = 0x04;
}  // namespace SignalFlags

struct alignas(8) SignalRecord
{
  int64_t run_ts_ns{0};
  int64_t feed_ts_ns{0};
  uint32_t signal_id{0};
  uint16_t name_len{0};
  uint16_t symbol_count{0};
  uint32_t payload_len{0};
  uint32_t flags{0};
  int64_t strength_raw{0};
  int64_t _reserved{0};
};
static_assert(sizeof(SignalRecord) == 48, "SignalRecord must be 48 bytes");

enum class OrderEventKind : uint8_t
{
  Submit = 1,
  Cancel = 2,
  Modify = 3,
  Ack = 4,
  Reject = 5,
  Expire = 6,
};

namespace OrderEventFlags
{
inline constexpr uint32_t PostOnly = 0x01;
inline constexpr uint32_t ReduceOnly = 0x02;
inline constexpr uint32_t Ioc = 0x04;
}  // namespace OrderEventFlags

struct alignas(8) OrderEventRecord
{
  int64_t run_ts_ns{0};
  int64_t feed_ts_ns{0};
  uint64_t order_id{0};
  uint64_t parent_signal_id{0};
  int64_t price_raw{0};
  int64_t qty_raw{0};
  uint32_t symbol_id{0};
  uint8_t event_kind{0};
  uint8_t side{0};
  uint8_t order_type{0};
  uint8_t _pad0{0};
  uint32_t reason_len{0};
  uint32_t flags{0};
};
static_assert(sizeof(OrderEventRecord) == 64, "OrderEventRecord must be 64 bytes");

enum class FillLiquidity : uint8_t
{
  Unknown = 0,
  Maker = 1,
  Taker = 2,
};

struct alignas(8) FillRecord
{
  int64_t run_ts_ns{0};
  int64_t feed_ts_ns{0};
  uint64_t order_id{0};
  uint64_t fill_id{0};
  int64_t price_raw{0};
  int64_t qty_raw{0};
  int64_t fee_raw{0};
  uint32_t symbol_id{0};
  uint8_t side{0};
  uint8_t liquidity{0};
  uint16_t _pad0{0};
};
static_assert(sizeof(FillRecord) == 64, "FillRecord must be 64 bytes");

inline constexpr size_t alignUp8(size_t n) noexcept
{
  return (n + 7u) & ~size_t{7u};
}

inline constexpr size_t signalFrameSize(uint16_t name_len, uint16_t symbol_count, uint32_t payload_len) noexcept
{
  return alignUp8(sizeof(SignalRecord) + size_t{name_len} +
                  size_t{symbol_count} * sizeof(uint32_t) + size_t{payload_len});
}

inline constexpr size_t orderEventFrameSize(uint32_t reason_len) noexcept
{
  return alignUp8(sizeof(OrderEventRecord) + size_t{reason_len});
}

inline constexpr size_t fillFrameSize() noexcept { return sizeof(FillRecord); }

}  // namespace flox::run
