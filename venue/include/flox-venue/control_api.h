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
#include <string_view>
#include <unordered_map>
#include <utility>

namespace flox::venue
{

// One parsed control request: the fields the caller named, the registry a verb
// validates against, and the journaled path a mutation travels. Both the
// built-in verbs and the methods a deployment registers read a request through
// this, so an added verb inherits the rule the built-ins follow rather than
// re-deriving it -- a field nobody named stays absent instead of becoming a
// zero.
class ControlRequest
{
 public:
  using Fields = std::unordered_map<std::string, std::string>;
  using CommandSink = std::function<void(const InboundCommand&)>;

  ControlRequest(const Fields& fields, InstrumentRegistry& reg, const CommandSink& sink)
      : fields_(fields), reg_(reg), sink_(sink)
  {
  }

  InstrumentRegistry& registry() const noexcept { return reg_; }

  // The journaling rule, in one call. A verb that changes engine state forwards
  // the record that reproduces the change on replay; a verb that only reads
  // never calls this. A deployment that wired no sink is the read-only case.
  void forward(const InboundCommand& cmd) const
  {
    if (sink_)
    {
      sink_(cmd);
    }
  }

  std::string text(const char* k) const
  {
    auto it = fields_.find(k);
    return it == fields_.end() ? std::string{} : it->second;
  }

