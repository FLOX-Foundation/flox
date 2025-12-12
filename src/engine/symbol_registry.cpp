/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/engine/symbol_registry.h"
#include "flox/util/performance/profile.h"

#include <cstdio>
#include <optional>
#include <span>

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
  FLOX_PROFILE_SCOPE("SymbolRegistry::getSymbolId");

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
  FLOX_PROFILE_SCOPE("SymbolRegistry::getSymbolInfo");

  std::lock_guard lock(_mutex);

  if (id == 0 || id > _symbols.size())
  {
    return std::nullopt;
  }

  return {_symbols[id - 1]};
}

void SymbolRegistry::clear()
{
  std::lock_guard lock(_mutex);
  _symbols.clear();
  _map.clear();
  _reverse.clear();
}

std::vector<SymbolInfo> SymbolRegistry::getAllSymbols() const
{
  std::lock_guard lock(_mutex);
  return _symbols;
}

size_t SymbolRegistry::size() const
{
  std::lock_guard lock(_mutex);
  return _symbols.size();
}

bool SymbolRegistry::saveToFile(const std::filesystem::path& path) const
{
  std::lock_guard lock(_mutex);

  std::FILE* f = std::fopen(path.string().c_str(), "w");
  if (!f)
  {
    return false;
  }

  // Simple JSON-like format
  std::fprintf(f, "{\n  \"version\": 1,\n  \"symbols\": [\n");

  for (size_t i = 0; i < _symbols.size(); ++i)
  {
    const auto& sym = _symbols[i];
    std::fprintf(f, "    {\"id\": %u, \"exchange\": \"%s\", \"symbol\": \"%s\", \"type\": %d",
                 sym.id, sym.exchange.c_str(), sym.symbol.c_str(), static_cast<int>(sym.type));

    if (sym.strike.has_value())
    {
      std::fprintf(f, ", \"strike\": %lld", static_cast<long long>(sym.strike->raw()));
    }
    if (sym.expiry.has_value())
    {
      auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    sym.expiry->time_since_epoch())
                    .count();
      std::fprintf(f, ", \"expiry\": %lld", static_cast<long long>(ns));
    }
    if (sym.optionType.has_value())
    {
      std::fprintf(f, ", \"option_type\": %d", static_cast<int>(*sym.optionType));
    }

    std::fprintf(f, "}%s\n", (i < _symbols.size() - 1) ? "," : "");
  }

  std::fprintf(f, "  ]\n}\n");
  std::fclose(f);
  return true;
}

