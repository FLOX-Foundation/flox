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
    const double notional = notionalOf(t, cfg);
    sink(FeeCharged{t.makerId, cfg.id, Volume::fromDouble(schedule_.feeFor(nowRaw, notional, true)),
                    true, t.makerAccount});
    sink(FeeCharged{t.takerId, cfg.id,
                    Volume::fromDouble(schedule_.feeFor(nowRaw, notional, false)), false,
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
    const double notional = notionalOf(t, cfg);
    charge(t.makerId, t.makerAccount, schedule_.feeFor(nowRaw, notional, true), true, cfg, ledger,
           venueAccount, sink);
    charge(t.takerId, t.takerAccount, schedule_.feeFor(nowRaw, notional, false), false, cfg, ledger,
           venueAccount, sink);
  }

 private:
  // The print's notional at the symbol's own scales, as a double, because that
  // is the shape flox::FeeSchedule prices in.
  static double notionalOf(const Trade& t, const SymbolConfig& cfg)
  {
    return static_cast<double>(
               notionalRaw(t.price.raw(), t.quantity.raw(), cfg.priceScale, cfg.qtyScale)) /
           kMoneyScale;
  }

  // Signed move: participant -fee, venue +fee (conserves value; fee<0 = rebate).
  static void charge(OrderId id, uint64_t acct, double feeD, bool maker, const SymbolConfig& cfg,
                     Ledger& ledger, uint64_t venueAccount, const EventSink& sink)
  {
    const Amount fee = static_cast<Amount>(Volume::fromDouble(feeD).raw());
    ledger.credit(acct, cfg.quoteAsset, -fee);
    ledger.credit(venueAccount, cfg.quoteAsset, fee);
    sink(FeeCharged{id, cfg.id, Volume::fromDouble(feeD), maker, acct});
  }

  flox::FeeSchedule schedule_{};
  bool enabled_{false};
};

}  // namespace flox::venue::engine
