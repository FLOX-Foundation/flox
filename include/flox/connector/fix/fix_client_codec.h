/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * The client half of a FIX 4.4 order-entry session: NewOrderSingle (35=D),
 * OrderCancelRequest (35=F) and OrderCancelReplaceRequest (35=G) out;
 * ExecutionReport (35=8), OrderCancelReject (35=9), Reject (35=3) and
 * BusinessMessageReject (35=j) in. The mirror image of the venue's FixCodec,
 * which encodes 35=8 and decodes D/F/G.
 *
 * Prices and quantities never pass through a double in either direction. They
 * are printed from and parsed into fixed-point by flox::decwire, the same code
 * the venue side uses, so a price that went out as 100.25 comes back as the
 * same raw value and prints again as 100.25 rather than 100.250000.
 *
 * The decoder reads the fields a flox venue puts on the wire, including the
 * ones FIX 4.4 has no standard spelling for: ExecType=U with custom tags 20001
 * (heldId) and 20002 (makerId) for a fill held pending a last look, and
 * ExecType=H for a held fill that will not stand. A client that dropped those
 * would see a last-look venue's held fill as a plain working order and would
 * never learn the hold resolved.
 *
 * Decoding is total: every message is either a value or std::nullopt, and no
 * input -- truncated, corrupt, or hostile -- reaches undefined behaviour. A
 * checksum that is present and wrong is a rejection, not a warning.
 */
#pragma once