bool SymbolRegistry::loadFromFile(const std::filesystem::path& path)
{
  std::FILE* f = std::fopen(path.string().c_str(), "r");
  if (!f)
  {
    return false;
  }

  // Read entire file
  std::fseek(f, 0, SEEK_END);
  long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);

  std::string content(size, '\0');
  std::fread(content.data(), 1, size, f);
  std::fclose(f);

  // Simple parsing - look for symbol entries
  // Format: {"id": N, "exchange": "X", "symbol": "Y", "type": T, ...}

  std::lock_guard lock(_mutex);
  _symbols.clear();
  _map.clear();
  _reverse.clear();

  size_t pos = 0;
  while ((pos = content.find("\"id\":", pos)) != std::string::npos)
  {
    SymbolInfo info;

    // Parse id
    pos += 5;
    info.id = static_cast<SymbolId>(std::stoul(content.substr(pos)));

    // Parse exchange
    size_t exch_start = content.find("\"exchange\":", pos);
    if (exch_start == std::string::npos)
    {
      break;
    }
    exch_start = content.find('"', exch_start + 11) + 1;
    size_t exch_end = content.find('"', exch_start);
    info.exchange = content.substr(exch_start, exch_end - exch_start);

    // Parse symbol
    size_t sym_start = content.find("\"symbol\":", pos);
    if (sym_start == std::string::npos)
    {
      break;
    }
    sym_start = content.find('"', sym_start + 9) + 1;
    size_t sym_end = content.find('"', sym_start);
    info.symbol = content.substr(sym_start, sym_end - sym_start);

    // Parse type
    size_t type_start = content.find("\"type\":", pos);
    if (type_start != std::string::npos && type_start < content.find('}', pos))
    {
      type_start += 7;
      info.type = static_cast<InstrumentType>(std::stoi(content.substr(type_start)));
    }

    // Optional fields - strike
    size_t strike_start = content.find("\"strike\":", pos);
    size_t entry_end = content.find('}', pos);
    if (strike_start != std::string::npos && strike_start < entry_end)
    {
      strike_start += 9;
      info.strike = Price::fromRaw(std::stol(content.substr(strike_start)));
    }

    // Optional - expiry
    size_t expiry_start = content.find("\"expiry\":", pos);
    if (expiry_start != std::string::npos && expiry_start < entry_end)
    {
      expiry_start += 9;
      auto ns = std::stol(content.substr(expiry_start));
      info.expiry = TimePoint(std::chrono::nanoseconds(ns));
    }

    // Optional - option_type
    size_t opt_start = content.find("\"option_type\":", pos);
    if (opt_start != std::string::npos && opt_start < entry_end)
    {
      opt_start += 14;
      info.optionType = static_cast<OptionType>(std::stoi(content.substr(opt_start)));
    }

    // Add to registry
    std::string key = info.exchange + ":" + info.symbol;
    _map[key] = info.id;
    _symbols.push_back(std::move(info));
    _reverse.emplace_back(info.exchange, info.symbol);

    pos = entry_end + 1;
  }

  return !_symbols.empty() || content.find("\"symbols\": []") != std::string::npos;
}

// Binary format:
// [4 bytes] magic: "SREG"
// [4 bytes] version
// [4 bytes] symbol count
// For each symbol:
//   [4 bytes] id
//   [2 bytes] exchange length
//   [N bytes] exchange string
//   [2 bytes] symbol length
//   [N bytes] symbol string
//   [1 byte] instrument type
//   [1 byte] flags (has_strike, has_expiry, has_option_type)
//   [8 bytes] strike (if flag set)
//   [8 bytes] expiry (if flag set)
//   [1 byte] option_type (if flag set)

static constexpr uint32_t kSymbolRegistryMagic = 0x47455253;  // "SREG"
static constexpr uint32_t kSymbolRegistryVersion = 1;

std::vector<std::byte> SymbolRegistry::serialize() const
{
  std::lock_guard lock(_mutex);

  std::vector<std::byte> result;
  result.reserve(1024 + _symbols.size() * 64);  // Estimate

  auto write_u32 = [&result](uint32_t v)
  {
    result.push_back(static_cast<std::byte>(v & 0xFF));
    result.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
    result.push_back(static_cast<std::byte>((v >> 16) & 0xFF));
    result.push_back(static_cast<std::byte>((v >> 24) & 0xFF));
  };

  auto write_u16 = [&result](uint16_t v)
  {
    result.push_back(static_cast<std::byte>(v & 0xFF));
    result.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
  };

  auto write_u8 = [&result](uint8_t v)
  { result.push_back(static_cast<std::byte>(v)); };

  auto write_i64 = [&result](int64_t v)
  {
    for (int i = 0; i < 8; ++i)
    {
      result.push_back(static_cast<std::byte>((v >> (i * 8)) & 0xFF));
    }
  };

  auto write_string = [&](const std::string& s)
  {
    write_u16(static_cast<uint16_t>(s.size()));
    for (char c : s)
    {
      result.push_back(static_cast<std::byte>(c));
    }
  };

  // Header
  write_u32(kSymbolRegistryMagic);
  write_u32(kSymbolRegistryVersion);
  write_u32(static_cast<uint32_t>(_symbols.size()));

  // Symbols
  for (const auto& sym : _symbols)
  {
    write_u32(sym.id);
    write_string(sym.exchange);
    write_string(sym.symbol);
    write_u8(static_cast<uint8_t>(sym.type));

    uint8_t flags = 0;
    if (sym.strike.has_value())
    {
      flags |= 0x01;
    }
    if (sym.expiry.has_value())
    {
      flags |= 0x02;
    }
    if (sym.optionType.has_value())
    {
      flags |= 0x04;
    }
    write_u8(flags);

    if (sym.strike.has_value())
    {
      write_i64(sym.strike->raw());
    }
    if (sym.expiry.has_value())
    {
      auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    sym.expiry->time_since_epoch())
                    .count();
      write_i64(ns);
    }
    if (sym.optionType.has_value())
    {
      write_u8(static_cast<uint8_t>(*sym.optionType));
    }
  }

  return result;
}

