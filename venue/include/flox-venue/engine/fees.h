/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/event_sink.h"
#include "flox-venue/ledger.h"
#include "flox-venue/messages.h"
#include "flox-venue/symbol_config.h"

#include "flox/clearing/fee_schedule.h"

#include <cstdint>
#include <utility>

namespace flox::venue::engine
{

// Fee rates travel as raws at this scale, the same power of ten money and
// funding use: one raw is 1e-8 of the notional, i.e. 1e-4 of a basis point.
// 2.5 bps is 25'000 raw. Anything a published fee ladder expresses -- tenths
// of a bp on the deepest VIP tier, a maker rebate -- is an exact integer here.
inline constexpr int64_t kFeeRateScale = kMoneyScale;

// The schedule is configuration, and configuration is written in basis points
// by a human, so it arrives as a double. It becomes a raw exactly ONCE, here,
// on the way out of the schedule; every fee after that is integer arithmetic.
// Round to nearest, because 2.5 bps is not the same double as 2.5.
inline int64_t feeRateRawOf(double bps)
{
  // (bps / 10'000) * kFeeRateScale as one multiply: the factor is a ratio of
  // two exactly representable powers of ten, so it introduces no error of its
  // own and the only rounding is the one below.
  return roundDoubleToI64(bps * (static_cast<double>(kFeeRateScale) / 10'000.0));
}

// What a print costs its two sides, and where that money goes.
//
// The schedule is configuration the embedder installs (setFeeSchedule); it is
// not journaled, not hashed and not written into a snapshot, exactly like the
// credit hook -- a venue that restarts is re-wired by whoever wired it the
// first time. A snapshot CLONE does carry it, because the clone has to price
// a trade the same way the engine it was taken from would.
//
// Until a schedule is installed the component is OFF and charges nothing: an
// engine with no fee schedule prints no FeeCharged at all, which is what an
// unconfigured venue has always done.
//
// Three callers used to spell the same four lines: the ledgerless path (fee
// reports, no money), the spot settlement and the perp settlement. The notional
// they price against is the SYMBOL's notional -- the same arithmetic
// reservations and margin use -- and getting that wrong on one of the three
// was a bug waiting for the symbol that declared its own scales. It is written
// once here.
//
// The money is fixed point from end to end: the notional stays the raw the
// reservations use, the tier's rate becomes a raw once, and the fee is their
// integer mulDiv. The schedule's own feeFor() is not on this path -- it
// divides the notional down into a double to price it, and a double stops
// carrying a notional at 2^53 raw, which is about 9e7 quote units.
//
// The book is not here and neither is the ledger's ownership: the component is
// handed the ledger for the one call that moves money and keeps no pointer to
// it, so a copy of this class cannot reach another engine's balances.
class Fees
{
 public:
  void setSchedule(flox::FeeSchedule s)
  {
    schedule_ = std::move(s);
    enabled_ = true;
  }

  bool enabled() const noexcept { return enabled_; }

  // Fee REPORTS only, no money: the path where the engine has no ledger bound
  // and so has nothing to move, but the two sides still have to be told what
  // the print would have cost.
  void emit(const Trade& t, const SymbolConfig& cfg, int64_t nowRaw, const EventSink& sink)
  {
    if (!enabled_)
    {
      return;
    }
    const Amount notional = notionalOf(t, cfg);
    const auto [makerRateRaw, takerRateRaw] = ratesAt(nowRaw);
    sink(FeeCharged{t.makerId, cfg.id, Volume::fromRaw(feeRawOf(notional, makerRateRaw)), true,
                    t.makerAccount});
    sink(FeeCharged{t.takerId, cfg.id, Volume::fromRaw(feeRawOf(notional, takerRateRaw)), false,
                    t.takerAccount});
  }

  // Charge both sides through the ledger: participant -fee, venue +fee. Maker
  // first, taker second -- the same order the reports go out in above, and the
  // order the event stream has always carried.
  void settle(const Trade& t, const SymbolConfig& cfg, int64_t nowRaw, Ledger& ledger,
              uint64_t venueAccount, const EventSink& sink)
  {
    if (!enabled_)
    {
      return;
    }
    const Amount notional = notionalOf(t, cfg);
    const auto [makerRateRaw, takerRateRaw] = ratesAt(nowRaw);
    charge(t.makerId, t.makerAccount, feeRawOf(notional, makerRateRaw), true, cfg, ledger,
           venueAccount, sink);
    charge(t.takerId, t.takerAccount, feeRawOf(notional, takerRateRaw), false, cfg, ledger,
           venueAccount, sink);
  }

 private:
  // The print's notional at the symbol's own scales -- the same raw the
  // reservations and the margin are computed from, kept as a raw. The
  // schedule's own feeFor() divides it down into a double to price it, which
  // is where the last raw of a large print used to go.
  static Amount notionalOf(const Trade& t, const SymbolConfig& cfg)
  {
    return notionalRaw(t.price.raw(), t.quantity.raw(), cfg.priceScale, cfg.qtyScale);
  }

  // The active tier's two rates as raws. Resolving the tier is what advances
  // the schedule's rolling window, so both sides of a print are priced off one
  // lookup rather than two -- the maker and the taker of the same trade cannot
  // land in different tiers.
  std::pair<int64_t, int64_t> ratesAt(int64_t nowRaw)
  {
    const auto [makerBps, takerBps] = schedule_.currentBps(nowRaw);
    return {feeRateRawOf(makerBps), feeRateRawOf(takerBps)};
  }

  // What the print costs at `rateRaw`, in money raws.
  static int64_t feeRawOf(Amount notional, int64_t rateRaw)
  {
    return checkedNarrowI64(rateOnNotional(notional, rateRaw, kFeeRateScale));
  }

  // Signed move: participant -fee, venue +fee (conserves value; fee<0 = rebate).
  static void charge(OrderId id, uint64_t acct, int64_t feeRaw, bool maker, const SymbolConfig& cfg,
                     Ledger& ledger, uint64_t venueAccount, const EventSink& sink)
  {
    const Amount fee = static_cast<Amount>(feeRaw);
    ledger.credit(acct, cfg.quoteAsset, -fee);
    ledger.credit(venueAccount, cfg.quoteAsset, fee);
    sink(FeeCharged{id, cfg.id, Volume::fromRaw(feeRaw), maker, acct});
  }

  flox::FeeSchedule schedule_{};
  bool enabled_{false};
};

}  // namespace flox::venue::engine
