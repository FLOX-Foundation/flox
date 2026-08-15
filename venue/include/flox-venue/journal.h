/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Write-ahead log for the sequenced venue shard: every inbound command is
 * appended here before it is applied, so a crash can be recovered by replaying
 * the log through a fresh engine.
 *
 * Record framing (native-endian, one record per command):
 *   [ts:8][tag:1][len:4][body:len][crc32:4]
 * tag is the InboundCommand variant index; len is sizeof the body struct; crc32
 * (flox::util::Crc32) covers ts+tag+len+body. A torn tail (a record whose bytes are
 * not fully present) or a corrupted record is DETECTED on load: the loader
 * returns the largest intact prefix and never materialises a partial/garbage
 * command.
 *
 * Durability: the writer uses a raw file descriptor, so an appended record is
 * in the OS page cache before append() returns -- it survives a process crash
 * even in the default (Sync::Off) mode, unlike a userspace-buffered stream.
 * Sync::Full additionally fsync()s each record, so it also survives power loss;
 * that is the mode a production venue should run its shard WAL in (trading
 * throughput for durability). SequencedShard selects the mode.
 */
#pragma once

#include "flox-venue/messages.h"

#include "flox/util/crc32.h"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace flox::venue
{

static_assert(std::is_trivially_copyable_v<NewOrder>, "NewOrder must be blittable");
static_assert(std::is_trivially_copyable_v<CancelOrder>, "CancelOrder must be blittable");
static_assert(std::is_trivially_copyable_v<ModifyOrder>, "ModifyOrder must be blittable");
static_assert(std::is_trivially_copyable_v<MassCancel>, "MassCancel must be blittable");
static_assert(std::is_trivially_copyable_v<Quote>, "Quote must be blittable");
static_assert(std::is_trivially_copyable_v<LastLookDecision>, "LastLookDecision must be blittable");
static_assert(std::is_trivially_copyable_v<SetMark>, "SetMark must be blittable");
static_assert(std::is_trivially_copyable_v<ApplyFunding>, "ApplyFunding must be blittable");
static_assert(std::is_trivially_copyable_v<AdminCmd>, "AdminCmd must be blittable");
static_assert(std::is_trivially_copyable_v<Deposit>, "Deposit must be blittable");
static_assert(std::is_trivially_copyable_v<Withdraw>, "Withdraw must be blittable");
static_assert(std::is_trivially_copyable_v<ListInstrument>, "ListInstrument must be blittable");
static_assert(std::is_trivially_copyable_v<SetBands>, "SetBands must be blittable");
static_assert(std::is_trivially_copyable_v<TimeTick>, "TimeTick must be blittable");
static_assert(std::is_trivially_copyable_v<SetTriggerRef>, "SetTriggerRef must be blittable");
static_assert(std::is_trivially_copyable_v<SnapshotBegin>, "SnapshotBegin must be blittable");
static_assert(std::is_trivially_copyable_v<RestoreOrder>, "RestoreOrder must be blittable");
static_assert(std::is_trivially_copyable_v<RestoreStop>, "RestoreStop must be blittable");
static_assert(std::is_trivially_copyable_v<RestorePeg>, "RestorePeg must be blittable");
static_assert(std::is_trivially_copyable_v<RestoreHeld>, "RestoreHeld must be blittable");
static_assert(std::is_trivially_copyable_v<RestorePosition>, "RestorePosition must be blittable");
static_assert(std::is_trivially_copyable_v<RestoreMmpCfg>, "RestoreMmpCfg must be blittable");
static_assert(std::is_trivially_copyable_v<RestoreClOrdIds>, "RestoreClOrdIds must be blittable");
static_assert(std::is_trivially_copyable_v<SnapshotEnd>, "SnapshotEnd must be blittable");
static_assert(std::is_trivially_copyable_v<RestoreReservation>,
              "RestoreReservation must be blittable");
static_assert(std::is_trivially_copyable_v<RestoreBalance>, "RestoreBalance must be blittable");
static_assert(std::is_trivially_copyable_v<RestoreMmpFills>, "RestoreMmpFills must be blittable");
static_assert(std::is_trivially_copyable_v<SetStpGroup>, "SetStpGroup must be blittable");
static_assert(std::is_trivially_copyable_v<SetFundingSchedule>,
              "SetFundingSchedule must be blittable");
static_assert(std::is_trivially_copyable_v<RestoreFunding>, "RestoreFunding must be blittable");
static_assert(std::is_trivially_copyable_v<ForceClosePosition>,
              "ForceClosePosition must be blittable");
static_assert(std::is_trivially_copyable_v<RestoreOrderStp>, "RestoreOrderStp must be blittable");
static_assert(std::is_trivially_copyable_v<SetAdmissionProfile>,
              "SetAdmissionProfile must be blittable");
static_assert(std::is_trivially_copyable_v<SetRiskLimits>, "SetRiskLimits must be blittable");
static_assert(std::variant_size_v<InboundCommand> == 34,
              "new InboundCommand alternative: extend expectedBodySize/appendDecoded and the "
              "blittable asserts above");

class Journal
{
 public:
  enum class Sync
  {
    Off,    // record is in the OS cache before append returns (survives process crash)
    Full,   // + fsync per record (survives power loss); production WAL default
    Group,  // + fsync on demand, not per record: the caller decides where the
            // barrier goes and pays for one fsync per batch instead of one per
            // command. Only durable if the caller calls sync() BEFORE telling
            // anyone the commands took effect -- an unsynced batch that has
            // already been acknowledged is exactly the promise Full exists to
            // keep, broken more cheaply.
  };

  // A recovering writer MUST open in Append: Truncate erases the very log the
  // process is supposed to replay. SequencedShard always uses Append; Truncate
  // is for starting a fresh log on a path the caller knows is disposable.
  enum class OpenMode
  {
    Truncate,
    Append,
  };

  static constexpr size_t kHeaderSize = sizeof(int64_t) + 1 + sizeof(uint32_t);  // ts+tag+len = 13

  explicit Journal(const std::string& path, Sync sync = Sync::Off,
                   OpenMode mode = OpenMode::Truncate)
      : sync_(sync)
  {
    const int flags = O_WRONLY | O_CREAT | (mode == OpenMode::Truncate ? O_TRUNC : O_APPEND);
    fd_ = ::open(path.c_str(), flags, 0644);
    if (fd_ < 0)
    {
      throw std::runtime_error("Journal: cannot open '" + path + "' for writing");
    }
  }

  ~Journal()
  {
    if (fd_ >= 0)
    {
      ::close(fd_);
    }
  }

  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;

  // Close the current file and continue on `path` (segment rotation at a
  // checkpoint). Resets the per-segment record/byte counters; they count
  // appends since open, so on an Append reopen of an existing file they
  // approximate the segment size from this process's perspective only.
  void reopen(const std::string& path, OpenMode mode)
  {
    if (fd_ >= 0)
    {
      ::fsync(fd_);
      ::close(fd_);
    }
    const int flags = O_WRONLY | O_CREAT | (mode == OpenMode::Truncate ? O_TRUNC : O_APPEND);
    fd_ = ::open(path.c_str(), flags, 0644);
    if (fd_ < 0)
    {
      throw std::runtime_error("Journal: cannot open '" + path + "' for writing");
    }
    count_.store(0, std::memory_order_relaxed);
    bytes_.store(0, std::memory_order_relaxed);
  }

  void append(const InboundCommand& c) { append(c, 0); }

  // Write-ahead record: [ts][tag][len][body][crc]. The sequencer timestamp is
  // journaled so replay reproduces last-look / MMP / LULD timing exactly.
  void append(const InboundCommand& c, int64_t tsNs)
  {
    const uint8_t tag = static_cast<uint8_t>(c.index());
    std::visit(
        [&](const auto& v)
        {
          const uint32_t len = static_cast<uint32_t>(sizeof(v));
          rec_.clear();
          appendBytes(&tsNs, sizeof(tsNs));
          appendBytes(&tag, sizeof(tag));
          appendBytes(&len, sizeof(len));
          appendBytes(&v, sizeof(v));
          const uint32_t crc = flox::util::Crc32::compute(rec_.data(), rec_.size());
          appendBytes(&crc, sizeof(crc));
        },
        c);
    writeAll(rec_.data(), rec_.size());
    if (sync_ == Sync::Full)
    {
      ::fsync(fd_);
    }
    else if (sync_ == Sync::Group)
    {
      dirty_ = true;
    }
    count_.fetch_add(1, std::memory_order_relaxed);
    bytes_.fetch_add(rec_.size(), std::memory_order_relaxed);
  }

  // Make everything appended since the last call durable. A no-op unless there
  // is something to make durable, so calling it on every quiet batch costs a
  // branch rather than a syscall.
  void sync()
  {
    if (dirty_ && fd_ >= 0)
    {
      ::fsync(fd_);
      ++syncs_;
      dirty_ = false;
    }
  }

  // Barriers taken. Observable because the barrier's ORDER relative to
  // publication is testable from this machine and its physics is not: a normal
  // read sees the page cache whether or not fsync ran, so a test that reads the
  // file back proves nothing about durability. This counter proves the code
  // took the barrier before it spoke, which is the part a test can own.
  uint64_t syncs() const noexcept { return syncs_; }

  void flush()
  {
    if (fd_ >= 0)
    {
      ::fsync(fd_);
    }
  }
  // Records/bytes appended since open (atomic: the shard's idle sweeper reads
  // them from another thread for the checkpoint auto-trigger).
  uint64_t count() const noexcept { return count_.load(std::memory_order_relaxed); }
  uint64_t bytes() const noexcept { return bytes_.load(std::memory_order_relaxed); }

  static std::vector<InboundCommand> load(const std::string& path)
  {
    std::vector<InboundCommand> v;
    for (auto& [ts, cmd] : loadTimed(path))
    {
      (void)ts;
      v.push_back(cmd);
    }
    return v;
  }

  // Replay records with their sequencer timestamps. Stops at the first record
  // that is short (torn tail) or fails its crc (corruption), returning the
  // intact prefix -- a partial trailing record is never materialised.
  static std::vector<std::pair<int64_t, InboundCommand>> loadTimed(const std::string& path)
  {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::pair<int64_t, InboundCommand>> v;
    if (!in)
    {
      return v;
    }

    std::vector<uint8_t> frame;  // header+body, staged for one-shot crc
    while (true)
    {
      frame.resize(kHeaderSize);
      if (!in.read(reinterpret_cast<char*>(frame.data()), kHeaderSize))
      {
        break;  // no more (complete) headers
      }
      const uint8_t tag = frame[8];
      uint32_t len;
      std::memcpy(&len, frame.data() + 9, sizeof(len));

      const uint32_t expect = expectedBodySize(tag);
      if (expect == 0 || len != expect)
      {
        break;  // unknown tag or wrong-sized body -> corrupt, stop
      }
      frame.resize(kHeaderSize + len);
      if (!in.read(reinterpret_cast<char*>(frame.data() + kHeaderSize), len))
      {
        break;  // torn body
      }
      uint32_t crc;
      if (!in.read(reinterpret_cast<char*>(&crc), sizeof(crc)))
      {
        break;  // torn crc
      }
      if (flox::util::Crc32::compute(frame.data(), frame.size()) != crc)
      {
        break;  // corrupted record
      }

      int64_t ts;
      std::memcpy(&ts, frame.data(), sizeof(ts));
      appendDecoded(v, ts, tag, frame.data() + kHeaderSize);
    }
    return v;
  }

 private:
  // Expected body size for a variant tag, or 0 if the tag is unknown.
  static uint32_t expectedBodySize(uint8_t tag)
  {
    switch (tag)
    {
      case 0:
        return sizeof(NewOrder);
      case 1:
        return sizeof(CancelOrder);
      case 2:
        return sizeof(ModifyOrder);
      case 3:
        return sizeof(MassCancel);
      case 4:
        return sizeof(Quote);
      case 5:
        return sizeof(LastLookDecision);
      case 6:
        return sizeof(SetMark);
      case 7:
        return sizeof(ApplyFunding);
      case 8:
        return sizeof(AdminCmd);
      case 9:
        return sizeof(Deposit);
      case 10:
        return sizeof(Withdraw);
      case 11:
        return sizeof(ListInstrument);
      case 12:
        return sizeof(SetBands);
      case 13:
        return sizeof(TimeTick);
      case 14:
        return sizeof(SetTriggerRef);
      case 15:
        return sizeof(SnapshotBegin);
      case 16:
        return sizeof(RestoreOrder);
      case 17:
        return sizeof(RestoreStop);
      case 18:
        return sizeof(RestorePeg);
      case 19:
        return sizeof(RestoreHeld);
      case 20:
        return sizeof(RestorePosition);
      case 21:
        return sizeof(RestoreMmpCfg);
      case 22:
        return sizeof(RestoreClOrdIds);
      case 23:
        return sizeof(SnapshotEnd);
      case 24:
        return sizeof(RestoreReservation);
      case 25:
        return sizeof(RestoreBalance);
      case 26:
        return sizeof(RestoreMmpFills);
      case 27:
        return sizeof(SetStpGroup);
      case 28:
        return sizeof(SetFundingSchedule);
      case 29:
        return sizeof(RestoreFunding);
      case 30:
        return sizeof(ForceClosePosition);
      case 31:
        return sizeof(RestoreOrderStp);
      case 32:
        return sizeof(SetAdmissionProfile);
      case 33:
        return sizeof(SetRiskLimits);
      default:
        return 0;
    }
  }

  template <class T>
  static InboundCommand fromBody(const uint8_t* body)
  {
    T proto;
    std::memcpy(&proto, body, sizeof(T));
    return InboundCommand{proto};
  }

  static void appendDecoded(std::vector<std::pair<int64_t, InboundCommand>>& v, int64_t ts,
                            uint8_t tag, const uint8_t* body)
  {
    switch (tag)
    {
      case 0:
        v.emplace_back(ts, fromBody<NewOrder>(body));
        break;
      case 1:
        v.emplace_back(ts, fromBody<CancelOrder>(body));
        break;
      case 2:
        v.emplace_back(ts, fromBody<ModifyOrder>(body));
        break;
      case 3:
        v.emplace_back(ts, fromBody<MassCancel>(body));
        break;
      case 4:
        v.emplace_back(ts, fromBody<Quote>(body));
        break;
      case 5:
        v.emplace_back(ts, fromBody<LastLookDecision>(body));
        break;
      case 6:
        v.emplace_back(ts, fromBody<SetMark>(body));
        break;
      case 7:
        v.emplace_back(ts, fromBody<ApplyFunding>(body));
        break;
      case 8:
        v.emplace_back(ts, fromBody<AdminCmd>(body));
        break;
      case 9:
        v.emplace_back(ts, fromBody<Deposit>(body));
        break;
      case 10:
        v.emplace_back(ts, fromBody<Withdraw>(body));
        break;
      case 11:
        v.emplace_back(ts, fromBody<ListInstrument>(body));
        break;
      case 12:
        v.emplace_back(ts, fromBody<SetBands>(body));
        break;
      case 13:
        v.emplace_back(ts, fromBody<TimeTick>(body));
        break;
      case 14:
        v.emplace_back(ts, fromBody<SetTriggerRef>(body));
        break;
      case 15:
        v.emplace_back(ts, fromBody<SnapshotBegin>(body));
        break;
      case 16:
        v.emplace_back(ts, fromBody<RestoreOrder>(body));
        break;
      case 17:
        v.emplace_back(ts, fromBody<RestoreStop>(body));
        break;
      case 18:
        v.emplace_back(ts, fromBody<RestorePeg>(body));
        break;
      case 19:
        v.emplace_back(ts, fromBody<RestoreHeld>(body));
        break;
      case 20:
        v.emplace_back(ts, fromBody<RestorePosition>(body));
        break;
      case 21:
        v.emplace_back(ts, fromBody<RestoreMmpCfg>(body));
        break;
      case 22:
        v.emplace_back(ts, fromBody<RestoreClOrdIds>(body));
        break;
      case 23:
        v.emplace_back(ts, fromBody<SnapshotEnd>(body));
        break;
      case 24:
        v.emplace_back(ts, fromBody<RestoreReservation>(body));
        break;
      case 25:
        v.emplace_back(ts, fromBody<RestoreBalance>(body));
        break;
      case 26:
        v.emplace_back(ts, fromBody<RestoreMmpFills>(body));
        break;
      case 27:
        v.emplace_back(ts, fromBody<SetStpGroup>(body));
        break;
      case 28:
        v.emplace_back(ts, fromBody<SetFundingSchedule>(body));
        break;
      case 29:
        v.emplace_back(ts, fromBody<RestoreFunding>(body));
        break;
      case 30:
        v.emplace_back(ts, fromBody<ForceClosePosition>(body));
        break;
      case 31:
        v.emplace_back(ts, fromBody<RestoreOrderStp>(body));
        break;
      case 32:
        v.emplace_back(ts, fromBody<SetAdmissionProfile>(body));
        break;
      case 33:
        v.emplace_back(ts, fromBody<SetRiskLimits>(body));
        break;
    }
  }

  void appendBytes(const void* p, size_t n)
  {
    const auto* b = static_cast<const uint8_t*>(p);
    rec_.insert(rec_.end(), b, b + n);
  }

  void writeAll(const uint8_t* p, size_t n)
  {
    size_t off = 0;
    while (off < n)
    {
      const ssize_t w = ::write(fd_, p + off, n - off);
      if (w <= 0)
      {
        throw std::runtime_error("Journal: write failed");
      }
      off += static_cast<size_t>(w);
    }
  }

  int fd_{-1};
  Sync sync_{Sync::Off};
  bool dirty_{false};  // Group mode: records appended since the last sync()
  uint64_t syncs_{0};
  std::atomic<uint64_t> count_{0};
  std::atomic<uint64_t> bytes_{0};
  std::vector<uint8_t> rec_;
};

}  // namespace flox::venue
