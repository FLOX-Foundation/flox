/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "flox/common.h"
#include "flox/engine/abstract_subsystem.h"

namespace flox
{

struct SymbolInfo
{
  SymbolId id;
  std::string exchange;
  std::string symbol;
  InstrumentType type = InstrumentType::Spot;

  std::optional<Price> strike;
  std::optional<TimePoint> expiry;
  std::optional<OptionType> optionType;
};

class SymbolRegistry : public ISubsystem
{
 public:
  SymbolId registerSymbol(const std::string& exchange, const std::string& symbol);
  SymbolId registerSymbol(const SymbolInfo& info);

  std::optional<SymbolId> getSymbolId(const std::string& exchange,
                                      const std::string& symbol) const;

  std::optional<SymbolInfo> getSymbolInfo(SymbolId id) const;

  std::pair<std::string, std::string> getSymbolName(SymbolId id) const;

 private:
  mutable std::mutex _mutex;
  std::vector<SymbolInfo> _symbols;
  std::unordered_map<std::string, SymbolId> _map;
  std::vector<std::pair<std::string, std::string>> _reverse;
};

}  // namespace flox
