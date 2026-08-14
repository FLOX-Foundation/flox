/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * FIX 4.4 codec for venue order entry (D/F/G in) and execution reports (35=8
 * out): real tag=value/SOH framing with BodyLength (9) and a validated
 * CheckSum (10).
 *
 * Numeric fields parse straight from the decimal string into fixed-point via
 * decwire (no double round-trip): a bad, exponent, over-precise or overflowing
 * number is rejected, not silently coerced. Required enum/quantity fields are
 * strict -- a missing or invalid Side (54), a present-but-unknown OrdType (40),
 * or a missing OrderQty (38) rejects the message rather than guessing a default.
 * Outbound prices/quantities serialise exactly (100.25, not 100.250000).
 */
#pragma once

#include "flox-venue/decimal_wire.h"
#include "flox-venue/messages.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace flox::venue
{

class FixCodec
{
 public:
  static constexpr char SOH = '\x01';

  // tag=value fields of a raw FIX message (last occurrence wins on repeats).
  static std::unordered_map<int, std::string> parseFields(const std::string& msg)
  {
    std::unordered_map<int, std::string> f;
    size_t i = 0;
    while (i < msg.size())
    {
      size_t eq = msg.find('=', i);
      if (eq == std::string::npos)
      {
        break;
      }
      size_t soh = msg.find(SOH, eq + 1);
      if (soh == std::string::npos)
      {
        soh = msg.size();
      }
      const int tag = std::atoi(msg.substr(i, eq - i).c_str());
      f[tag] = msg.substr(eq + 1, soh - eq - 1);
      i = soh + 1;
    }
    return f;
  }

  // FIX integrity: if a CheckSum (tag 10) is present it MUST be correct --
  // sum of every byte up to and including the SOH before "10=", mod 256.
  // Lenient when absent, for internal/test callers that don't append one.
  static bool checksumValid(const std::string& msg)
  {
    const std::string marker = std::string(1, SOH) + "10=";
    const size_t p = msg.rfind(marker);
    if (p == std::string::npos)
    {
      return true;
    }
    unsigned sum = 0;
    for (size_t k = 0; k <= p; ++k)
    {
      sum += static_cast<unsigned char>(msg[k]);
    }
    return (sum % 256) ==
           static_cast<unsigned>(std::atoi(msg.c_str() + p + marker.size()));
  }

  // ---- inbound: FIX message -> InboundCommand ----
  static std::optional<InboundCommand> decode(const std::string& msg)
  {
    std::unordered_map<int, std::string> f = parseFields(msg);

    auto has = [&](int t)
    { return f.count(t) != 0; };
    auto s = [&](int t)
    { return has(t) ? f[t] : std::string{}; };

    if (!checksumValid(msg))
    {
      return std::nullopt;  // checksum mismatch -> reject
    }

    auto u64 = [&](int t)
    { return static_cast<uint64_t>(std::strtoull(s(t).c_str(), nullptr, 10)); };
    auto sym = [&](int t)
    { return static_cast<SymbolId>(std::strtoul(s(t).c_str(), nullptr, 10)); };
    // Strict fixed-point parse of a decimal FIX field; false if the tag is
    // absent or the value is not a clean decimal.
    auto fix = [&](int t, int64_t& out)
    { return has(t) && decwire::parse(s(t), out); };

    const std::string type = s(35);
    if (type == "D")  // NewOrderSingle
    {
      NewOrder o;
      o.id = u64(11);
      o.clientOrderId = u64(11);
      o.symbol = sym(55);
      o.accountId = u64(1);

      // Side (54) is required and must be Buy(1)/Sell(2) -- never a guessed default.
      const std::string side = s(54);
      if (side == "1")
      {
        o.side = Side::BUY;
      }
      else if (side == "2")
      {
        o.side = Side::SELL;
      }
      else
      {
        return std::nullopt;
      }

      // OrderQty (38) required.
      int64_t qtyRaw;
      if (!fix(38, qtyRaw))
      {
        return std::nullopt;
      }
      o.quantity = Quantity::fromRaw(qtyRaw);

      // OrdType (40): absent -> Limit (the common default); present must be a
      // known type -- a garbage value is rejected, not mapped to Limit.
      if (has(40))
      {
        switch (std::atoi(s(40).c_str()))
        {
          case 1:
            o.type = OrderType::MARKET;
            break;
          case 2:
            o.type = OrderType::LIMIT;
            break;
          case 3:
            o.type = OrderType::STOP_MARKET;
            break;
          case 4:
            o.type = OrderType::STOP_LIMIT;
            break;
          default:
            return std::nullopt;
        }
      }
      else
      {
        o.type = OrderType::LIMIT;
      }

      // Optional decimals: present -> must parse.
      int64_t v;
      if (has(44))
      {
        if (!fix(44, v))
        {
          return std::nullopt;
        }
        o.price = Price::fromRaw(v);
      }
      if (has(99))
      {
        if (!fix(99, v))
        {
          return std::nullopt;
        }
        o.triggerPrice = Price::fromRaw(v);
      }
      if (has(111))
      {
        if (!fix(111, v))
        {
          return std::nullopt;
        }
        o.visibleQuantity = Quantity::fromRaw(v);  // MaxFloor -> iceberg peak
      }

      switch (std::atoi(s(59).c_str()))  // TimeInForce (absent/other -> GTC)
      {
        case 3:
          o.tif = TimeInForce::IOC;
          break;
        case 4:
          o.tif = TimeInForce::FOK;
          break;
        default:
          o.tif = TimeInForce::GTC;
          break;
      }
      const std::string execInst = s(18);
      if (execInst.find('6') != std::string::npos)  // ParticipateDoNotInitiate
      {
        o.postOnly = true;
      }
      if (execInst.find('E') != std::string::npos)  // DoNotIncrease -> reduce-only
      {
        o.reduceOnly = true;
      }
      return InboundCommand{o};
    }
    if (type == "F")  // OrderCancelRequest
    {
      if (!has(41))
      {
        return std::nullopt;  // OrigClOrdID required
      }
      CancelOrder c;
      c.id = u64(41);
      c.symbol = sym(55);
      c.accountId = u64(1);
      return InboundCommand{c};
    }
    if (type == "G")  // OrderCancelReplaceRequest
    {
      if (!has(41))
      {
        return std::nullopt;  // OrigClOrdID required
      }
      ModifyOrder m;
      m.id = u64(41);
      m.symbol = sym(55);
      if (has(44))
      {
        int64_t pr;
        if (!fix(44, pr))
        {
          return std::nullopt;
        }
        m.newPrice = Price::fromRaw(pr);
      }
      int64_t qtyRaw;
      if (!fix(38, qtyRaw))
      {
        return std::nullopt;  // new OrderQty required
      }
      m.newQty = Quantity::fromRaw(qtyRaw);
      m.accountId = u64(1);
      return InboundCommand{m};
    }
    return std::nullopt;
  }

  // MsgSeqNum (34) of a raw FIX message; 0 when absent (a structurally
  // incomplete message -- FIX 4.4 requires 34 on every message).
  static uint64_t msgSeqNum(const std::string& msg)
  {
    return tagValueU64(msg, 34);
  }

  // ---- outbound: OutboundEvent -> ExecutionReport (35=8) ----
  // Session-framed variant: injects the FIX 4.4 required header fields --
  // MsgSeqNum (34), SenderCompID (49), TargetCompID (56), SendingTime (52) --
  // that the bare encode() (embedded/test use) omits. Empty on events with no
  // exec-report mapping. A resend replay sets possDup (PossDupFlag 43=Y) and
  // origSendingTime (OrigSendingTime 122, the first transmission's 52) -- the
  // reason resends re-encode instead of replaying bytes: 43/122 change
  // BodyLength and CheckSum.
  static std::string encode(const OutboundEvent& ev, uint64_t seq, const std::string& senderCompId,
                            const std::string& targetCompId, const std::string& sendingTime,
                            bool possDup = false, const std::string& origSendingTime = {})
  {
    const std::string bare = encode(ev);
    if (bare.empty())
    {
      return bare;
    }
    // Re-frame: keep the body after 35=8, prepend the session header fields.
    const std::string marker = std::string("35=8") + SOH;
    const size_t p = bare.find(marker);
    if (p == std::string::npos)
    {
      return {};
    }
    const size_t bodyStart = p + marker.size();
    const size_t csum = bare.rfind(std::string(1, SOH) + "10=");
    const std::string tail =
        bare.substr(bodyStart, (csum == std::string::npos ? bare.size() : csum + 1) - bodyStart);
    std::string b = marker;
    b += "34=" + std::to_string(seq) + SOH;
    b += "49=" + senderCompId + SOH;
    b += "56=" + targetCompId + SOH;
    b += "52=" + sendingTime + SOH;
    if (possDup)
    {
      b += std::string("43=Y") + SOH;
    }
    if (!origSendingTime.empty())
    {
      b += "122=" + origSendingTime + SOH;
    }
    b += tail;
    return frame(b);
  }

  // Session/admin message (Logon 35=A, Heartbeat 35=0, TestRequest 35=1,
  // ResendRequest 35=2, SequenceReset 35=4, Logout 35=5) with the full FIX 4.4
  // header and body `fields`, framed with BodyLength and CheckSum.
  static std::string encodeAdmin(const std::string& msgType, uint64_t seq,
                                 const std::string& senderCompId, const std::string& targetCompId,
                                 const std::string& sendingTime,
                                 const std::vector<std::pair<int, std::string>>& fields = {},
                                 bool possDup = false)
  {
    std::string b = "35=" + msgType + SOH;
    b += "34=" + std::to_string(seq) + SOH;
    b += "49=" + senderCompId + SOH;
    b += "56=" + targetCompId + SOH;
    b += "52=" + sendingTime + SOH;
    if (possDup)
    {
      b += std::string("43=Y") + SOH;
    }
    for (const auto& [tag, val] : fields)
    {
      b += std::to_string(tag) + "=" + val + SOH;
    }
    return frame(b);
  }

  static std::string encode(const OutboundEvent& ev)
  {
    std::string b;  // body after 35
    auto add = [&](int tag, const std::string& val)
    { b += std::to_string(tag) + "=" + val + SOH; };
    auto px = [](Price p)
    {
      std::string s;
      decwire::append(s, p.raw());
      return s;
    };
    auto qn = [](Quantity q)
    {
      std::string s;
      decwire::append(s, q.raw());
      return s;
    };

    add(35, "8");  // ExecutionReport
    if (const auto* a = std::get_if<OrderAccepted>(&ev))
    {
      add(37, std::to_string(a->id));
      add(11, std::to_string(a->id));
      add(55, std::to_string(a->symbol));
      add(54, a->side == Side::SELL ? "2" : "1");
      add(150, "0");  // ExecType New
      add(39, "0");   // OrdStatus New
      add(151, qn(a->leavesQty));
      add(44, px(a->price));
    }
    else if (const auto* x = std::get_if<OrderExecuted>(&ev))
    {
      add(37, std::to_string(x->id));
      add(55, std::to_string(x->symbol));
      add(150, "F");                     // ExecType Trade
      add(39, x->complete ? "2" : "1");  // Filled / Partially filled
      add(32, qn(x->lastQty));           // LastQty
      add(31, px(x->lastPx));            // LastPx -- price of this fill
      add(6, px(x->lastPx));             // AvgPx (single-fill report)
      add(151, qn(x->leavesQty));        // LeavesQty
    }
    else if (const auto* c = std::get_if<OrderCanceled>(&ev))
    {
      add(37, std::to_string(c->id));
      add(150, "4");  // Canceled
      add(39, "4");
    }
    else if (const auto* j = std::get_if<OrderRejected>(&ev))
    {
      add(37, std::to_string(j->id));
      add(150, "8");  // Rejected
      add(39, "8");
      add(58, std::string(toString(j->reason)));
    }
    else if (const auto* m = std::get_if<OrderModified>(&ev))
    {
      add(37, std::to_string(m->id));
      add(150, "5");  // Replaced
      add(39, "5");
      add(151, qn(m->leavesQty));
      add(44, px(m->price));
    }
    else if (const auto* fh = std::get_if<FillHeld>(&ev))
    {
      // Last-look hold: FIX has no honest ExecType for "fill pending the
      // maker's confirmation", so this uses the documented custom value
      // ExecType=U plus custom tags 20001 (heldId) / 20002 (makerId); see
      // docs/venue/matching.md. The order is still working (39=0); 32/31
      // carry the held size and price.
      add(37, std::to_string(fh->takerId));
      add(55, std::to_string(fh->symbol));
      add(150, "U");  // custom ExecType: fill held pending last look
      add(39, "0");   // OrdStatus New/working -- nothing has executed yet
      add(32, qn(fh->qty));
      add(31, px(fh->price));
      add(20001, std::to_string(fh->heldId));
      add(20002, std::to_string(fh->makerId));
    }
    else if (const auto* fr = std::get_if<FillRejected>(&ev))
    {
      // Held fill rejected/timed out: the pending fill is busted, which maps
      // honestly onto ExecType=H (Trade Cancel). Same custom tags identify the
      // held fill being cancelled.
      add(37, std::to_string(fr->takerId));
      add(55, std::to_string(fr->symbol));
      add(150, "H");  // ExecType Trade Cancel: the held fill will not stand
      add(39, "0");
      add(32, qn(fr->qty));
      add(31, px(fr->price));
      add(20001, std::to_string(fr->heldId));
      add(20002, std::to_string(fr->makerId));
      add(58, "LastLookRejected");
    }
    else
    {
      return {};  // Trade/Triggered are market-data, not exec reports
    }

    return frame(b);
  }

  // Prepend 8/9, append 10 with correct BodyLength and CheckSum.
  static std::string frame(const std::string& body)
  {
    const std::string prefix = std::string("8=FIX.4.4") + SOH;
    const std::string lenField = std::string("9=") + std::to_string(body.size()) + SOH;
    std::string msg = prefix + lenField + body;
    uint32_t sum = 0;
    for (unsigned char ch : msg)
    {
      sum += ch;
    }
    char cs[4];
    std::snprintf(cs, sizeof(cs), "%03u", sum % 256);
    msg += std::string("10=") + cs + SOH;
    return msg;
  }

 private:
  // Value of the first `tag=` field as u64 (0 when absent / non-numeric).
  static uint64_t tagValueU64(const std::string& msg, int tag)
  {
    const std::string needle = std::to_string(tag) + "=";
    size_t p = 0;
    while ((p = msg.find(needle, p)) != std::string::npos)
    {
      if (p == 0 || msg[p - 1] == SOH)  // field boundary, not a substring of another tag
      {
        return std::strtoull(msg.c_str() + p + needle.size(), nullptr, 10);
      }
      p += needle.size();
    }
    return 0;
  }
};

// FIX session-layer state: monotonic outbound MsgSeqNum, inbound MsgSeqNum
// validation, CompIDs from the gateway configuration and SendingTime.
//
// Scope: sequencing/framing building block for embedded and test use. The
// full session layer -- Logon negotiation, Heartbeat/TestRequest liveness,
// ResendRequest 35=2 / SequenceReset-GapFill 35=4, PossDup replay -- lives in
// fix_session.h (FixSessionHost / FixConnection), wired into the gateways via
// setFixSession. See docs/venue/perimeter.md.
class FixSession
{
 public:
  FixSession(std::string senderCompId, std::string targetCompId)
      : sender_(std::move(senderCompId)), target_(std::move(targetCompId))
  {
  }

  // Encode an exec report as the next sequenced session message. Empty string
  // = the event has no FIX mapping (the seq is NOT consumed).
  std::string encode(const OutboundEvent& ev, int64_t wallClockNs)
  {
    const std::string msg =
        FixCodec::encode(ev, nextOut_, sender_, target_, sendingTime(wallClockNs));
    if (!msg.empty())
    {
      ++nextOut_;
    }
    return msg;
  }

  uint64_t nextOutboundSeq() const noexcept { return nextOut_; }

  enum class InSeq : uint8_t
  {
    Ok,         // expected seq (or first message)
    Gap,        // seq jumped forward: messages lost -> reject the session
    Duplicate,  // seq at or below the last accepted one
    Missing,    // no tag 34: structurally invalid FIX 4.4
  };

  // Validate the inbound MsgSeqNum. Ok advances the expectation; anything else
  // leaves it unchanged so the caller can terminate the session.
  InSeq acceptInbound(const std::string& msg)
  {
    const uint64_t seq = FixCodec::msgSeqNum(msg);
    if (seq == 0)
    {
      return InSeq::Missing;
    }
    if (seq == expectedIn_)
    {
      ++expectedIn_;
      return InSeq::Ok;
    }
    return seq > expectedIn_ ? InSeq::Gap : InSeq::Duplicate;
  }

  uint64_t expectedInboundSeq() const noexcept { return expectedIn_; }

  // UTCTimestamp for tag 52: YYYYMMDD-HH:MM:SS.sss from wall-clock ns.
  static std::string sendingTime(int64_t wallClockNs)
  {
    const time_t secs = static_cast<time_t>(wallClockNs / 1'000'000'000);
    const int millis = static_cast<int>((wallClockNs / 1'000'000) % 1000);
    tm g{};
    gmtime_r(&secs, &g);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d%02d%02d-%02d:%02d:%02d.%03d", g.tm_year + 1900,
                  g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec, millis);
    return buf;
  }

 private:
  std::string sender_;
  std::string target_;
  uint64_t nextOut_{1};
  uint64_t expectedIn_{1};
};

}  // namespace flox::venue
