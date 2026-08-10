/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * REST/JSON perimeter codec for the venue simulator (the low-friction client
 * path next to FIX/OUCH: curl and script agents speak JSON).
 *
 * Decode is STRICT. Tokenization is simdjson; on top of it the schema is
 * closed: unknown action, unknown or duplicate key, missing required field,
 * or a malformed value all yield std::nullopt -- never a guessed default.
 * Price/quantity fields parse from the raw JSON number token straight into
 * fixed-point (no double round-trip); out-of-range and excess precision are
 * rejected, not clamped. Encode emits fixed-point exactly via to_chars.
 */
#pragma once

#include "flox-venue/decimal_wire.h"
#include "flox-venue/messages.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <simdjson.h>

namespace flox::venue
{

class RestJson
{
 public:
  // ---- inbound: JSON body -> InboundCommand ----
  // Schema: {"action":"new|cancel|modify", ...fields}. See decode(sv, err)
  // for the strict field rules; this overload discards the error reason.
  static std::optional<InboundCommand> decode(std::string_view json)
  {
    const char* err = nullptr;
    return decode(json, err);
  }

  static std::optional<InboundCommand> decode(std::string_view json, const char*& err)
  {
    static_assert(Price::Scale == Quantity::Scale, "shared fixed-point scale");
    err = nullptr;

    static thread_local simdjson::ondemand::parser parser;
    const simdjson::padded_string padded(json);

    Fields f{};
    {
      simdjson::ondemand::document doc;
      if (parser.iterate(padded).get(doc) != simdjson::SUCCESS)
      {
        err = "malformed json";
        return std::nullopt;
      }
      simdjson::ondemand::object obj;
      if (doc.get_object().get(obj) != simdjson::SUCCESS)
      {
        err = "body must be a json object";
        return std::nullopt;
      }
      for (auto field : obj)
      {
        std::string_view key;
        if (field.unescaped_key().get(key) != simdjson::SUCCESS)
        {
          err = "bad key";
          return std::nullopt;
        }
        simdjson::ondemand::value value;
        if (field.value().get(value) != simdjson::SUCCESS)
        {
          err = "bad value";
          return std::nullopt;
        }
        if (!readField(key, value, f, err))
        {
          if (err == nullptr)
          {
            err = "bad field";
          }
          return std::nullopt;
        }
      }
      if (!doc.at_end())
      {
        err = "trailing content after object";
        return std::nullopt;
      }
    }

    return assemble(f, err);
  }

  // ---- outbound: OutboundEvent -> JSON ----
  static std::string encode(const OutboundEvent& ev)
  {
    std::string out;
    out.reserve(160);
    encode(ev, out);
    return out;
  }

  static void encode(const OutboundEvent& ev, std::string& out)
  {
    Writer w{out};
    if (const auto* a = std::get_if<OrderAccepted>(&ev))
    {
      w.str("type", "accepted");
      w.u64("id", a->id);
      w.str("side", a->side == Side::SELL ? "sell" : "buy");
      w.fixed("price", a->price.raw());
      w.fixed("leaves", a->leavesQty.raw());
      w.boolean("onBook", a->restingOnBook);
    }
    else if (const auto* t = std::get_if<Trade>(&ev))
    {
      w.str("type", "trade");
      w.u64("tradeId", t->tradeId);
      w.fixed("price", t->price.raw());
      w.fixed("qty", t->quantity.raw());
      w.u64("maker", t->makerId);
      w.u64("taker", t->takerId);
    }
    else if (const auto* x = std::get_if<OrderExecuted>(&ev))
    {
      w.str("type", "executed");
      w.u64("id", x->id);
      w.fixed("lastQty", x->lastQty.raw());
      w.fixed("leaves", x->leavesQty.raw());
      w.boolean("complete", x->complete);
    }
    else if (const auto* c = std::get_if<OrderCanceled>(&ev))
    {
      w.str("type", "canceled");
      w.u64("id", c->id);
      w.str("reason", toString(c->reason));
    }
    else if (const auto* j = std::get_if<OrderRejected>(&ev))
    {
      w.str("type", "rejected");
      w.u64("id", j->id);
      w.str("reason", toString(j->reason));
    }
    else if (const auto* m = std::get_if<OrderModified>(&ev))
    {
      w.str("type", "modified");
      w.u64("id", m->id);
      w.fixed("price", m->price.raw());
      w.fixed("leaves", m->leavesQty.raw());
    }
    else if (const auto* g = std::get_if<OrderTriggered>(&ev))
    {
      w.str("type", "triggered");
      w.u64("id", g->id);
      w.fixed("refPrice", g->refPrice.raw());
    }
    w.finish();
  }

 private:
  // ---- decode internals ----

  enum FieldBit : uint32_t
  {
    kAction = 1u << 0,
    kId = 1u << 1,
    kSymbol = 1u << 2,
    kSide = 1u << 3,
    kQty = 1u << 4,
    kPrice = 1u << 5,
    kOrdType = 1u << 6,
    kTif = 1u << 7,
    kPostOnly = 1u << 8,
    kReduceOnly = 1u << 9,
    kStp = 1u << 10,
    kTrigger = 1u << 11,
    kTrailingOffset = 1u << 12,
    kVisible = 1u << 13,
    kAccount = 1u << 14,
    kClientOrderId = 1u << 15,
  };

