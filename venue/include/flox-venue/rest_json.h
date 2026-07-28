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

#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_map>

namespace flox::venue
{

class RestJson
{
 public:
  // ---- inbound: JSON body -> InboundCommand ----
  // Schema: {"action":"new|cancel|modify", ...fields}
  static std::optional<InboundCommand> decode(const std::string& json)
  {
    std::unordered_map<std::string, std::string> f;
    if (!parse(json, f))
    {
      return std::nullopt;
    }
    auto has = [&](const char* k)
    { return f.count(k) != 0; };
    auto s = [&](const char* k)
    { return has(k) ? f[k] : std::string{}; };
    auto u64 = [&](const char* k)
    { return static_cast<uint64_t>(std::strtoull(s(k).c_str(), nullptr, 10)); };
    auto sym = [&](const char* k)
    { return static_cast<SymbolId>(std::strtoul(s(k).c_str(), nullptr, 10)); };
    auto price = [&](const char* k)
    { return safeDecimal<Price>(std::strtod(s(k).c_str(), nullptr)); };
    auto qtyf = [&](const char* k)
    { return safeDecimal<Quantity>(std::strtod(s(k).c_str(), nullptr)); };

    const std::string action = s("action");
    if (action == "new")
    {
      NewOrder o;
      o.id = u64("id");
      o.clientOrderId = has("clientOrderId") ? u64("clientOrderId") : o.id;
      o.symbol = sym("symbol");
      o.side = (s("side") == "sell") ? Side::SELL : Side::BUY;
      o.quantity = qtyf("qty");
      o.accountId = u64("account");
      const std::string t = s("ordType");
      if (t == "market")
      {
        o.type = OrderType::MARKET;
      }
      else if (t == "stop")
      {
        o.type = OrderType::STOP_MARKET;
      }
      else if (t == "stop_limit")
      {
        o.type = OrderType::STOP_LIMIT;
      }
      else if (t == "take_profit")
      {
        o.type = OrderType::TAKE_PROFIT_MARKET;
      }
      else if (t == "trailing")
      {
        o.type = OrderType::TRAILING_STOP;
      }
      else
      {
        o.type = OrderType::LIMIT;
      }
      if (has("price"))
      {
        o.price = price("price");
      }
      if (has("trigger"))
      {
        o.triggerPrice = price("trigger");
      }
      if (has("trailingOffset"))
      {
        o.trailingOffset = price("trailingOffset");
      }
      if (has("visible"))
      {
        o.visibleQuantity = qtyf("visible");
      }
      const std::string tif = s("tif");
      if (tif == "ioc")
      {
        o.tif = TimeInForce::IOC;
      }
      else if (tif == "fok")
      {
        o.tif = TimeInForce::FOK;
      }
      else
      {
        o.tif = TimeInForce::GTC;
      }
      if (s("postOnly") == "true")
      {
        o.postOnly = true;
      }
      if (s("reduceOnly") == "true")
      {
        o.reduceOnly = true;  // derivatives: never open
      }
      const std::string stp = s("stp");  // self-trade prevention mode
      if (stp == "cancelNewest")
      {
        o.stp = STPMode::CancelNewest;
      }
      else if (stp == "cancelOldest")
      {
        o.stp = STPMode::CancelOldest;
      }
      else if (stp == "cancelBoth")
      {
        o.stp = STPMode::CancelBoth;
      }
      else if (stp == "decrement")
      {
        o.stp = STPMode::Decrement;
      }
      return InboundCommand{o};
    }
    if (action == "cancel")
    {
      return InboundCommand{CancelOrder{u64("id"), sym("symbol"), u64("account")}};
    }
    if (action == "modify")
    {
      ModifyOrder m;
      m.id = u64("id");
      m.symbol = sym("symbol");
      if (has("price"))
      {
        m.newPrice = price("price");
      }
      m.newQty = qtyf("qty");
      m.accountId = u64("account");
      return InboundCommand{m};
    }
    return std::nullopt;
  }

  // ---- outbound: OutboundEvent -> JSON ----
  static std::string encode(const OutboundEvent& ev)
  {
    std::string o = "{";
    auto kv = [&](const char* k, const std::string& v, bool quote)
    {
      if (o.size() > 1)
      {
        o += ",";
      }
      o += "\"";
      o += k;
      o += "\":";
      if (quote)
      {
        o += "\"";
      }
      o += v;
      if (quote)
      {
        o += "\"";
      }
    };
    auto id = [](uint64_t v)
    { return std::to_string(v); };
    auto pr = [](Price p)
    { return std::to_string(p.toDouble()); };
    auto qn = [](Quantity q)
    { return std::to_string(q.toDouble()); };

    if (const auto* a = std::get_if<OrderAccepted>(&ev))
    {
      kv("type", "accepted", true);
      kv("id", id(a->id), false);
      kv("side", a->side == Side::SELL ? "sell" : "buy", true);
      kv("price", pr(a->price), false);
      kv("leaves", qn(a->leavesQty), false);
      kv("onBook", a->restingOnBook ? "true" : "false", false);
    }
    else if (const auto* t = std::get_if<Trade>(&ev))
    {
      kv("type", "trade", true);
      kv("tradeId", id(t->tradeId), false);
      kv("price", pr(t->price), false);
      kv("qty", qn(t->quantity), false);
      kv("maker", id(t->makerId), false);
      kv("taker", id(t->takerId), false);
    }
    else if (const auto* x = std::get_if<OrderExecuted>(&ev))
    {
      kv("type", "executed", true);
      kv("id", id(x->id), false);
      kv("lastQty", qn(x->lastQty), false);
      kv("leaves", qn(x->leavesQty), false);
      kv("complete", x->complete ? "true" : "false", false);
    }
    else if (const auto* c = std::get_if<OrderCanceled>(&ev))
    {
      kv("type", "canceled", true);
      kv("id", id(c->id), false);
      kv("reason", toString(c->reason), true);
    }
    else if (const auto* j = std::get_if<OrderRejected>(&ev))
    {
      kv("type", "rejected", true);
      kv("id", id(j->id), false);
      kv("reason", toString(j->reason), true);
    }
    else if (const auto* m = std::get_if<OrderModified>(&ev))
    {
      kv("type", "modified", true);
      kv("id", id(m->id), false);
      kv("price", pr(m->price), false);
      kv("leaves", qn(m->leavesQty), false);
    }
    else if (const auto* g = std::get_if<OrderTriggered>(&ev))
    {
      kv("type", "triggered", true);
      kv("id", id(g->id), false);
      kv("refPrice", pr(g->refPrice), false);
    }
    o += "}";
    return o;
  }

 private:
  static std::string parseString(const std::string& s, size_t& i)
  {
    std::string out;
    ++i;  // opening quote
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
      ++i;  // closing quote
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
};

}  // namespace flox::venue
