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

class Journal
{
 public:
  enum class Sync
  {
    Off,   // record is in the OS cache before append returns (survives process crash)
    Full,  // + fsync per record (survives power loss); production WAL default
  };

  static constexpr size_t kHeaderSize = sizeof(int64_t) + 1 + sizeof(uint32_t);  // ts+tag+len = 13

  explicit Journal(const std::string& path, Sync sync = Sync::Off) : sync_(sync)
  {
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
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
    ++count_;
  }

  void flush()
  {
    if (fd_ >= 0)
    {
      ::fsync(fd_);
    }
  }
  uint64_t count() const noexcept { return count_; }

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
  uint64_t count_{0};
  std::vector<uint8_t> rec_;
};

}  // namespace flox::venue
