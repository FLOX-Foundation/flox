/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/engine/symbol_registry.h"
#include <cstddef>
#include <optional>

namespace flox
{

SymbolId SymbolRegistry::registerSymbol(const std::string& exchange, const std::string& symbol)
{
  std::scoped_lock lock(_mutex);
  std::string key = exchange + ":" + symbol;
  auto it = _map.find(key);
  if (it != _map.end())
  {
    return it->second;
  }

  SymbolId id = static_cast<SymbolId>(_reverse.size());
  _map[key] = id;
  _reverse.emplace_back(exchange, symbol);
  return id;
}

SymbolId SymbolRegistry::registerSymbol(const SymbolInfo& info)
{
  std::lock_guard lock(_mutex);
  std::string key = info.exchange + ":" + info.symbol;

  auto it = _map.find(key);
  if (it != _map.end())
  {
    return it->second;
  }

  SymbolId newId = static_cast<SymbolId>(_symbols.size() + 1);
  _map[key] = newId;

  SymbolInfo copy = info;
  copy.id = newId;
  _symbols.push_back(std::move(copy));

  return newId;
}

std::optional<SymbolId> SymbolRegistry::getSymbolId(const std::string& exchange,
                                                    const std::string& symbol) const
{
  std::scoped_lock lock(_mutex);
  std::string key = exchange + ":" + symbol;
  auto it = _map.find(key);
  if (it != _map.end())
  {
    return it->second;
  }
  return std::nullopt;
}

std::pair<std::string, std::string> SymbolRegistry::getSymbolName(SymbolId id) const
{
  std::scoped_lock lock(_mutex);
  return _reverse.at(id);
}

std::optional<SymbolInfo> SymbolRegistry::getSymbolInfo(SymbolId id) const
{
  std::lock_guard lock(_mutex);

  if (id == 0 || id > _symbols.size())
  {
    return std::nullopt;
  }

  return {_symbols[id - 1]};
}
}  // namespace flox
