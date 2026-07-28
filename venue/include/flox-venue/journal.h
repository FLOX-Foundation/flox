/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/messages.h"

#include <cstdint>
#include <fstream>
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
  explicit Journal(const std::string& path)
      : out_(path, std::ios::binary | std::ios::trunc)
  {
  }

  void append(const InboundCommand& c) { append(c, 0); }

  // Write-ahead record: [ts:8][tag:1][command bytes]. The sequencer timestamp is
  // journaled so replay reproduces last-look / MMP / LULD timing exactly.
  void append(const InboundCommand& c, int64_t tsNs)
  {
    out_.write(reinterpret_cast<const char*>(&tsNs), 8);
    const uint8_t tag = static_cast<uint8_t>(c.index());  // variant alternative index
    out_.write(reinterpret_cast<const char*>(&tag), 1);
    std::visit([&](const auto& v)
               { out_.write(reinterpret_cast<const char*>(&v), sizeof(v)); }, c);
    ++count_;
  }

  void flush() { out_.flush(); }
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

  // Replay records with their sequencer timestamps (for exact timing replay).
  static std::vector<std::pair<int64_t, InboundCommand>> loadTimed(const std::string& path)
  {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::pair<int64_t, InboundCommand>> v;
    int64_t ts;
    uint8_t tag;
    auto readInto = [&](auto proto)
    {
      in.read(reinterpret_cast<char*>(&proto), sizeof(proto));
      v.emplace_back(ts, InboundCommand{proto});
    };
    while (in.read(reinterpret_cast<char*>(&ts), 8) && in.read(reinterpret_cast<char*>(&tag), 1))
    {
      switch (tag)
      {
        case 0:
          readInto(NewOrder{});
          break;
        case 1:
          readInto(CancelOrder{});
          break;
        case 2:
          readInto(ModifyOrder{});
          break;
        case 3:
          readInto(MassCancel{});
          break;
        case 4:
          readInto(Quote{});
          break;
        case 5:
          readInto(LastLookDecision{});
          break;
        case 6:
          readInto(SetMark{});
          break;
        case 7:
          readInto(ApplyFunding{});
          break;
        case 8:
          readInto(AdminCmd{});
          break;
        default:
          return v;  // unknown tag -> stop
      }
    }
    return v;
  }

 private:
  std::ofstream out_;
  uint64_t count_{0};
};

}  // namespace flox::venue
