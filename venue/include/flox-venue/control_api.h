/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/control_plane.h"

#include <cctype>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace flox::venue
{

class ControlApi
{
 public:
  explicit ControlApi(InstrumentRegistry& reg) : reg_(reg) {}

  std::string handle(const std::string& request)
  {
    std::unordered_map<std::string, std::string> f;
    if (!parse(request, f))
    {
      return err("bad_json");
    }
    const std::string method = get(f, "method");

    if (method == "listInstrument")
    {
      SymbolConfig c;
      c.id = symOf(f, "symbol");
      c.tickSize = priceOf(f, "tick");
      c.minPrice = priceOf(f, "minPrice");
      c.maxPrice = priceOf(f, "maxPrice");
      return reg_.listInstrument(c) ? ok() : err("exists");
    }
    if (method == "halt")
    {
      return reg_.halt(symOf(f, "symbol"), get(f, "halted") == "true") ? ok() : err("unknown_symbol");
    }
    if (method == "setBand")
    {
      return reg_.setPriceBand(symOf(f, "symbol"), priceOf(f, "minPrice"), priceOf(f, "maxPrice"))
                 ? ok()
                 : err("unknown_symbol");
    }
    if (method == "setTriggerRef")
    {
      const TriggerRef ref = (get(f, "ref") == "mark") ? TriggerRef::Mark : TriggerRef::Last;
      return reg_.setTriggerRef(symOf(f, "symbol"), ref) ? ok() : err("unknown_symbol");
    }
    if (method == "get")
    {
      const SymbolConfig* c = reg_.get(symOf(f, "symbol"));
      return c ? instrumentJson(*c) : err("unknown_symbol");
    }
    if (method == "list")
    {
      std::string a = "{\"ok\":true,\"instruments\":[";
      bool first = true;
      for (SymbolId id : reg_.list())
      {
        if (!first)
        {
          a += ",";
        }
        a += std::to_string(id);
        first = false;
      }
      a += "]}";
      return a;
    }
    return err("unknown_method");
  }

 private:
  static std::string ok() { return "{\"ok\":true}"; }
  static std::string err(const char* e) { return std::string("{\"ok\":false,\"error\":\"") + e + "\"}"; }

  static std::string instrumentJson(const SymbolConfig& c)
  {
    return std::string("{\"ok\":true,\"symbol\":") + std::to_string(c.id) +
           ",\"tick\":" + std::to_string(c.tickSize.toDouble()) +
           ",\"minPrice\":" + std::to_string(c.minPrice.toDouble()) +
           ",\"maxPrice\":" + std::to_string(c.maxPrice.toDouble()) +
           ",\"halted\":" + (c.halted ? "true" : "false") +
           ",\"triggerRef\":\"" + (c.triggerRef == TriggerRef::Mark ? "mark" : "last") + "\"}";
  }

  static std::string get(const std::unordered_map<std::string, std::string>& f, const char* k)
  {
    auto it = f.find(k);
    return it == f.end() ? std::string{} : it->second;
  }
  static SymbolId symOf(const std::unordered_map<std::string, std::string>& f, const char* k)
  {
    return static_cast<SymbolId>(std::strtoul(get(f, k).c_str(), nullptr, 10));
  }
  static Price priceOf(const std::unordered_map<std::string, std::string>& f, const char* k)
  {
    return Price::fromDouble(std::strtod(get(f, k).c_str(), nullptr));
  }

  static std::string parseString(const std::string& s, size_t& i)
  {
    std::string out;
    ++i;
    while (i < s.size() && s[i] != '"')
    {
      if (s[i] == '\\' && i + 1 < s.size())
      {
        ++i;
      }
      out += s[i++];
    }
    if (i < s.size())
    {
      ++i;
    }
    return out;
  }

  static bool parse(const std::string& s, std::unordered_map<std::string, std::string>& out)
  {
    size_t i = 0;
    auto ws = [&]
    { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))){ ++i;
} };
    ws();
    if (i >= s.size() || s[i] != '{')
    {
      return false;
    }
    ++i;
    ws();
    if (i < s.size() && s[i] == '}')
    {
      return true;
    }
    while (i < s.size())
    {
      ws();
      if (i >= s.size() || s[i] != '"')
      {
        return false;
      }
      const std::string key = parseString(s, i);
      ws();
      if (i >= s.size() || s[i] != ':')
      {
        return false;
      }
      ++i;
      ws();
      std::string val;
      if (i < s.size() && s[i] == '"')
      {
        val = parseString(s, i);
      }
      else
      {
        const size_t st = i;
        while (i < s.size() && s[i] != ',' && s[i] != '}' &&
               !std::isspace(static_cast<unsigned char>(s[i])))
        {
          ++i;
        }
        val = s.substr(st, i - st);
      }
      out[key] = val;
      ws();
      if (i < s.size() && s[i] == ',')
      {
        ++i;
        continue;
      }
      if (i < s.size() && s[i] == '}')
      {
        return true;
      }
      return false;
    }
    return false;
  }

  InstrumentRegistry& reg_;
};

}  // namespace flox::venue