bool SymbolRegistry::deserialize(std::span<const std::byte> data)
{
  if (data.size() < 12)
  {
    return false;  // Minimum header size
  }

  size_t pos = 0;

  auto read_u32 = [&data, &pos]() -> uint32_t
  {
    if (pos + 4 > data.size())
    {
      return 0;
    }
    uint32_t v = static_cast<uint32_t>(data[pos]) | (static_cast<uint32_t>(data[pos + 1]) << 8) |
                 (static_cast<uint32_t>(data[pos + 2]) << 16) |
                 (static_cast<uint32_t>(data[pos + 3]) << 24);
    pos += 4;
    return v;
  };

  auto read_u16 = [&data, &pos]() -> uint16_t
  {
    if (pos + 2 > data.size())
    {
      return 0;
    }
    uint16_t v = static_cast<uint16_t>(data[pos]) | (static_cast<uint16_t>(data[pos + 1]) << 8);
    pos += 2;
    return v;
  };

  auto read_u8 = [&data, &pos]() -> uint8_t
  {
    if (pos >= data.size())
    {
      return 0;
    }
    return static_cast<uint8_t>(data[pos++]);
  };

  auto read_i64 = [&data, &pos]() -> int64_t
  {
    if (pos + 8 > data.size())
    {
      return 0;
    }
    int64_t v = 0;
    for (int i = 0; i < 8; ++i)
    {
      v |= static_cast<int64_t>(static_cast<uint8_t>(data[pos + i])) << (i * 8);
    }
    pos += 8;
    return v;
  };

  auto read_string = [&data, &pos, &read_u16]() -> std::string
  {
    uint16_t len = read_u16();
    if (pos + len > data.size())
    {
      return "";
    }
    std::string s(len, '\0');
    for (uint16_t i = 0; i < len; ++i)
    {
      s[i] = static_cast<char>(data[pos + i]);
    }
    pos += len;
    return s;
  };

  // Read header
  uint32_t magic = read_u32();
  if (magic != kSymbolRegistryMagic)
  {
    return false;
  }

  uint32_t version = read_u32();
  if (version != kSymbolRegistryVersion)
  {
    return false;
  }

  uint32_t count = read_u32();

  std::lock_guard lock(_mutex);
  _symbols.clear();
  _map.clear();
  _reverse.clear();

  _symbols.reserve(count);

  for (uint32_t i = 0; i < count; ++i)
  {
    SymbolInfo info;
    info.id = read_u32();
    info.exchange = read_string();
    info.symbol = read_string();
    info.type = static_cast<InstrumentType>(read_u8());

    uint8_t flags = read_u8();

    if (flags & 0x01)
    {
      info.strike = Price::fromRaw(read_i64());
    }
    if (flags & 0x02)
    {
      info.expiry = TimePoint(std::chrono::nanoseconds(read_i64()));
    }
    if (flags & 0x04)
    {
      info.optionType = static_cast<OptionType>(read_u8());
    }

    std::string key = info.exchange + ":" + info.symbol;
    _map[key] = info.id;
    _symbols.push_back(std::move(info));
    _reverse.emplace_back(info.exchange, info.symbol);
  }

  return true;
}

}  // namespace flox