  // Longest legal token is "cancelNewest" (12); anything longer is not ours.
  struct Tok
  {
    char buf[16]{};
    uint8_t len{0};

    std::string_view view() const { return {buf, len}; }

    bool set(std::string_view sv)
    {
      if (sv.size() > sizeof(buf))
      {
        return false;
      }
      for (size_t i = 0; i < sv.size(); ++i)
      {
        buf[i] = sv[i];
      }
      len = static_cast<uint8_t>(sv.size());
      return true;
    }
  };

  struct Fields
  {
    uint32_t seen{0};
    Tok action, side, ordType, tif, stp;
    uint64_t id{0}, clientOrderId{0}, account{0};
    uint64_t symbol{0};
    int64_t qtyRaw{0}, priceRaw{0}, triggerRaw{0}, trailRaw{0}, visibleRaw{0};
    bool postOnly{false}, reduceOnly{false};
  };

  static bool mark(Fields& f, uint32_t bit, const char*& err)
  {
    if (f.seen & bit)
    {
      err = "duplicate key";
      return false;
    }
    f.seen |= bit;
    return true;
  }

  static bool readTok(simdjson::ondemand::value& v, Tok& t, const char*& err)
  {
    std::string_view sv;
    if (v.get_string().get(sv) != simdjson::SUCCESS)
    {
      err = "expected string";
      return false;
    }
    if (!t.set(sv))
    {
      err = "string too long";
      return false;
    }
    return true;
  }

  static bool readU64(simdjson::ondemand::value& v, uint64_t& out, const char*& err)
  {
    if (v.get_uint64().get(out) != simdjson::SUCCESS)
    {
      err = "expected unsigned integer";
      return false;
    }
    return true;
  }

  static bool readBool(simdjson::ondemand::value& v, bool& out, const char*& err)
  {
    if (v.get_bool().get(out) != simdjson::SUCCESS)
    {
      err = "expected true/false";
      return false;
    }
    return true;
  }

