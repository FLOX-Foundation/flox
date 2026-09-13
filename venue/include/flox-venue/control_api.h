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
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <limits>
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
      if (!symbolField(f, "symbol", c.id) || !optionalDecimalField(f, "tick", c.tickSize) ||
          !optionalDecimalField(f, "minPrice", c.minPrice) ||
          !optionalDecimalField(f, "maxPrice", c.maxPrice))
      {
        return err("bad_field");
      }
      if (c.minPrice > c.maxPrice && !c.maxPrice.isZero())
      {
        return err("bad_band");
      }
      if (!reg_.listInstrument(c))
      {
        return err("exists");
      }
      forward(InboundCommand{ListInstrument{c.id, c.tickSize, c.lotSize, c.minPrice, c.maxPrice}});
      return ok();
    }
    if (method == "halt")
    {
      SymbolId sym{};
      bool halted = false;
      if (!symbolField(f, "symbol", sym) || !boolField(f, "halted", halted))
      {
        return err("bad_field");
      }
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
      SymbolId sym{};
      bool open = false;
      if (!symbolField(f, "symbol", sym) || !boolField(f, "open", open))
      {
        return err("bad_field");
      }
      if (reg_.get(sym) == nullptr)
      {
        return err("unknown_symbol");
      }
      forward(InboundCommand{
          AdminCmd{sym, open ? AdminAction::OpenSession : AdminAction::CloseSession}});
      return ok();
    }
    if (method == "setFundingSchedule")
    {
      SymbolId sym{};
      int64_t intervalNs = 0;
      int64_t nextNs = 0;
      if (!symbolField(f, "symbol", sym) || !i64Field(f, "intervalNs", intervalNs) ||
          !i64Field(f, "nextFundingNs", nextNs))
      {
        return err("bad_field");
      }
      if (reg_.get(sym) == nullptr)
      {
        return err("unknown_symbol");
      }
      forward(InboundCommand{
          SetFundingSchedule{sym, DurationNs{intervalNs}, SeqNanos::fromRaw(nextNs)}});
      return ok();
    }
    if (method == "setBand")
    {
      // Both bounds are required. A band is a pair, and half a pair is a
      // request to remove the half that was not named.
      SymbolId sym{};
      Price lo{};
      Price hi{};
      if (!symbolField(f, "symbol", sym) || !decimalField(f, "minPrice", lo) ||
          !decimalField(f, "maxPrice", hi))
      {
        return err("bad_field");
      }
      if (lo > hi)
      {
        return err("bad_band");
      }
      if (!reg_.setPriceBand(sym, lo, hi))
      {
        return err("unknown_symbol");
      }
      forward(InboundCommand{SetBands{sym, lo, hi}});
      return ok();
    }
    if (method == "setTriggerRef")
    {
      SymbolId sym{};
      const std::string refText = get(f, "ref");
      if (!symbolField(f, "symbol", sym) || (refText != "mark" && refText != "last"))
      {
        return err("bad_field");
      }
      const TriggerRef ref = refText == "mark" ? TriggerRef::Mark : TriggerRef::Last;
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
      SymbolId sym{};
      uint64_t account = 0;
      uint64_t group = 0;
      if (!symbolField(f, "symbol", sym) || !u64Field(f, "account", account) ||
          !u64Field(f, "group", group))
      {
        return err("bad_field");
      }
      if (!reg_.get(sym))
      {
        return err("unknown_symbol");
      }
      forward(InboundCommand{SetStpGroup{sym, account, group}});
      return ok();
    }
    if (method == "setRiskLimits")
    {
      // Only the limits named in the request are touched. Replace-all
      // semantics would let an operator raising a position cap silently zero
      // the fat-finger cap by not mentioning it.
      SymbolId sym{};
      if (!symbolField(f, "symbol", sym))
      {
        return err("bad_field");
      }
      if (!reg_.get(sym))
      {
        return err("unknown_symbol");
      }
      // Knobs that travel as a pair are named as a pair. Accepting one half
      // writes a zero into the other, which is the silent-cap-removal this
      // mask was introduced to prevent.
      SetRiskLimits r;
      r.symbol = sym;
      int64_t luldBps = 0;
      int64_t luldHaltNs = 0;
      uint64_t maxOpenOrders = 0;
      uint64_t imBps = 0;
      uint64_t mmBps = 0;
      if (present(f, "luldBps") || present(f, "luldHaltNs"))
      {
        if (!i64Field(f, "luldBps", luldBps) || !i64Field(f, "luldHaltNs", luldHaltNs))
        {
          return err("bad_field");
        }
        r.fields |= RiskLimitField::RiskLuld;
        r.luldBps = static_cast<int32_t>(luldBps);
        r.luldHaltNs = DurationNs{luldHaltNs};
      }
      if (present(f, "maxOrderQty") || present(f, "maxOrderNotional"))
      {
        if (!decimalField(f, "maxOrderQty", r.maxOrderQty) ||
            !decimalField(f, "maxOrderNotional", r.maxOrderNotional))
        {
          return err("bad_field");
        }
        r.fields |= RiskLimitField::RiskFatFinger;
      }
      if (present(f, "maxOpenOrders"))
      {
        if (!u64Field(f, "maxOpenOrders", maxOpenOrders) ||
            maxOpenOrders > (std::numeric_limits<uint32_t>::max)())
        {
          return err("bad_field");
        }
        r.fields |= RiskLimitField::RiskMaxOpenOrders;
        r.maxOpenOrders = static_cast<uint32_t>(maxOpenOrders);
      }
      if (present(f, "maxPositionQty"))
      {
        if (!decimalField(f, "maxPositionQty", r.maxPositionQty))
        {
          return err("bad_field");
        }
        r.fields |= RiskLimitField::RiskMaxPosition;
      }
      if (present(f, "initialMarginBps") || present(f, "maintenanceMarginBps"))
      {
        if (!u64Field(f, "initialMarginBps", imBps) ||
            !u64Field(f, "maintenanceMarginBps", mmBps) ||
            imBps > (std::numeric_limits<int32_t>::max)() ||
            mmBps > (std::numeric_limits<int32_t>::max)())
        {
          return err("bad_field");
        }
        r.fields |= RiskLimitField::RiskMargin;
        r.initialMarginBps = static_cast<int32_t>(imBps);
        r.maintenanceMarginBps = static_cast<int32_t>(mmBps);
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
      SymbolId sym{};
      bool off = false;
      if (!symbolField(f, "symbol", sym) || !boolField(f, "delisted", off))
      {
        return err("bad_field");
      }
      if (!reg_.get(sym))
      {
        return err("unknown_symbol");
      }
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
      SymbolId sym{};
      if (!symbolField(f, "symbol", sym))
      {
        return err("bad_field");
      }
      if (!reg_.get(sym))
      {
        return err("unknown_symbol");
      }
      uint64_t account = 0;
      if (!u64Field(f, "account", account))
      {
        return err("bad_field");
      }
      AdmissionProfile p;
      uint64_t allowedTypes = 0;
      uint64_t allowedTif = 0;
      if ((present(f, "allowedTypes") && !u64Field(f, "allowedTypes", allowedTypes)) ||
          (present(f, "allowedTif") && !u64Field(f, "allowedTif", allowedTif)) ||
          allowedTypes > (std::numeric_limits<uint32_t>::max)() ||
          allowedTif > (std::numeric_limits<uint32_t>::max)())
      {
        return err("bad_field");
      }
      p.allowedTypes = static_cast<uint32_t>(allowedTypes);
      p.allowedTif = static_cast<uint32_t>(allowedTif);
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
      forward(InboundCommand{SetAdmissionProfile{sym, account, p}});
      return ok();
    }
    if (method == "snapshotNow")
    {
      SymbolId sym{};
      if (!symbolField(f, "symbol", sym))
      {
        return err("bad_field");
      }
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
      SymbolId sym{};
      if (!symbolField(f, "symbol", sym))
      {
        return err("bad_field");
      }
      const SymbolConfig* c = reg_.get(sym);
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

  static bool present(const std::unordered_map<std::string, std::string>& f, const char* k)
  {
    return f.count(k) != 0;
  }

  // Numeric accessors that fail instead of guessing.
  //
  // The old accessors read a missing key as an empty string and handed it to
  // strtod, which answers zero. A request that mentioned a symbol and nothing
  // else therefore came back ok having written zeros over a live price band --
  // the collar an operator believed was in place, removed by a request that
  // never named a price. The REST codec next door states the rule these follow:
  // a field is what the caller wrote, never a guessed default.
  //
  // The same accessors also bound the value. A double past the fixed-point
  // range has no representable answer, so it is rejected at the perimeter
  // rather than saturated into a limit nobody asked for.

  static bool doubleField(const std::unordered_map<std::string, std::string>& f, const char* k,
                          double& out)
  {
    auto it = f.find(k);
    if (it == f.end() || it->second.empty())
    {
      return false;
    }
    const std::string& s = it->second;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size())
    {
      return false;  // trailing text, or not a number at all
    }
    if (!std::isfinite(v))
    {
      return false;  // inf / nan, however they were spelled
    }
    out = v;
    return true;
  }

  template <class D>
  static bool decimalField(const std::unordered_map<std::string, std::string>& f, const char* k,
                           D& out)
  {
    double v = 0.0;
    if (!doubleField(f, k, v))
    {
      return false;
    }
    const double scaled = v * static_cast<double>(D::Scale);
    if (!(scaled > -9223372036854775808.0 && scaled < 9223372036854775808.0))
    {
      return false;
    }
    out = D::fromDouble(v);
    return true;
  }

  // Present-and-valid, or absent. Only a present-but-unparseable value fails,
  // which keeps the documented "omitted means unset" fields working.
  template <class D>
  static bool optionalDecimalField(const std::unordered_map<std::string, std::string>& f,
                                   const char* k, D& out)
  {
    return !present(f, k) || decimalField(f, k, out);
  }

  static bool i64Field(const std::unordered_map<std::string, std::string>& f, const char* k,
                       int64_t& out)
  {
    auto it = f.find(k);
    if (it == f.end() || it->second.empty())
    {
      return false;
    }
    const std::string& s = it->second;
    char* end = nullptr;
    errno = 0;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (end != s.c_str() + s.size() || errno == ERANGE)
    {
      return false;
    }
    out = static_cast<int64_t>(v);
    return true;
  }

  static bool u64Field(const std::unordered_map<std::string, std::string>& f, const char* k,
                       uint64_t& out)
  {
    auto it = f.find(k);
    if (it == f.end() || it->second.empty() || it->second[0] == '-')
    {
      return false;
    }
    const std::string& s = it->second;
    char* end = nullptr;
    errno = 0;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    if (end != s.c_str() + s.size() || errno == ERANGE)
    {
      return false;
    }
    out = static_cast<uint64_t>(v);
    return true;
  }

  // Same rule for the flags. "halt" defaulted a missing key to resume and
  // "delist" defaulted it to delist, so the two opposite guesses lived one
  // handler apart.
  static bool boolField(const std::unordered_map<std::string, std::string>& f, const char* k,
                        bool& out)
  {
    auto it = f.find(k);
    if (it == f.end())
    {
      return false;
    }
    if (it->second == "true")
    {
      out = true;
      return true;
    }
    if (it->second == "false")
    {
      out = false;
      return true;
    }
    return false;
  }

  static bool symbolField(const std::unordered_map<std::string, std::string>& f, const char* k,
                          SymbolId& out)
  {
    uint64_t v = 0;
    if (!u64Field(f, k, v) || v > (std::numeric_limits<SymbolId>::max)())
    {
      return false;
    }
    out = static_cast<SymbolId>(v);
    return true;
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
