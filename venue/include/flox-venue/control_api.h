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
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>

namespace flox::venue
{

class ControlApi
{
 public:
  // Configuration mutations must survive a restart, so every successful
  // mutation is also forwarded as an InboundCommand to `sink` -- the caller
  // wires it into the sequenced/journaled path (and to the live engines). On
  // replay, InstrumentRegistry::apply consumes the same records. A read-only
  // deployment may omit the sink.
  using CommandSink = std::function<void(const InboundCommand&)>;

  explicit ControlApi(InstrumentRegistry& reg, CommandSink sink = {})
      : reg_(reg), sink_(std::move(sink))
  {
  }

  // SnapshotNow support: the hook triggers SequencedShard::checkpointNow for
  // the symbol's shard. Deliberately NOT forwarded to the command sink -- a
  // snapshot must never become a journaled, replay-visible record.
  using SnapshotHook = std::function<bool(SymbolId)>;
  void setSnapshotHook(SnapshotHook hook) { snapshotHook_ = std::move(hook); }

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
      if (!reg_.listInstrument(c))
      {
        return err("exists");
      }
      forward(InboundCommand{ListInstrument{c.id, c.tickSize, c.lotSize, c.minPrice, c.maxPrice}});
      return ok();
    }
    if (method == "halt")
    {
      const SymbolId sym = symOf(f, "symbol");
      const bool halted = get(f, "halted") == "true";
      if (!reg_.halt(sym, halted))
      {
        return err("unknown_symbol");
      }
      forward(InboundCommand{AdminCmd{sym, halted ? AdminAction::Halt : AdminAction::Resume}});
      return ok();
    }
    if (method == "session")
    {
      // Session boundary. The engine holds the STATE; the CALENDAR that decides
      // when to call this is the operator's -- a scheduler here, not in the
      // matching path. Registry-invisible (a closed session is not instrument
      // configuration), so only the sequenced AdminCmd is forwarded.
      const SymbolId sym = symOf(f, "symbol");
      if (reg_.get(sym) == nullptr)
      {
        return err("unknown_symbol");
      }
      const bool open = get(f, "open") == "true";
      forward(InboundCommand{
          AdminCmd{sym, open ? AdminAction::OpenSession : AdminAction::CloseSession}});
      return ok();
    }
    if (method == "setFundingSchedule")
    {
      const SymbolId sym = symOf(f, "symbol");
      if (reg_.get(sym) == nullptr)
      {
        return err("unknown_symbol");
      }
      forward(InboundCommand{
          SetFundingSchedule{sym, i64Of(f, "intervalNs"), i64Of(f, "nextFundingNs")}});
      return ok();
    }
    if (method == "setBand")
    {
      const SymbolId sym = symOf(f, "symbol");
      const Price lo = priceOf(f, "minPrice");
      const Price hi = priceOf(f, "maxPrice");
      if (!reg_.setPriceBand(sym, lo, hi))
      {
        return err("unknown_symbol");
      }
      forward(InboundCommand{SetBands{sym, lo, hi}});
      return ok();
    }
    if (method == "setTriggerRef")
    {
      const SymbolId sym = symOf(f, "symbol");
      const TriggerRef ref = (get(f, "ref") == "mark") ? TriggerRef::Mark : TriggerRef::Last;
      if (!reg_.setTriggerRef(sym, ref))
      {
        return err("unknown_symbol");
      }
      forward(InboundCommand{SetTriggerRef{sym, ref}});
      return ok();
    }
    if (method == "setStpGroup")
    {
      // Firm-group STP membership (group 0 removes it). Engine state, not
      // registry state: the forwarded SetStpGroup rides the sequenced /
      // journaled stream and is re-emitted by checkpoints, so it survives
      // replay and recovery like every other matching-relevant mutation.
      const SymbolId sym = symOf(f, "symbol");
      if (!reg_.get(sym))
      {
        return err("unknown_symbol");
      }
      forward(InboundCommand{SetStpGroup{sym, u64Of(f, "account"), u64Of(f, "group")}});
      return ok();
    }
    if (method == "setRiskLimits")
    {
      // Only the limits named in the request are touched. Replace-all
      // semantics would let an operator raising a position cap silently zero
      // the fat-finger cap by not mentioning it.
      const SymbolId sym = symOf(f, "symbol");
      if (!reg_.get(sym))
      {
        return err("unknown_symbol");
      }
      SetRiskLimits r;
      r.symbol = sym;
      if (f.count("luldBps") != 0 || f.count("luldHaltNs") != 0)
      {
        r.fields |= RiskLimitField::RiskLuld;
        r.luldBps = static_cast<int32_t>(i64Of(f, "luldBps"));
        r.luldHaltNs = i64Of(f, "luldHaltNs");
      }
      if (f.count("maxOrderQty") != 0 || f.count("maxOrderNotional") != 0)
      {
        r.fields |= RiskLimitField::RiskFatFinger;
        r.maxOrderQty = qtyOf(f, "maxOrderQty");
        r.maxOrderNotional = volOf(f, "maxOrderNotional");
      }
      if (f.count("maxOpenOrders") != 0)
      {
        r.fields |= RiskLimitField::RiskMaxOpenOrders;
        r.maxOpenOrders = static_cast<uint32_t>(u64Of(f, "maxOpenOrders"));
      }
      if (f.count("maxPositionQty") != 0)
      {
        r.fields |= RiskLimitField::RiskMaxPosition;
        r.maxPositionQty = qtyOf(f, "maxPositionQty");
      }
      if (f.count("initialMarginBps") != 0 || f.count("maintenanceMarginBps") != 0)
      {
        r.fields |= RiskLimitField::RiskMargin;
        r.initialMarginBps = static_cast<int32_t>(u64Of(f, "initialMarginBps"));
        r.maintenanceMarginBps = static_cast<int32_t>(u64Of(f, "maintenanceMarginBps"));
      }
      if (r.fields == 0)
      {
        return err("no_limits_named");
      }
      forward(InboundCommand{r});
      return ok();
    }
    if (method == "delist")
    {
      // Withdraw from trading with no scheduled return: the resting book is
      // pulled, unlike a halt or a session close. Reversible by delist=false.
      const SymbolId sym = symOf(f, "symbol");
      if (!reg_.get(sym))
      {
        return err("unknown_symbol");
      }
      const bool off = get(f, "delisted") != "false";
      forward(InboundCommand{
          AdminCmd{sym, off ? AdminAction::Delist : AdminAction::Relist}});
      return ok();
    }
    if (method == "setAdmissionProfile")
    {
      // What a counterparty may send. Engine state on the same footing as the
      // STP groups above: the forwarded command is sequenced and journaled, so
      // an entitlement survives replay and recovery rather than living only in
      // whatever process happened to set it.
      //
      // Omitted type/tif lists mean "no restriction on that axis", which is
      // how an operator narrows one dimension without having to enumerate
      // every value of the other.
      const SymbolId sym = symOf(f, "symbol");
      if (!reg_.get(sym))
      {
        return err("unknown_symbol");
      }
      AdmissionProfile p;
      p.allowedTypes = static_cast<uint32_t>(u64Of(f, "allowedTypes"));
      p.allowedTif = static_cast<uint32_t>(u64Of(f, "allowedTif"));
      uint8_t deny = 0;
      if (get(f, "denyResting") == "true")
      {
        deny |= AdmissionDeny::DenyResting;
      }
      if (get(f, "denyAmend") == "true")
      {
        deny |= AdmissionDeny::DenyAmend;
      }
      if (get(f, "denyCancel") == "true")
      {
        deny |= AdmissionDeny::DenyCancel;
      }
      if (get(f, "denyQuote") == "true")
      {
        deny |= AdmissionDeny::DenyQuote;
      }
      p.deny = deny;
      forward(InboundCommand{SetAdmissionProfile{sym, u64Of(f, "account"), p}});
      return ok();
    }
    if (method == "snapshotNow")
    {
      const SymbolId sym = symOf(f, "symbol");
      if (!reg_.get(sym))
      {
        return err("unknown_symbol");
      }
      if (!snapshotHook_)
      {
        return err("unsupported");
      }
      return snapshotHook_(sym) ? ok() : err("snapshot_failed");
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
  void forward(const InboundCommand& cmd)
  {
    if (sink_)
    {
      sink_(cmd);
    }
  }

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
  static uint64_t u64Of(const std::unordered_map<std::string, std::string>& f, const char* k)
  {
    return std::strtoull(get(f, k).c_str(), nullptr, 10);
  }
  static int64_t i64Of(const std::unordered_map<std::string, std::string>& f, const char* k)
  {
    return static_cast<int64_t>(std::strtoll(get(f, k).c_str(), nullptr, 10));
  }
  static Price priceOf(const std::unordered_map<std::string, std::string>& f, const char* k)
  {
    return Price::fromDouble(std::strtod(get(f, k).c_str(), nullptr));
  }
  static Quantity qtyOf(const std::unordered_map<std::string, std::string>& f, const char* k)
  {
    return Quantity::fromDouble(std::strtod(get(f, k).c_str(), nullptr));
  }
  static Volume volOf(const std::unordered_map<std::string, std::string>& f, const char* k)
  {
    return Volume::fromDouble(std::strtod(get(f, k).c_str(), nullptr));
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
  CommandSink sink_;
  SnapshotHook snapshotHook_;
};

}  // namespace flox::venue
