/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Deterministic command-stream generators for the venue engine.
 *
 * Every profile here is a pure function of its Params: same seed, same stream,
 * on every platform and in every process. Nothing reads a clock, a global RNG,
 * or an environment variable -- the golden replay stores hashes of what these
 * streams produce, so a generator that drifted by one command would show up as
 * a behaviour change in the engine and send somebody looking in the wrong file.
 *
 * The profiles live here rather than in one test because more than one caller
 * drives them: the differential fuzz uses mixedCommand as its command source,
 * the golden replay builds its corpus from all of them, and a benchmark can
 * reach for the same flow without copying it. A second copy of "the same"
 * stream would drift, and the drift would look like an engine bug.
 */
#pragma once

#include "flox-venue/messages.h"

#include <cstdint>
#include <vector>

namespace flox::venue::workload
{

struct Params
{
  SymbolId symbol{1};
  double mid{100.0};
  double tick{0.01};
  int spreadTicks{50};
  uint64_t seed{0x1234abcdULL};
  size_t count{1000};
  // Distinct account ids a profile draws from (ids are 1..accounts). Small on
  // purpose: accounts have to collide for self-trade prevention, MM protection
  // and position limits to be reached at all.
  uint32_t accounts{8};
  // How far the perp mark walks either side of `mid`, in ticks. Deliberately
  // separate from the order flow's own spread: the mark is what decides
  // maintenance margin, and a mark confined to the prices positions were
  // opened at never liquidates anybody.
  int64_t markSpanTicks{2000};
};

// xorshift64: the same three shifts every profile here has always used, named
// once so a profile cannot quietly pick a different generator.
struct Rng
{
  uint64_t s{1};
  uint64_t next() noexcept
  {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
};

// Symmetric buy/sell limit flow around `mid`. Symmetric so crossings are
// frequent and resting depth stays bounded.
inline std::vector<InboundCommand> symmetricLimits(const Params& p)
{
  const int64_t midRaw = Price::fromDouble(p.mid).raw();
  const int64_t tickRaw = Price::fromDouble(p.tick).raw();
  const uint64_t span = static_cast<uint64_t>(2 * p.spreadTicks + 1);

  std::vector<InboundCommand> v;
  v.reserve(p.count);
  uint64_t s = p.seed;
  auto next = [&]() noexcept
  {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  };

  for (size_t i = 0; i < p.count; ++i)
  {
    const uint64_t r = next();
    NewOrder o;
    o.id = i + 1;
    o.symbol = p.symbol;
    o.side = (r & 1U) ? Side::BUY : Side::SELL;
    o.type = OrderType::LIMIT;
    const int ticks = static_cast<int>((r >> 1) % span) - p.spreadTicks;
    o.price = Price::fromRaw(midRaw + static_cast<int64_t>(ticks) * tickRaw);
    o.quantity = Quantity::fromDouble(1.0 + static_cast<double>((r >> 10) % 5));
    o.tif = TimeInForce::GTC;
    o.accountId = 1 + ((r >> 20) % 64);
    v.emplace_back(o);
  }
  return v;
}

// One command of the mixed order-level flow: limits on both sides, markets,
// stop-markets, IOC, post-only, icebergs, GTD, OCO groups, pegs, cancels and
// modifies of earlier ids. `s` and `nextId` are carried across calls, so the
// stream is a function of the starting seed alone.
//
// Cancels and modifies deliberately name ids that may already be gone: an
// engine has to answer a stale cancel the same way every time, and a generator
// that only ever cancelled live orders would never ask.
inline InboundCommand mixedCommand(uint64_t& s, OrderId& nextId, const Params& p)
{
  auto next = [&]() noexcept
  {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  };
  const uint64_t r = next();
  const int64_t midRaw = Price::fromDouble(p.mid).raw();
  const int64_t tickRaw = Price::fromDouble(p.tick).raw();
  const uint32_t kind = r % 100;

  if (kind < 15 && nextId > 1)
  {
    const OrderId victim = 1 + (next() % (nextId - 1));
    return CancelOrder{victim, p.symbol, 1};
  }
  if (kind < 25 && nextId > 1)
  {
    const OrderId victim = 1 + (next() % (nextId - 1));
    const int ticks = static_cast<int>((next() % 101)) - 50;
    const Price newPrice = Price::fromRaw(midRaw + static_cast<int64_t>(ticks) * tickRaw);
    const Quantity newQty = Quantity::fromDouble(1.0 + static_cast<double>(next() % 6));
    return ModifyOrder{victim, p.symbol, newPrice, newQty, 1};
  }

  NewOrder o;
  o.id = nextId++;
  o.symbol = p.symbol;
  o.side = (r & 1U) ? Side::BUY : Side::SELL;
  o.quantity = Quantity::fromDouble(1.0 + static_cast<double>((r >> 10) % 5));
  o.accountId = 1 + ((r >> 20) % p.accounts);

  if (kind < 30)
  {
    o.type = OrderType::MARKET;
  }
  else if (kind < 40)
  {
    o.type = OrderType::STOP_MARKET;
    const int ticks = static_cast<int>((r >> 1) % 101) - 50;
    o.triggerPrice = Price::fromRaw(midRaw + static_cast<int64_t>(ticks) * tickRaw);
  }
  else
  {
    o.type = OrderType::LIMIT;
    const int ticks = static_cast<int>((r >> 1) % 101) - 50;  // +/-50 ticks
    o.price = Price::fromRaw(midRaw + static_cast<int64_t>(ticks) * tickRaw);
    const uint32_t t = (r >> 32) % 100;
    if (t < 12)
    {
      o.tif = TimeInForce::IOC;
    }
    else if (t < 17)
    {
      o.postOnly = true;
    }
    else if (t < 30)
    {
      o.visibleQuantity = Quantity::fromDouble(1.0);  // iceberg peak 1 (if qty > 1)
    }
    else if (t < 42)
    {
      o.tif = TimeInForce::GTD;  // some expire, some never
      o.expiryNs = SeqNanos::fromRaw(static_cast<int64_t>((r >> 40) % 2'000'000));
    }
    else if (t < 50)
    {
      o.ocoGroup = 1 + ((r >> 44) % 6);  // small set -> siblings collide
    }
    else if (t < 60)
    {
      const uint32_t pr = static_cast<uint32_t>((r >> 48) % 3);
      o.peg = pr == 0 ? PegRef::Bid : (pr == 1 ? PegRef::Ask : PegRef::Mid);
    }
  }
  return InboundCommand{o};
}

inline std::vector<InboundCommand> mixedFlow(const Params& p)
{
  std::vector<InboundCommand> v;
  v.reserve(p.count);
  uint64_t s = p.seed;
  OrderId nextId = 1;
  for (size_t i = 0; i < p.count; ++i)
  {
    v.push_back(mixedCommand(s, nextId, p));
  }
  return v;
}

// Maker flow: two-sided quotes replaced in place, last-look limit makers,
// aggressive IOC takers against them, mass cancels, and self-trade-prevention
// modes on both the quote and the plain orders.
//
// The holds this flow opens are answered by the caller, not here: a
// LastLookDecision names a heldId the engine invents, so a pre-built command
// vector cannot carry one. The generator's job is to make holds happen.
inline std::vector<InboundCommand> makerFlow(const Params& p)
{
  const int64_t midRaw = Price::fromDouble(p.mid).raw();
  const int64_t tickRaw = Price::fromDouble(p.tick).raw();
  const STPMode stpModes[4] = {STPMode::None, STPMode::CancelOldest, STPMode::CancelNewest,
                               STPMode::Decrement};

  std::vector<InboundCommand> v;
  v.reserve(p.count);
  Rng rng{p.seed};
  OrderId nextId = 1000;
  for (size_t i = 0; i < p.count; ++i)
  {
    const uint64_t r = rng.next();
    const uint32_t kind = r % 100;
    const uint64_t acct = 1 + ((r >> 8) % p.accounts);
    const int ticks = static_cast<int>((r >> 16) % 21) - 10;
    const int64_t refRaw = midRaw + static_cast<int64_t>(ticks) * tickRaw;

    if (kind < 30)
    {
      // A quote replaces this account's previous quote in place: the ids are a
      // function of the account, not of the iteration.
      Quote q{};
      q.bidId = 1 + acct * 2;
      q.askId = 2 + acct * 2;
      q.symbol = p.symbol;
      q.bidPrice = Price::fromRaw(refRaw - 2 * tickRaw);
      q.bidQty = Quantity::fromDouble(1.0 + static_cast<double>((r >> 24) % 4));
      q.askPrice = Price::fromRaw(refRaw + 2 * tickRaw);
      q.askQty = Quantity::fromDouble(1.0 + static_cast<double>((r >> 28) % 4));
      q.accountId = acct;
      q.stp = stpModes[(r >> 32) % 4];
      q.lastLook = ((r >> 34) % 4) == 0;
      q.postOnly = ((r >> 36) % 8) == 0;
      v.emplace_back(q);
      continue;
    }
    if (kind < 38)
    {
      v.emplace_back(MassCancel{acct, p.symbol});
      continue;
    }

    NewOrder o;
    o.id = nextId++;
    o.symbol = p.symbol;
    o.side = (r & 1U) ? Side::BUY : Side::SELL;
    o.type = OrderType::LIMIT;
    o.price = Price::fromRaw(refRaw);
    o.quantity = Quantity::fromDouble(1.0 + static_cast<double>((r >> 40) % 5));
    o.accountId = acct;
    o.stp = stpModes[(r >> 44) % 4];
    if (kind < 60)
    {
      o.lastLook = true;  // resting maker whose fills are held
    }
    else if (kind < 80)
    {
      o.tif = TimeInForce::IOC;  // taker that walks into the holds above
    }
    else if (kind < 90)
    {
      o.tif = TimeInForce::GTD;
      o.expiryNs = SeqNanos::fromRaw(static_cast<int64_t>(i) + 1 + ((r >> 48) % 64));
    }
    else
    {
      const uint32_t pr = static_cast<uint32_t>((r >> 50) % 3);
      o.peg = pr == 0 ? PegRef::Bid : (pr == 1 ? PegRef::Ask : PegRef::Mid);
      o.pegOffsetRaw = (static_cast<int64_t>((r >> 52) % 3) - 1) * tickRaw;
    }
    v.emplace_back(o);
  }
  return v;
}

// Perp flow: limit / market / stop-market / stop-limit orders with reduce-only
// slices, interleaved with mark moves, funding settlements, operator force
// closes and hand corrections.
//
// Unpriced flow is in on purpose: a perp market or stop order is margined
// against the price BAND rather than its own price, and a LIMIT-only stream
// never reaches that arithmetic.
//
// The mark walks much wider than the order flow does (+/-20 against a +/-0.30
// spread of entry prices). That asymmetry is what makes maintenance margin
// bite: a mark that stays inside the spread of the prices positions were
// opened at never produces an unrealized loss large enough to liquidate
// anybody, and a perp corpus with no liquidation in it does not watch the
// liquidation or deleveraging path at all.
inline std::vector<InboundCommand> perpFlow(const Params& p)
{
  const int64_t midRaw = Price::fromDouble(p.mid).raw();
  const int64_t tickRaw = Price::fromDouble(p.tick).raw();

  std::vector<InboundCommand> v;
  v.reserve(p.count + p.accounts);
  Rng rng{p.seed};
  OrderId nextId = 1;
  for (size_t i = 0; i < p.count; ++i)
  {
    const uint64_t r = rng.next();
    const uint32_t kind = r % 100;
    if (kind < 15 && nextId > 1)
    {
      v.emplace_back(CancelOrder{1 + (rng.next() % (nextId - 1)), p.symbol, 0});
      continue;
    }
    const int priceTicks = static_cast<int>((r >> 1) % 61) - 30;
    const int64_t markTicks =
        (static_cast<int64_t>((r >> 24) % 41) - 20) * (p.markSpanTicks / 20);
    const Price mark = Price::fromRaw(midRaw + markTicks * tickRaw);
    if (kind < 22)
    {
      v.emplace_back(SetMark{p.symbol, mark});
      continue;
    }
    if (kind < 25)
    {
      // Funding at a rate that is small but not symmetric around zero: a payer
      // at full leverage must occasionally be unable to afford it.
      const double rate = (static_cast<double>((r >> 8) % 21) - 8.0) * 0.0001;
      v.emplace_back(ApplyFunding{p.symbol, rate, mark});
      continue;
    }
    if (kind < 27)
    {
      // Operator verbs on a position that may or may not exist -- both answers
      // have to be reproducible.
      const uint64_t acct = 1 + ((r >> 8) % p.accounts);
      if ((r >> 40) % 2 == 0)
      {
        v.emplace_back(ForceClosePosition{acct, p.symbol, 0});
      }
      else
      {
        AdjustPosition a{};
        a.accountId = acct;
        a.symbol = p.symbol;
        a.qtyDeltaRaw = (static_cast<int64_t>((r >> 44) % 5) - 2) * Quantity::fromDouble(1.0).raw();
        a.entryRaw = 0;  // keep the average entry
        a.reason = AdjustReason::Reconciliation;
        v.emplace_back(a);
      }
      continue;
    }

    NewOrder o;
    o.id = nextId++;
    o.symbol = p.symbol;
    o.side = (r & 1U) ? Side::BUY : Side::SELL;
    o.accountId = 1 + ((r >> 8) % p.accounts);
    o.price = Price::fromRaw(midRaw + static_cast<int64_t>(priceTicks) * tickRaw);
    o.quantity = Quantity::fromDouble(1.0 + static_cast<double>((r >> 20) % 5));
    const uint32_t t = static_cast<uint32_t>((r >> 32) % 100);
    if (t < 15)
    {
      o.type = OrderType::MARKET;
    }
    else if (t < 25)
    {
      o.type = OrderType::STOP_MARKET;
      const int tt = static_cast<int>((r >> 40) % 61) - 30;
      o.triggerPrice = Price::fromRaw(midRaw + static_cast<int64_t>(tt) * tickRaw);
    }
    else if (t < 32)
    {
      o.type = OrderType::STOP_LIMIT;
      const int tt = static_cast<int>((r >> 40) % 61) - 30;
      o.triggerPrice = Price::fromRaw(midRaw + static_cast<int64_t>(tt) * tickRaw);
    }
    else
    {
      o.type = OrderType::LIMIT;
    }
    o.reduceOnly = ((r >> 48) % 100) < 20;
    v.emplace_back(o);
  }
  return v;
}

}  // namespace flox::venue::workload
