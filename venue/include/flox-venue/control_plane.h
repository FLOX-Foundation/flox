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
