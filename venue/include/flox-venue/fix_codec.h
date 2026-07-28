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

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_map>

namespace flox::venue
{

class FixCodec
{
 public:
  static constexpr char SOH = '\x01';

  // ---- inbound: FIX message -> InboundCommand ----
  static std::optional<InboundCommand> decode(const std::string& msg)
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

    auto has = [&](int t)
    { return f.count(t) != 0; };
    auto s = [&](int t)
    { return has(t) ? f[t] : std::string{}; };

    // FIX integrity: if a CheckSum (tag 10) is present it MUST be correct --
    // sum of every byte up to and including the SOH before "10=", mod 256. A
    // corrupted message with a bad checksum is rejected. (Lenient when absent,
    // for internal/test callers that don't append one.)
    if (has(10))
    {
      const std::string marker = std::string(1, SOH) + "10=";
      const size_t p = msg.rfind(marker);
      if (p != std::string::npos)
      {
        unsigned sum = 0;
        for (size_t k = 0; k <= p; ++k)
        {
          sum += static_cast<unsigned char>(msg[k]);
        }
        if ((sum % 256) != static_cast<unsigned>(std::atoi(f[10].c_str())))
        {
          return std::nullopt;  // checksum mismatch -> reject
        }
      }
    }
    auto u64 = [&](int t)
    { return static_cast<uint64_t>(std::strtoull(s(t).c_str(), nullptr, 10)); };
    auto sym = [&](int t)
    { return static_cast<SymbolId>(std::strtoul(s(t).c_str(), nullptr, 10)); };
    auto price = [&](int t)
    { return safeDecimal<Price>(std::strtod(s(t).c_str(), nullptr)); };
    auto qtyf = [&](int t)
    { return safeDecimal<Quantity>(std::strtod(s(t).c_str(), nullptr)); };

    const std::string type = s(35);
    if (type == "D")  // NewOrderSingle
    {
      NewOrder o;
      o.id = u64(11);
      o.clientOrderId = u64(11);
      o.symbol = sym(55);
      o.side = (s(54) == "2") ? Side::SELL : Side::BUY;
      o.quantity = qtyf(38);
      o.accountId = u64(1);
      switch (std::atoi(s(40).c_str()))  // OrdType
      {
        case 1:
          o.type = OrderType::MARKET;
          break;
        case 3:
          o.type = OrderType::STOP_MARKET;
          break;
        case 4:
          o.type = OrderType::STOP_LIMIT;
          break;
        default:
          o.type = OrderType::LIMIT;
          break;
      }
      if (has(44))
      {
        o.price = price(44);
      }
      if (has(99))
      {
        o.triggerPrice = price(99);
      }
      if (has(111))
      {
        o.visibleQuantity = qtyf(111);  // MaxFloor -> iceberg peak
      }
      switch (std::atoi(s(59).c_str()))  // TimeInForce
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
      CancelOrder c;
      c.id = u64(41);  // OrigClOrdID
      c.symbol = sym(55);
      c.accountId = u64(1);
      return InboundCommand{c};
    }
    if (type == "G")  // OrderCancelReplaceRequest
    {
      ModifyOrder m;
      m.id = u64(41);
      m.symbol = sym(55);
      if (has(44))
      {
        m.newPrice = price(44);
      }
      m.newQty = qtyf(38);
      m.accountId = u64(1);
      return InboundCommand{m};
    }
    return std::nullopt;
  }

  // ---- outbound: OutboundEvent -> ExecutionReport (35=8) ----
  static std::string encode(const OutboundEvent& ev)
  {
    std::string b;  // body after 35
    auto add = [&](int tag, const std::string& val)
    { b += std::to_string(tag) + "=" + val + SOH; };
    auto num = [](Price p)
    { return std::to_string(p.toDouble()); };
    auto qnum = [](Quantity q)
    { return std::to_string(q.toDouble()); };

    add(35, "8");  // ExecutionReport
    if (const auto* a = std::get_if<OrderAccepted>(&ev))
    {
      add(37, std::to_string(a->id));
      add(11, std::to_string(a->id));
      add(55, std::to_string(a->symbol));
      add(54, a->side == Side::SELL ? "2" : "1");
      add(150, "0");  // ExecType New
      add(39, "0");   // OrdStatus New
      add(151, qnum(a->leavesQty));
      add(44, num(a->price));
    }
    else if (const auto* x = std::get_if<OrderExecuted>(&ev))
    {
      add(37, std::to_string(x->id));
      add(55, std::to_string(x->symbol));
      add(150, "F");                     // ExecType Trade
      add(39, x->complete ? "2" : "1");  // Filled / Partially filled
      add(32, qnum(x->lastQty));         // LastQty
      add(31, num(x->lastPx));           // LastPx -- price of this fill
      add(6, num(x->lastPx));            // AvgPx (single-fill report)
      add(151, qnum(x->leavesQty));      // LeavesQty
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
      add(151, qnum(m->leavesQty));
      add(44, num(m->price));
    }
    else
    {
      return {};  // Trade/Triggered are market-data, not exec reports
    }

    return frame(b);
  }

 private:
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
};

}  // namespace flox::venue
