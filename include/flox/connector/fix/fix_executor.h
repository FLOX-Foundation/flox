/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * A FIX counterparty behind the router's order sink. FixRoutableExecutor
 * implements IRoutableExecutor over a FixInitiator, so OrderRouter registers
 * several FIX counterparties the way it registers any other executor and
 * switches between them on its own routing strategy.
 *
 * The router's cancel() carries only an OrderId, while FIX wants the symbol and
 * side of the order being cancelled. The executor therefore remembers the few
 * bytes of each live order it submitted and drops the entry on the terminal
 * report, so the map tracks what is actually working rather than everything the
 * session ever sent.
 */
#pragma once

#include "flox/connector/fix/fix_client_codec.h"
#include "flox/connector/fix/fix_initiator.h"
#include "flox/execution/order_router.h"
#include "flox/util/base/time.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <variant>

namespace flox::fix
{

class FixRoutableExecutor : public IRoutableExecutor
{
 public:
  // Wall-clock nanoseconds, for SendingTime (52) and the session timers. FIX
  // timestamps are UTC, so this is deliberately not the steady clock.
  using ClockFn = std::function<int64_t()>;

  explicit FixRoutableExecutor(FixInitiator& initiator, uint64_t accountId = 0)
      : initiator_(initiator), accountId_(accountId)
  {
  }

  void setClock(ClockFn fn) { clock_ = std::move(fn); }

  // Feed every inbound report through this so the live-order map stays honest.
  // Wire it as (or inside) the initiator's report handler.
  void onReport(const InboundReport& r)
  {
    const auto* e = std::get_if<ExecutionReport>(&r);
    if (e == nullptr)
    {
      return;
    }
    switch (e->execType)
    {
      case ExecType::Fill:
      case ExecType::Canceled:
      case ExecType::Rejected:
        forget(e->clOrdId != 0 ? e->clOrdId : e->orderId);
        break;
      default:
        break;  // New, partial fills, replaces and last-look holds stay live
    }
  }

  void submit(SymbolId symbol, Side side, Price price, Quantity quantity,
              OrderId orderId) override
  {
    NewOrderRequest o;
    o.clOrdId = orderId;
    o.symbol = symbol;
    o.side = side;
    o.type = price.raw() == 0 ? OrderType::MARKET : OrderType::LIMIT;
    o.price = price;
    o.quantity = quantity;
    o.accountId = accountId_;
    {
      std::lock_guard<std::mutex> lk(m_);
      live_[orderId] = Live{symbol, side, quantity};
    }
    if (!initiator_.submit(o, clock_()))
    {
      forget(orderId);  // never sent: do not leave a phantom working order
    }
  }

  void cancel(OrderId orderId) override
  {
    Live l{};
    {
      std::lock_guard<std::mutex> lk(m_);
      const auto it = live_.find(orderId);
      if (it == live_.end())
      {
        return;  // nothing working under that id; a cancel would name no order
      }
      l = it->second;
    }
    CancelRequest c;
    c.origClOrdId = orderId;
    c.clOrdId = orderId;
    c.symbol = l.symbol;
    c.side = l.side;
    c.quantity = l.quantity;
    c.accountId = accountId_;
    initiator_.cancel(c, clock_());
  }

  // Amend in place: FIX 4.4 spells it OrderCancelReplaceRequest (35=G), and it
  // is a different message from a cancel followed by a new order -- the venue
  // may keep queue position across it.
  void replace(OrderId orderId, int64_t priceRaw, int64_t quantityRaw)
  {
    Live l{};
    {
      std::lock_guard<std::mutex> lk(m_);
      const auto it = live_.find(orderId);
      if (it == live_.end())
      {
        return;
      }
      l = it->second;
      it->second.quantity = Quantity::fromRaw(quantityRaw);
    }
    CancelReplaceRequest r;
    r.origClOrdId = orderId;
    r.clOrdId = orderId;
    r.symbol = l.symbol;
    r.side = l.side;
    r.price = Price::fromRaw(priceRaw);
    r.quantity = Quantity::fromRaw(quantityRaw);
    r.accountId = accountId_;
    initiator_.replace(r, clock_());
  }

  size_t liveOrderCount()
  {
    std::lock_guard<std::mutex> lk(m_);
    return live_.size();
  }

 private:
  struct Live
  {
    SymbolId symbol{};
    Side side{Side::BUY};
    Quantity quantity{};
  };

  void forget(OrderId id)
  {
    std::lock_guard<std::mutex> lk(m_);
    live_.erase(id);
  }

  static int64_t wallClockNs()
  {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  FixInitiator& initiator_;
  uint64_t accountId_;
  ClockFn clock_{&FixRoutableExecutor::wallClockNs};
  std::mutex m_;
  std::unordered_map<OrderId, Live> live_;
};

}  // namespace flox::fix