#include "flox/common.h"
#include "flox/connector/fix/fix_wire.h"
#include "flox/util/decimal_wire.h"

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace flox::fix
{

// ---- outbound: what a client sends -----------------------------------------

// NewOrderSingle (35=D). `price` is ignored for a MARKET order; `triggerPrice`
// carries the stop / take-profit activation level (99), `visibleQuantity` the
// iceberg display size (111, MaxFloor). A zero visibleQuantity means fully
// visible and the tag is omitted.
struct NewOrderRequest
{
  uint64_t clOrdId{};
  SymbolId symbol{};
  Side side{Side::BUY};
  OrderType type{OrderType::LIMIT};
  Price price{};
  Quantity quantity{};
  TimeInForce tif{TimeInForce::GTC};
  bool postOnly{false};
  bool reduceOnly{false};
  Quantity visibleQuantity{};
  Price triggerPrice{};
  uint64_t accountId{0};
};

// OrderCancelRequest (35=F). OrigClOrdID (41) names the order being cancelled;
// ClOrdID (11) names this cancel request.
struct CancelRequest
{
  uint64_t origClOrdId{};
  uint64_t clOrdId{};
  SymbolId symbol{};
  Side side{Side::BUY};
  Quantity quantity{};
  uint64_t accountId{0};
};

// OrderCancelReplaceRequest (35=G): the new price and quantity for the order
// named by OrigClOrdID.
struct CancelReplaceRequest
{
  uint64_t origClOrdId{};
  uint64_t clOrdId{};
  SymbolId symbol{};
  Side side{Side::BUY};
  OrderType type{OrderType::LIMIT};
  Price price{};
  Quantity quantity{};
  uint64_t accountId{0};
};

// ---- inbound: what a client receives ---------------------------------------

// ExecType (150) as the venue writes it, including the two values FIX 4.4 does
// not define and a flox venue uses for the last-look hold.
enum class ExecType : uint8_t
{
  New,          // 0
  PartialFill,  // 1
  Fill,         // 2
  Canceled,     // 4
  Replaced,     // 5
  Rejected,     // 8
  Trade,        // F
  FillHeld,     // U -- custom: fill pending the maker's confirmation
  TradeCancel,  // H -- the held fill will not stand
  Other,        // anything else the counterparty sends
};

struct ExecutionReport
{
  uint64_t orderId{};   // 37
  uint64_t clOrdId{};   // 11 (absent on some reports; 0 then)
  SymbolId symbol{};    // 55
  bool hasSide{false};  // 54 is optional on a venue exec report
  Side side{Side::BUY};
  ExecType execType{ExecType::Other};
  std::string execTypeRaw;  // the tag-150 value exactly as received
  std::string ordStatus;    // 39, kept raw: its enum is wider than we act on
  Quantity lastQty{};       // 32
  Price lastPx{};           // 31
  Price avgPx{};            // 6
  Quantity leavesQty{};     // 151
  Price price{};            // 44
  std::string text;         // 58
  uint64_t heldId{};        // 20001 -- last-look hold identity
  uint64_t makerId{};       // 20002
};

struct CancelReject
{
  uint64_t orderId{};      // 37
  uint64_t clOrdId{};      // 11
  uint64_t origClOrdId{};  // 41
  std::string ordStatus;   // 39
  int responseTo{0};       // 434: 1 = cancel request, 2 = cancel/replace
  int reason{0};           // 102
  std::string text;        // 58
};

// Session-level Reject (35=3): the counterparty could not process a message of
// ours at the session layer.
struct SessionReject
{
  uint64_t refSeqNum{};    // 45
  int refTagId{0};         // 371
  std::string refMsgType;  // 372
  int reason{0};           // 373
  std::string text;        // 58
};

// BusinessMessageReject (35=j): the session is healthy and the application
// declines the message. Distinct from 35=3 on purpose -- see fix_wire.h.
struct BusinessReject
{
  uint64_t refSeqNum{};    // 45
  std::string refMsgType;  // 372
  int reason{0};           // 380
  std::string text;        // 58
};

using InboundReport = std::variant<ExecutionReport, CancelReject, SessionReject, BusinessReject>;

class ClientCodec
{
 public:
  static constexpr char SOH = kSoh;

  // ---- encode ----
  // Each returns the framed message body from tag 35 onward, WITHOUT the
  // session header: encodeSequenced() below is the normal entry point and
  // injects 34/49/56/52. The bare form exists for the same reason the venue's
  // does -- embedded and test callers that own their own framing.
  static std::string encode(const NewOrderRequest& o)
  {
    std::string b;
    add(b, 35, "D");
    add(b, 11, std::to_string(o.clOrdId));
    if (o.accountId != 0)
    {
      add(b, 1, std::to_string(o.accountId));
    }
    add(b, 55, std::to_string(o.symbol));
    add(b, 54, o.side == Side::SELL ? "2" : "1");
    add(b, 38, qtyStr(o.quantity));
    add(b, 40, ordTypeStr(o.type));
    if (o.type != OrderType::MARKET)
    {
      add(b, 44, pxStr(o.price));
    }
    if (o.triggerPrice.raw() != 0)
    {
      add(b, 99, pxStr(o.triggerPrice));
    }
    if (o.visibleQuantity.raw() != 0)
    {
      add(b, 111, qtyStr(o.visibleQuantity));  // MaxFloor -> iceberg peak
    }
    add(b, 59, tifStr(o.tif));
    // ExecInst (18) is a multi-value field: '6' ParticipateDoNotInitiate is
    // post-only, 'E' DoNotIncrease is reduce-only. The venue reads it by
    // substring search, so both can ride in one field.
    std::string execInst;
    if (o.postOnly || o.tif == TimeInForce::POST_ONLY)
    {
      execInst += '6';
    }
    if (o.reduceOnly)
    {
      execInst += 'E';
    }
    if (!execInst.empty())
    {
      add(b, 18, execInst);
    }
    return frame(b);
  }

  static std::string encode(const CancelRequest& c)
  {
    std::string b;
    add(b, 35, "F");
    add(b, 41, std::to_string(c.origClOrdId));
    add(b, 11, std::to_string(c.clOrdId != 0 ? c.clOrdId : c.origClOrdId));
    if (c.accountId != 0)
    {
      add(b, 1, std::to_string(c.accountId));
    }
    add(b, 55, std::to_string(c.symbol));
    add(b, 54, c.side == Side::SELL ? "2" : "1");
    if (c.quantity.raw() != 0)
    {
      add(b, 38, qtyStr(c.quantity));
    }
    return frame(b);
  }

  static std::string encode(const CancelReplaceRequest& r)
  {
    std::string b;
    add(b, 35, "G");
    add(b, 41, std::to_string(r.origClOrdId));
    add(b, 11, std::to_string(r.clOrdId != 0 ? r.clOrdId : r.origClOrdId));
    if (r.accountId != 0)
    {
      add(b, 1, std::to_string(r.accountId));
    }
    add(b, 55, std::to_string(r.symbol));
    add(b, 54, r.side == Side::SELL ? "2" : "1");
    add(b, 38, qtyStr(r.quantity));
    add(b, 40, ordTypeStr(r.type));
    if (r.type != OrderType::MARKET)
    {
      add(b, 44, pxStr(r.price));
    }
    return frame(b);
  }

  // Session-framed variant: takes the bare encoding above and injects the
  // header fields (34/49/56/52, plus 43/122 on a resend replay) after the
  // MsgType, the way the venue's FixCodec::encode does on its side. Empty in,
  // empty out.
  static std::string reframe(const std::string& bare, uint64_t seq, const std::string& senderCompId,
                             const std::string& targetCompId, const std::string& sendingTimeStr,
                             bool possDup = false, const std::string& origSendingTime = {})
  {
    if (bare.empty())
    {
      return bare;
    }
    const std::string anchor = std::string(1, kSoh) + "35=";
    const size_t a = bare.find(anchor);
    if (a == std::string::npos)
    {
      return {};
    }
    const size_t tend = bare.find(kSoh, a + anchor.size());
    if (tend == std::string::npos)
    {
      return {};
    }
    const std::string marker = bare.substr(a + 1, tend - a);  // "35=<type><SOH>"
    const size_t bodyStart = tend + 1;
    const size_t csum = bare.rfind(std::string(1, kSoh) + "10=");
    const std::string tail =
        bare.substr(bodyStart, (csum == std::string::npos ? bare.size() : csum + 1) - bodyStart);
    std::string b = marker;
    appendHeader(b, seq, senderCompId, targetCompId, sendingTimeStr, possDup, origSendingTime);
    b += tail;
    return frame(b);
  }

  template <typename Request>
  static std::string encodeSequenced(const Request& r, uint64_t seq,
                                     const std::string& senderCompId,
                                     const std::string& targetCompId,
                                     const std::string& sendingTimeStr, bool possDup = false,
                                     const std::string& origSendingTime = {})
  {
    return reframe(encode(r), seq, senderCompId, targetCompId, sendingTimeStr, possDup,
                   origSendingTime);
  }

  // ---- decode ----
  // An application message from the counterparty. std::nullopt = a bad
  // checksum, or a MsgType outside the four this codec speaks. Session-layer
  // traffic (0/1/2/4/5/A) is the initiator's business, not the codec's.
  static std::optional<InboundReport> decode(const std::string& msg)
  {
    if (!checksumValid(msg))
    {
      return std::nullopt;
    }
    Fields f = parseFields(msg);
    const std::string type = str(f, 35);
    if (type == "8")
    {
      return InboundReport{decodeExecReport(f)};
    }
    if (type == "9")
    {
      CancelReject c;
      c.orderId = u64(f, 37);
      c.clOrdId = u64(f, 11);
      c.origClOrdId = u64(f, 41);
      c.ordStatus = str(f, 39);
      c.responseTo = i32(f, 434);
      c.reason = i32(f, 102);
      c.text = str(f, 58);
      return InboundReport{c};
    }
    if (type == "3")
    {
      SessionReject r;
      r.refSeqNum = u64(f, 45);
      r.refTagId = i32(f, 371);
      r.refMsgType = str(f, 372);
      r.reason = i32(f, 373);
      r.text = str(f, 58);
      return InboundReport{r};
    }
    if (type == "j")
    {
      BusinessReject r;
      r.refSeqNum = u64(f, 45);
      r.refMsgType = str(f, 372);
      r.reason = i32(f, 380);
      r.text = str(f, 58);
      return InboundReport{r};
    }
    return std::nullopt;
  }

  static ExecType execTypeOf(const std::string& raw)
  {
    if (raw == "0")
    {
      return ExecType::New;
    }
    if (raw == "1")
    {
      return ExecType::PartialFill;
    }
    if (raw == "2")
    {
      return ExecType::Fill;
    }
    if (raw == "4")
    {
      return ExecType::Canceled;
    }
    if (raw == "5")
    {
      return ExecType::Replaced;
    }
    if (raw == "8")
    {
      return ExecType::Rejected;
    }
    if (raw == "F")
    {
      return ExecType::Trade;
    }
    if (raw == "U")
    {
      return ExecType::FillHeld;
    }
    if (raw == "H")
    {
      return ExecType::TradeCancel;
    }
    return ExecType::Other;
  }

 private:
  static void add(std::string& b, int tag, const std::string& v)
  {
    b += std::to_string(tag) + "=" + v + kSoh;
  }

  static std::string pxStr(Price p)
  {
    std::string s;
    decwire::append(s, p.raw());
    return s;
  }

  static std::string qtyStr(Quantity q)
  {
    std::string s;
    decwire::append(s, q.raw());
    return s;
  }

  // OrdType (40). The venue maps 1/2/3/4; the conditional types beyond
  // STOP_LIMIT have no FIX 4.4 spelling a counterparty is guaranteed to read,
  // so they go out as their nearest honest equivalent rather than as a value
  // the far side would reject outright.
  static const char* ordTypeStr(OrderType t)
  {
    switch (t)
    {
      case OrderType::MARKET:
        return "1";
      case OrderType::STOP_MARKET:
      case OrderType::TAKE_PROFIT_MARKET:
      case OrderType::TRAILING_STOP:
        return "3";
      case OrderType::STOP_LIMIT:
      case OrderType::TAKE_PROFIT_LIMIT:
        return "4";
      case OrderType::LIMIT:
      case OrderType::ICEBERG:
      default:
        return "2";
    }
  }

  // TimeInForce (59). POST_ONLY has no TIF value in FIX 4.4 -- it is an
  // ExecInst -- so it goes out as GTC with '6' in tag 18.
  static const char* tifStr(TimeInForce t)
  {
    switch (t)
    {
      case TimeInForce::IOC:
        return "3";
      case TimeInForce::FOK:
        return "4";
      case TimeInForce::GTD:
        return "6";
      case TimeInForce::GTC:
      case TimeInForce::POST_ONLY:
      default:
        return "1";
    }
  }

  static ExecutionReport decodeExecReport(Fields& f)
  {
    ExecutionReport e;
    e.orderId = u64(f, 37);
    e.clOrdId = u64(f, 11);
    e.symbol = static_cast<SymbolId>(u64(f, 55));
    const std::string side = str(f, 54);
    if (side == "1" || side == "2")
    {
      e.hasSide = true;
      e.side = side == "2" ? Side::SELL : Side::BUY;
    }
    e.execTypeRaw = str(f, 150);
    e.execType = execTypeOf(e.execTypeRaw);
    e.ordStatus = str(f, 39);
    e.lastQty = Quantity::fromRaw(dec(f, 32));
    e.lastPx = Price::fromRaw(dec(f, 31));
    e.avgPx = Price::fromRaw(dec(f, 6));
    e.leavesQty = Quantity::fromRaw(dec(f, 151));
    e.price = Price::fromRaw(dec(f, 44));
    e.text = str(f, 58);
    e.heldId = u64(f, 20001);
    e.makerId = u64(f, 20002);
    return e;
  }

  static std::string str(Fields& f, int tag)
  {
    const auto it = f.find(tag);
    return it != f.end() ? it->second : std::string{};
  }

  static uint64_t u64(Fields& f, int tag)
  {
    const auto it = f.find(tag);
    return it != f.end() ? std::strtoull(it->second.c_str(), nullptr, 10) : 0;
  }

  static int i32(Fields& f, int tag)
  {
    const auto it = f.find(tag);
    return it != f.end() ? std::atoi(it->second.c_str()) : 0;
  }

  // A decimal field as a fixed-point raw value. Absent or unparseable reads as
  // zero: an exec report is a report, and dropping the whole message because
  // one optional decimal is malformed would lose a fill the venue believes it
  // told us about. The strict path is the other direction -- what we SEND is
  // built from fixed-point and cannot be malformed.
  static int64_t dec(Fields& f, int tag)
  {
    const auto it = f.find(tag);
    int64_t v = 0;
    if (it != f.end() && decwire::parse(it->second, v))
    {
      return v;
    }
    return 0;
  }
};

}  // namespace flox::fix
