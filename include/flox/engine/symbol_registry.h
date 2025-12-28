/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "flox/common.h"
#include "flox/engine/abstract_subsystem.h"
#include "flox/exchange/exchange_info.h"
#include "flox/util/base/time.h"

namespace flox
{

struct SymbolInfo
{
  SymbolId id{0};
  std::string exchange;
  std::string symbol;
  InstrumentType type = InstrumentType::Spot;
  Price tickSize{Price::fromDouble(0.01)};
  ExchangeId exchangeId{InvalidExchangeId};

  std::optional<Price> strike;
  std::optional<TimePoint> expiry;
  std::optional<OptionType> optionType;
};

class SymbolRegistry : public ISubsystem
{
 public:
  static constexpr size_t kMaxExchanges = 32;
  static constexpr size_t kMaxSymbols = 4096;
  static constexpr size_t kMaxEquivalentsPerSymbol = 8;

  // Exchange management
  ExchangeId registerExchange(std::string_view name,
                              VenueType type = VenueType::CentralizedExchange);
  const ExchangeInfo* getExchange(ExchangeId id) const;
  ExchangeId getExchangeId(std::string_view name) const;
  size_t exchangeCount() const { return _numExchanges; }

  // Symbol registration (legacy string-based API)
  SymbolId registerSymbol(const std::string& exchange, const std::string& symbol);
  SymbolId registerSymbol(const SymbolInfo& info);
  std::optional<SymbolId> getSymbolId(const std::string& exchange,
                                      const std::string& symbol) const;
  std::optional<SymbolInfo> getSymbolInfo(SymbolId id) const;
  std::pair<std::string, std::string> getSymbolName(SymbolId id) const;

  // Symbol registration (ExchangeId-based API)
  SymbolId registerSymbol(ExchangeId exchange, std::string_view symbol);
  ExchangeId getExchangeForSymbol(SymbolId symbol) const;

  // Symbol equivalence (cross-exchange mapping)
  void mapEquivalentSymbols(std::span<const SymbolId> equivalentSymbols);
  std::span<const SymbolId> getEquivalentSymbols(SymbolId symbol) const;
  SymbolId getEquivalentOnExchange(SymbolId symbol, ExchangeId exchange) const;

  // Persistence
  bool saveToFile(const std::filesystem::path& path) const;
  bool loadFromFile(const std::filesystem::path& path);

  std::vector<std::byte> serialize() const;
  bool deserialize(std::span<const std::byte> data);

  void clear();
  std::vector<SymbolInfo> getAllSymbols() const;
  size_t size() const;

 private:
  mutable std::mutex _mutex;
  std::unordered_map<SymbolId, SymbolInfo> _symbols;
  std::unordered_map<std::string, SymbolId> _map;
  std::vector<std::pair<std::string, std::string>> _reverse;

  // Exchange storage
  std::array<ExchangeInfo, kMaxExchanges> _exchanges{};
  std::unordered_map<std::string, ExchangeId> _exchangeNameToId;
  size_t _numExchanges{0};

  // Symbol-to-exchange mapping (O(1) lookup)
  std::vector<ExchangeId> _symbolToExchange;

  // Equivalence: flat storage for O(1) lookup
  // [sym * kMaxEquivalentsPerSymbol ... (sym+1) * kMaxEquivalentsPerSymbol)
  std::vector<SymbolId> _equivalents;
  std::vector<uint8_t> _equivalentCounts;
};

}  // namespace flox