  bool present(const char* k) const
  {
    return fields_.count(k) != 0;
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

  bool doubleField(const char* k,
                   double& out) const
  {
    auto it = fields_.find(k);
    if (it == fields_.end() || it->second.empty())
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
  bool decimalField(const char* k,
                    D& out) const
  {
    double v = 0.0;
    if (!doubleField(k, v))
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
  bool optionalDecimalField(const char* k, D& out) const
  {
    return !present(k) || decimalField(k, out);
  }

  bool i64Field(const char* k,
                int64_t& out) const
  {
    auto it = fields_.find(k);
    if (it == fields_.end() || it->second.empty())
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

  bool u64Field(const char* k,
                uint64_t& out) const
  {
    auto it = fields_.find(k);
    if (it == fields_.end() || it->second.empty() || it->second[0] == '-')
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
  bool boolField(const char* k,
                 bool& out) const
  {
    auto it = fields_.find(k);
    if (it == fields_.end())
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

  bool symbolField(const char* k,
                   SymbolId& out) const
  {
    uint64_t v = 0;
    if (!u64Field(k, v) || v > (std::numeric_limits<SymbolId>::max)())
    {
      return false;
    }
    out = static_cast<SymbolId>(v);
    return true;
  }

 private:
  const Fields& fields_;
  InstrumentRegistry& reg_;
  const CommandSink& sink_;
};

class ControlApi
{
 public:
  // Configuration mutations must survive a restart, so every successful
  // mutation is also forwarded as an InboundCommand to `sink` -- the caller
  // wires it into the sequenced/journaled path (and to the live engines). On
  // replay, InstrumentRegistry::apply consumes the same records. A read-only
  // deployment may omit the sink.
  using CommandSink = ControlRequest::CommandSink;

  explicit ControlApi(InstrumentRegistry& reg, CommandSink sink = {})
      : reg_(reg), sink_(std::move(sink))
  {
  }

  // SnapshotNow support: the hook triggers SequencedShard::checkpointNow for
  // the symbol's shard. Deliberately NOT forwarded to the command sink -- a
  // snapshot must never become a journaled, replay-visible record.
  using SnapshotHook = std::function<bool(SymbolId)>;
  void setSnapshotHook(SnapshotHook hook) { snapshotHook_ = std::move(hook); }

  // A verb the deployment adds to this surface. The handler answers the way a
  // built-in does -- ok(), err("..."), or a JSON object of its own -- and
  // reaches the venue through the request: registry() to validate against,
  // forward() for a change that must journal. Nothing in ControlServer or
  // TcpControlServer knows the difference, so a registered verb is served by
  // the existing accept loop and line framing.
  using Method = std::function<std::string(const ControlRequest&)>;

  // Wiring time only. handle() reads the table without a lock, so every
  // registration belongs before a server starts accepting on this api.
  //
  // Refused: an empty name, an empty handler, a name a built-in verb already
  // answers, and a second registration of a name already taken. A registration
  // that silently replaced another -- or that was silently shadowed by a
  // built-in -- routes operator traffic to a handler other than the one whose
  // name was typed.
  bool registerMethod(std::string name, Method handler)
  {
    if (name.empty() || !handler || isBuiltinMethod(name))
    {
      return false;
    }
    return methods_.emplace(std::move(name), std::move(handler)).second;
  }

  // Every verb handle() answers itself. This list is what registerMethod
  // refuses, so a built-in added to handle() has to be added here too;
  // test_venue_control_methods walks it and fails on a name handle() does not
  // know.
  static constexpr std::string_view kBuiltinMethods[] = {
      "listInstrument", "halt", "session", "setFundingSchedule",
      "setBand", "setTriggerRef", "setStpGroup", "setRiskLimits",
      "delist", "setAdmissionProfile", "snapshotNow", "get",
      "list"};

  static bool isBuiltinMethod(std::string_view name)
  {
    for (std::string_view v : kBuiltinMethods)
    {
      if (v == name)
      {
        return true;
      }
    }
    return false;
  }

  // The response shapes, so a registered method answers in the same one.
  static std::string ok() { return "{\"ok\":true}"; }
  static std::string err(const char* e) { return std::string("{\"ok\":false,\"error\":\"") + e + "\"}"; }

  std::string handle(const std::string& request)
  {
    std::unordered_map<std::string, std::string> f;
    if (!parse(request, f))
    {
      return err("bad_json");
    }
    const ControlRequest req(f, reg_, sink_);
    const std::string method = req.text("method");

    if (method == "listInstrument")
    {
      SymbolConfig c;
      if (!req.symbolField("symbol", c.id) || !req.optionalDecimalField("tick", c.tickSize) ||
          !req.optionalDecimalField("minPrice", c.minPrice) ||
          !req.optionalDecimalField("maxPrice", c.maxPrice))
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
      forward(InboundCommand{ListInstrument{c.id, {}, c.tickSize, c.lotSize, c.minPrice, c.maxPrice}});
      return ok();
    }
    if (method == "halt")
    {
      SymbolId sym{};
      bool halted = false;
      if (!req.symbolField("symbol", sym) || !req.boolField("halted", halted))
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
      if (!req.symbolField("symbol", sym) || !req.boolField("open", open))
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
      if (!req.symbolField("symbol", sym) || !req.i64Field("intervalNs", intervalNs) ||
          !req.i64Field("nextFundingNs", nextNs))
      {
        return err("bad_field");
      }
      if (reg_.get(sym) == nullptr)
      {
        return err("unknown_symbol");
      }
      forward(InboundCommand{
          SetFundingSchedule{sym, {}, DurationNs{intervalNs}, SeqNanos::fromRaw(nextNs)}});
      return ok();
    }
    if (method == "setBand")
    {
      // Both bounds are required. A band is a pair, and half a pair is a
      // request to remove the half that was not named.
      SymbolId sym{};
      Price lo{};
      Price hi{};
      if (!req.symbolField("symbol", sym) || !req.decimalField("minPrice", lo) ||
          !req.decimalField("maxPrice", hi))
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
      forward(InboundCommand{SetBands{sym, {}, lo, hi}});
      return ok();
    }
    if (method == "setTriggerRef")
    {
      SymbolId sym{};
      const std::string refText = req.text("ref");
      if (!req.symbolField("symbol", sym) || (refText != "mark" && refText != "last"))
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
      if (!req.symbolField("symbol", sym) || !req.u64Field("account", account) ||
          !req.u64Field("group", group))
      {
        return err("bad_field");
      }
      if (!reg_.get(sym))
      {
        return err("unknown_symbol");
      }
      forward(InboundCommand{SetStpGroup{sym, {}, account, group}});
      return ok();
    }
    if (method == "setRiskLimits")
    {
      // Only the limits named in the request are touched. Replace-all
      // semantics would let an operator raising a position cap silently zero
      // the fat-finger cap by not mentioning it.
      SymbolId sym{};
      if (!req.symbolField("symbol", sym))
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
      if (req.present("luldBps") || req.present("luldHaltNs"))
      {
        if (!req.i64Field("luldBps", luldBps) || !req.i64Field("luldHaltNs", luldHaltNs))
        {
          return err("bad_field");
        }
        r.fields |= RiskLimitField::RiskLuld;
        r.luldBps = static_cast<int32_t>(luldBps);
        r.luldHaltNs = DurationNs{luldHaltNs};
      }
      if (req.present("maxOrderQty") || req.present("maxOrderNotional"))
      {
        if (!req.decimalField("maxOrderQty", r.maxOrderQty) ||
            !req.decimalField("maxOrderNotional", r.maxOrderNotional))
        {
          return err("bad_field");
        }
        r.fields |= RiskLimitField::RiskFatFinger;
      }
      if (req.present("maxOpenOrders"))
      {
        if (!req.u64Field("maxOpenOrders", maxOpenOrders) ||
            maxOpenOrders > (std::numeric_limits<uint32_t>::max)())
        {
          return err("bad_field");
        }
        r.fields |= RiskLimitField::RiskMaxOpenOrders;
        r.maxOpenOrders = static_cast<uint32_t>(maxOpenOrders);
      }
      if (req.present("maxPositionQty"))
      {
        if (!req.decimalField("maxPositionQty", r.maxPositionQty))
        {
          return err("bad_field");
        }
        r.fields |= RiskLimitField::RiskMaxPosition;
      }
      if (req.present("initialMarginBps") || req.present("maintenanceMarginBps"))
      {
        if (!req.u64Field("initialMarginBps", imBps) ||
            !req.u64Field("maintenanceMarginBps", mmBps) ||
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
      if (!req.symbolField("symbol", sym) || !req.boolField("delisted", off))
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
      if (!req.symbolField("symbol", sym))
      {
        return err("bad_field");
      }
      if (!reg_.get(sym))
      {
        return err("unknown_symbol");
      }
      uint64_t account = 0;
      if (!req.u64Field("account", account))
      {
        return err("bad_field");
      }
      AdmissionProfile p;
      uint64_t allowedTypes = 0;
      uint64_t allowedTif = 0;
      if ((req.present("allowedTypes") && !req.u64Field("allowedTypes", allowedTypes)) ||
          (req.present("allowedTif") && !req.u64Field("allowedTif", allowedTif)) ||
          allowedTypes > (std::numeric_limits<uint32_t>::max)() ||
          allowedTif > (std::numeric_limits<uint32_t>::max)())
      {
        return err("bad_field");
      }
      p.allowedTypes = static_cast<uint32_t>(allowedTypes);
      p.allowedTif = static_cast<uint32_t>(allowedTif);
      uint8_t deny = 0;
      if (req.text("denyResting") == "true")
      {
        deny |= AdmissionDeny::DenyResting;
      }
      if (req.text("denyAmend") == "true")
      {
        deny |= AdmissionDeny::DenyAmend;
      }
      if (req.text("denyCancel") == "true")
      {
        deny |= AdmissionDeny::DenyCancel;
      }
      if (req.text("denyQuote") == "true")
      {
        deny |= AdmissionDeny::DenyQuote;
      }
      p.deny = deny;
      forward(InboundCommand{SetAdmissionProfile{sym, {}, account, p}});
      return ok();
    }
    if (method == "snapshotNow")
    {
      SymbolId sym{};
      if (!req.symbolField("symbol", sym))
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
      if (!req.symbolField("symbol", sym))
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
    if (const auto it = methods_.find(method); it != methods_.end())
    {
      return it->second(req);
    }
    return unknownMethod(method);
  }

 private:
  void forward(const InboundCommand& cmd)
  {
    if (sink_)
    {
      sink_(cmd);
    }
  }

  // An unknown verb answers with the name it did not know. A bare
  // "unknown_method" reads the same for a typo, a registration that never ran
  // and a request that reached the wrong process, which leaves the caller
  // guessing which of the three it is.
  static std::string unknownMethod(const std::string& name)
  {
    return std::string("{\"ok\":false,\"error\":\"unknown_method\",\"method\":\"") + echo(name) +
           "\"}";
  }

  // The name is caller input on its way back out, so it is cut and escaped: a
  // response is JSON before it is a diagnostic. Anything outside printable
  // ASCII travels as a \uXXXX escape, which also keeps the cut from splitting
  // an escape in half.
  static std::string echo(const std::string& name)
  {
    static constexpr size_t kMaxEcho = 64;
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    const size_t n = name.size() < kMaxEcho ? name.size() : kMaxEcho;
    for (size_t i = 0; i < n; ++i)
    {
      const auto c = static_cast<unsigned char>(name[i]);
      if (c == '"' || c == '\\')
      {
        out += '\\';
        out += static_cast<char>(c);
      }
      else if (c < 0x20 || c > 0x7e)
      {
        out += "\\u00";
        out += kHex[c >> 4];
        out += kHex[c & 0x0f];
      }
      else
      {
        out += static_cast<char>(c);
      }
    }
    return out;
  }

  static std::string instrumentJson(const SymbolConfig& c)
  {
    return std::string("{\"ok\":true,\"symbol\":") + std::to_string(c.id) +
           ",\"tick\":" + std::to_string(c.tickSize.toDouble()) +
           ",\"minPrice\":" + std::to_string(c.minPrice.toDouble()) +
           ",\"maxPrice\":" + std::to_string(c.maxPrice.toDouble()) +
           ",\"halted\":" + (c.halted ? "true" : "false") +
           ",\"triggerRef\":\"" + (c.triggerRef == TriggerRef::Mark ? "mark" : "last") + "\"}";
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
  std::unordered_map<std::string, Method> methods_;
};

}  // namespace flox::venue