  // Raw JSON number token -> fixed-point raw. Strict: unsigned decimal, at
  // most Scale precision, no exponent, no leading '+'/'-', overflow rejected.
  static bool readFixed(simdjson::ondemand::value& v, int64_t& out, const char*& err)
  {
    simdjson::ondemand::json_type t;
    if (v.type().get(t) != simdjson::SUCCESS || t != simdjson::ondemand::json_type::number)
    {
      err = "expected number";
      return false;
    }
    std::string_view tok = v.raw_json_token();
    while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t' || tok.back() == '\n' ||
                            tok.back() == '\r'))
    {
      tok.remove_suffix(1);
    }
    if (!decwire::parse(tok, out))
    {
      err = "bad decimal";
      return false;
    }
    return true;
  }

  static bool readField(std::string_view key, simdjson::ondemand::value value, Fields& f,
                        const char*& err)
  {
    if (key == "action")
    {
      return mark(f, kAction, err) && readTok(value, f.action, err);
    }
    if (key == "id")
    {
      return mark(f, kId, err) && readU64(value, f.id, err);
    }
    if (key == "symbol")
    {
      return mark(f, kSymbol, err) && readU64(value, f.symbol, err);
    }
    if (key == "side")
    {
      return mark(f, kSide, err) && readTok(value, f.side, err);
    }
    if (key == "qty")
    {
      return mark(f, kQty, err) && readFixed(value, f.qtyRaw, err);
    }
    if (key == "price")
    {
      return mark(f, kPrice, err) && readFixed(value, f.priceRaw, err);
    }
    if (key == "ordType")
    {
      return mark(f, kOrdType, err) && readTok(value, f.ordType, err);
    }
    if (key == "tif")
    {
      return mark(f, kTif, err) && readTok(value, f.tif, err);
    }
    if (key == "postOnly")
    {
      return mark(f, kPostOnly, err) && readBool(value, f.postOnly, err);
    }
    if (key == "reduceOnly")
    {
      return mark(f, kReduceOnly, err) && readBool(value, f.reduceOnly, err);
    }
    if (key == "stp")
    {
      return mark(f, kStp, err) && readTok(value, f.stp, err);
    }
    if (key == "trigger")
    {
      return mark(f, kTrigger, err) && readFixed(value, f.triggerRaw, err);
    }
    if (key == "trailingOffset")
    {
      return mark(f, kTrailingOffset, err) && readFixed(value, f.trailRaw, err);
    }
    if (key == "visible")
    {
      return mark(f, kVisible, err) && readFixed(value, f.visibleRaw, err);
    }
    if (key == "account")
    {
      return mark(f, kAccount, err) && readU64(value, f.account, err);
    }
    if (key == "clientOrderId")
    {
      return mark(f, kClientOrderId, err) && readU64(value, f.clientOrderId, err);
    }
    err = "unknown key";
    return false;
  }

  static bool require(const Fields& f, uint32_t bits, const char*& err)
  {
    if ((f.seen & bits) != bits)
    {
      err = "missing required field";
      return false;
    }
    return true;
  }

  static std::optional<InboundCommand> assemble(const Fields& f, const char*& err)
  {
    if (!(f.seen & kAction))
    {
      err = "missing action";
      return std::nullopt;
    }
    const std::string_view action = f.action.view();

    if (f.symbol > std::numeric_limits<SymbolId>::max())
    {
      err = "symbol out of range";
      return std::nullopt;
    }
    const SymbolId symbol = static_cast<SymbolId>(f.symbol);

    if (action == "new")
    {
      if (!require(f, kId | kSymbol | kSide | kQty, err))
      {
        return std::nullopt;
      }
      NewOrder o;
      o.id = f.id;
      o.clientOrderId = (f.seen & kClientOrderId) ? f.clientOrderId : f.id;
      o.symbol = symbol;
      o.accountId = f.account;
      const std::string_view side = f.side.view();
      if (side == "buy")
      {
        o.side = Side::BUY;
      }
      else if (side == "sell")
      {
        o.side = Side::SELL;
      }
      else
      {
        err = "side must be buy or sell";
        return std::nullopt;
      }
      o.quantity = Quantity::fromRaw(f.qtyRaw);
      const std::string_view t = (f.seen & kOrdType) ? f.ordType.view() : "limit";
      if (t == "limit")
      {
        o.type = OrderType::LIMIT;
      }
      else if (t == "market")
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
        err = "unknown ordType";
        return std::nullopt;
      }
      if (f.seen & kPrice)
      {
        o.price = Price::fromRaw(f.priceRaw);
      }
      if (f.seen & kTrigger)
      {
        o.triggerPrice = Price::fromRaw(f.triggerRaw);
      }
      if (f.seen & kTrailingOffset)
      {
        o.trailingOffset = Price::fromRaw(f.trailRaw);
      }
      if (f.seen & kVisible)
      {
        o.visibleQuantity = Quantity::fromRaw(f.visibleRaw);
      }
      const std::string_view tif = (f.seen & kTif) ? f.tif.view() : "gtc";
      if (tif == "gtc")
      {
        o.tif = TimeInForce::GTC;
      }
      else if (tif == "ioc")
      {
        o.tif = TimeInForce::IOC;
      }
      else if (tif == "fok")
      {
        o.tif = TimeInForce::FOK;
      }
      else
      {
        err = "unknown tif";
        return std::nullopt;
      }
      o.postOnly = f.postOnly;
      o.reduceOnly = f.reduceOnly;
      const std::string_view stp = (f.seen & kStp) ? f.stp.view() : "none";
      if (stp == "none")
      {
        o.stp = STPMode::None;
      }
      else if (stp == "cancelNewest")
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
      else
      {
        err = "unknown stp";
        return std::nullopt;
      }
      return InboundCommand{o};
    }

    if (action == "cancel")
    {
      static constexpr uint32_t kAllowed = kAction | kId | kSymbol | kAccount;
      if (f.seen & ~kAllowed)
      {
        err = "unexpected field for cancel";
        return std::nullopt;
      }
      if (!require(f, kId | kSymbol, err))
      {
        return std::nullopt;
      }
      return InboundCommand{CancelOrder{f.id, symbol, f.account}};
    }

    if (action == "modify")
    {
      static constexpr uint32_t kAllowed = kAction | kId | kSymbol | kPrice | kQty | kAccount;
      if (f.seen & ~kAllowed)
      {
        err = "unexpected field for modify";
        return std::nullopt;
      }
      if (!require(f, kId | kSymbol | kQty, err))
      {
        return std::nullopt;
      }
      ModifyOrder m;
      m.id = f.id;
      m.symbol = symbol;
      if (f.seen & kPrice)
      {
        m.newPrice = Price::fromRaw(f.priceRaw);
      }
      m.newQty = Quantity::fromRaw(f.qtyRaw);
      m.accountId = f.account;
      return InboundCommand{m};
    }

    err = "unknown action";
    return std::nullopt;
  }

  // ---- encode internals ----

  struct Writer
  {
    std::string& o;
    bool first{true};

    explicit Writer(std::string& out) : o(out) { o.push_back('{'); }

    void key(const char* k)
    {
      if (!first)
      {
        o.push_back(',');
      }
      first = false;
      o.push_back('"');
      o.append(k);
      o.append("\":", 2);
    }

    void str(const char* k, const char* v)
    {
      key(k);
      o.push_back('"');
      o.append(v);
      o.push_back('"');
    }

    void u64(const char* k, uint64_t v)
    {
      key(k);
      decwire::appendU64(o, v);
    }

    void boolean(const char* k, bool v)
    {
      key(k);
      o.append(v ? "true" : "false");
    }

    void fixed(const char* k, int64_t raw)
    {
      key(k);
      decwire::append(o, raw);
    }

    void finish() { o.push_back('}'); }
  };
};

}  // namespace flox::venue
