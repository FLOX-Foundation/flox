/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/matching_engine.h"

#include <optional>
#include <unordered_map>
#include <vector>

namespace flox::venue
{

class InstrumentRegistry
{
 public:
  // List a new tradable instrument. Returns false if the id already exists.
  bool listInstrument(const SymbolConfig& cfg)
  {
    if (instruments_.count(cfg.id))
    {
      return false;
    }
    instruments_[cfg.id] = cfg;
    return true;
  }

  bool has(SymbolId id) const noexcept { return instruments_.count(id) != 0; }

  const SymbolConfig* get(SymbolId id) const
  {
    auto it = instruments_.find(id);
    return it == instruments_.end() ? nullptr : &it->second;
  }

  bool halt(SymbolId id, bool halted)
  {
    auto it = instruments_.find(id);
    if (it == instruments_.end())
    {
      return false;
    }
    it->second.halted = halted;
    return true;
  }

  bool setPriceBand(SymbolId id, Price minPrice, Price maxPrice)
  {
    auto it = instruments_.find(id);
    if (it == instruments_.end())
    {
      return false;
    }
    it->second.minPrice = minPrice;
    it->second.maxPrice = maxPrice;
    return true;
  }

  bool setTriggerRef(SymbolId id, TriggerRef ref)
  {
    auto it = instruments_.find(id);
    if (it == instruments_.end())
    {
      return false;
    }
    it->second.triggerRef = ref;
    return true;
  }

  // Rebuild registry state from the journaled command stream. Listing, band
  // changes, trigger-reference switches and halts are sequenced commands
  // (forwarded by ControlApi), so a restart replays the same journal into the
  // registry that the engines replay -- the WAL is the configuration source of
  // truth, not an external store.
  bool apply(const InboundCommand& cmd)
  {
    if (const auto* li = std::get_if<ListInstrument>(&cmd))
    {
      SymbolConfig c;
      c.id = li->symbol;
      c.tickSize = li->tickSize;
      c.lotSize = li->lotSize;
      c.minPrice = li->minPrice;
      c.maxPrice = li->maxPrice;
      return listInstrument(c);
    }
    if (const auto* sb = std::get_if<SetBands>(&cmd))
    {
      return setPriceBand(sb->symbol, sb->minPrice, sb->maxPrice);
    }
    if (const auto* st = std::get_if<SetTriggerRef>(&cmd))
    {
      return setTriggerRef(st->symbol, st->ref);
    }
    if (const auto* ad = std::get_if<AdminCmd>(&cmd))
    {
      switch (ad->action)
      {
        case AdminAction::Halt:
        case AdminAction::HaltAndCancelAll:
          return halt(ad->symbol, true);
        case AdminAction::Resume:
        case AdminAction::ResumeAuction:
          return halt(ad->symbol, false);
        default:
          // Auction phases and session boundaries carry no registry state: the
          // registry holds instrument CONFIGURATION, and neither an uncross nor
          // a closed session is configuration -- both are engine state the
          // shards own and the checkpoint carries.
          return true;
      }
    }
    if (std::get_if<SetStpGroup>(&cmd) != nullptr ||
        std::get_if<SetFundingSchedule>(&cmd) != nullptr)
    {
      return true;  // engine-owned state: the shards consume it, no registry state
    }
    return false;  // not a configuration command
  }

  std::vector<SymbolId> list() const
  {
    std::vector<SymbolId> out;
    out.reserve(instruments_.size());
    for (const auto& [id, _] : instruments_)
    {
      out.push_back(id);
    }
    return out;
  }

  size_t size() const noexcept { return instruments_.size(); }

 private:
  std::unordered_map<SymbolId, SymbolConfig> instruments_;
};

}  // namespace flox::venue
