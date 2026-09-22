/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

// MatchingEngine<Book>: pre-trade validation, admission, risk gates and order entry.
//
// Included only from flox-venue/matching_engine.h, which declares every member
// defined here. Including it directly gives a fragment with no class to attach
// to, so the include is refused rather than left to fail on the first method.
#ifndef FLOX_VENUE_MATCHING_ENGINE_INL
#error "flox-venue/engine/validate.inl is a fragment of flox-venue/matching_engine.h; include that instead"
#endif

namespace flox::venue
{

template <class Book>
void MatchingEngine<Book>::setCreditCheck(CreditCheck c)
{
  credit_.setCreditCheck(std::move(c));
}

template <class Book>
void MatchingEngine<Book>::setAdmissionProfile(uint64_t account, const AdmissionProfile& p)
{
  credit_.setAdmissionProfile(account, p);
}

template <class Book>
const std::unordered_map<uint64_t, AdmissionProfile>& MatchingEngine<Book>::admissionProfiles() const noexcept
{
  return credit_.admissionProfiles();
}

template <class Book>
uint64_t MatchingEngine<Book>::admissionRejects() const noexcept
{
  return credit_.admissionRejects();
}

template <class Book>
RejectReason MatchingEngine<Book>::validateConditional(const NewOrder& o) const
{
  return credit_.validateConditional(o, cfg_);
}

template <class Book>
RejectReason MatchingEngine<Book>::validate(const NewOrder& o) const
{
  if (o.symbol != cfg_.id)
  {
    return RejectReason::UnknownSymbol;
  }
  // Outermost first: a client whose order is refused deserves the reason that
  // tells it what to do next. Delisted means do not come back; closed means
  // next session; halted means something is wrong with the instrument now.
  if (session_.delisted())
  {
    return RejectReason::InstrumentDelisted;
  }
  if (session_.closed())
  {
    return RejectReason::MarketClosed;
  }
  if (cfg_.halted)
  {
    return RejectReason::Halted;
  }
  if (o.quantity.raw() <= 0)
  {
    return RejectReason::InvalidQuantity;
  }
  // Pro-rata allocation does not honour last look (a held slice cannot be
  // carved out of a proportional round), so the combination is refused at
  // admission instead of silently filling the maker as firm.
  if (o.lastLook && matcher_.policy() == MatchPolicy::ProRata)
  {
    return RejectReason::LastLookUnsupported;
  }
  if (!cfg_.minQty.isZero() && o.quantity < cfg_.minQty)
  {
    return RejectReason::InvalidQuantity;
  }
  if (!cfg_.lotSize.isZero() && (o.quantity.raw() % cfg_.lotSize.raw()) != 0)
  {
    return RejectReason::LotSizeViolation;
  }
  if (!cfg_.maxOrderQty.isZero() && cfg_.maxOrderQty < o.quantity)
  {
    return RejectReason::OrderTooLarge;  // fat-finger size
  }
  // Notional at the SYMBOL's scale, the same arithmetic reservations, margin
  // and fees use. `quantity * price` reads both raws under the compile-time
  // 1e8 scale instead, so on a symbol that declared its own the gate compares
  // a number that is off by (1e8/priceScale) * (1e8/qtyScale) -- coarser
  // scales turn the fat-finger check off, finer ones reject everything.
  // Bit-identical to the old expression at the default 1e8/1e8.
  if (o.type == OrderType::LIMIT && !cfg_.maxOrderNotional.isZero() &&
      static_cast<Amount>(cfg_.maxOrderNotional.raw()) <
          notionalRaw(o.price.raw(), o.quantity.raw(), cfg_.priceScale, cfg_.qtyScale))
  {
    return RejectReason::OrderTooLarge;  // fat-finger notional
  }
  if (o.type == OrderType::LIMIT)
  {
    if (o.price.raw() <= 0)
    {
      return RejectReason::InvalidPrice;
    }
    if (!cfg_.tickSize.isZero() && (o.price.raw() % cfg_.tickSize.raw()) != 0)
    {
      return RejectReason::TickSizeViolation;
    }
    if (!cfg_.minPrice.isZero() && o.price < cfg_.minPrice)
    {
      return RejectReason::InvalidPrice;
    }
    if (!cfg_.maxPrice.isZero() && cfg_.maxPrice < o.price)
    {
      return RejectReason::InvalidPrice;
    }
  }
  if (book_.contains(o.id) || stops_.contains(o.id))
  {
    return RejectReason::DuplicateOrderId;
  }
  // An id referenced by an active last-look hold is still live even when its
  // order is (fully held) out of the book: a reject will restore quantity
  // under that id, so a reused id would merge two unrelated orders.
  if (hasHoldsFor(o.id))
  {
    return RejectReason::DuplicateOrderId;
  }
  return RejectReason::None;
}

// Per-order perp risk gates shared by onNew and the trigger path (a triggered
// stop re-enters matching directly, NOT through onNew, so it must run these too
// -- otherwise a reduce-only stop opens an uncollateralized position and a
// plain stop bypasses the position cap). Caps a reduce-only order to the
// opposing position (0 reducible -> reject: nothing to reduce) and rejects a
// fill that would breach maxPositionQty. Mutates o.quantity for the cap.
// Entitlement gate. Every admission path consults this before anything else
// decides the order's fate, so a counterparty cannot reach the book through
// a path that forgot to ask. Declared in scripts/check_gate_reachability.py,
// which fails the build if a path stops calling it. The rules live in
// engine::Credit; the auction flag is the engine's, so it is handed over.
template <class Book>
RejectReason MatchingEngine<Book>::admissionGate(const NewOrder& o) const
{
  return credit_.admissionGate(o, session_.auction());
}

template <class Book>
bool MatchingEngine<Book>::admissionDenies(uint64_t account, uint8_t bit) const
{
  return credit_.admissionDenies(account, bit);
}

template <class Book>
RejectReason MatchingEngine<Book>::perpRiskGate(NewOrder& o)
{
  if (!cfg_.linearPerp)
  {
    return RejectReason::None;
  }
  if (o.reduceOnly)
  {
    const int64_t posQ = clearing_.positionQty(o.accountId);
    int64_t reducible = (o.side == Side::BUY && posQ < 0)    ? -posQ
                        : (o.side == Side::SELL && posQ > 0) ? posQ
                                                             : 0;
    // The account's reduce-only orders already resting on this side are
    // queued against the SAME position. Sizing each one against the whole
    // position lets N of them rest against a position only one of them can
    // close, so the book advertises reduce-only depth that cannot trade --
    // and an all-or-none taker that believed the depth gets a partial print.
    reducible -= restingReduceOnlyRaw(o.accountId, o.side, o.id);
    if (reducible < 0)
    {
      reducible = 0;
    }
    if (o.quantity.raw() > reducible)
    {
      o.quantity = Quantity::fromRaw(reducible);
    }
    if (o.quantity.raw() <= 0)
    {
      return RejectReason::InvalidQuantity;  // nothing to reduce -> reduce-only never opens
    }
  }
  if (!cfg_.maxPositionQty.isZero())
  {
    const int64_t posQ = clearing_.positionQty(o.accountId);
    const int64_t worst = posQ + (o.side == Side::BUY ? o.quantity.raw() : -o.quantity.raw());
    if (iabs64(worst) > cfg_.maxPositionQty.raw())
    {
      return RejectReason::PositionLimitExceeded;
    }
  }
  return RejectReason::None;
}

// Total reduce-only quantity the account already has resting on `side`
// (displayed peak plus hidden reserve), ignoring `exclude` -- the order being
// re-admitted on the modify and stop-trigger paths.
template <class Book>
int64_t MatchingEngine<Book>::restingReduceOnlyRaw(uint64_t account, Side side, OrderId exclude) const
{
  const auto* own = pub_.ordersOf(account);
  if (own == nullptr)
  {
    return 0;
  }
  const std::unordered_set<OrderId>& ids = *own;
  int64_t sum = 0;
  // order: not observable -- int64 sum of the account's reduce-only leaves
  for (OrderId id : ids)
  {
    if (id == exclude)
    {
      continue;
    }
    const RestingOrder* r = book_.find(id);
    if (r != nullptr && r->reduceOnly && r->side == side)
    {
      sum += r->leaves.raw() + r->hidden.raw();
    }
  }
  return sum;
}

// legFillLimit against the position the account holds RIGHT NOW (plus
// whatever a dry run has already planned for it). Resolving the position is
// the engine's half; the limits themselves are engine::Credit's.
template <class Book>
int64_t MatchingEngine<Book>::legFillLimit(uint64_t account, Side side, bool reduceOnly, int64_t want,
                                           CancelReason& reason, int64_t posDeltaRaw) const
{
  const int64_t posQ = clearing_.positionQty(account) + posDeltaRaw;
  return credit_.legFillLimit(posQ, side, reduceOnly, want, reason, cfg_);
}

// Fill-time risk re-check for one prospective bite (see the FillLimit hook).
// reduceOnly and maxPositionQty are gated at submit / stop-trigger / modify,
// but a RESTING order fills later, against a position that has since moved:
// a reduce-only order whose position shrank would open or flip it (with zero
// margin -- reduce-only reserves none), and orders that each passed the cap
// individually would settle past it together. Both legs are therefore
// re-measured here, on live engine state, so a replay reproduces it.
template <class Book>
FillLimit MatchingEngine<Book>::fillLimit(const RestingOrder& maker, const NewOrder& taker, Quantity want) const
{
  if (ledger_ == nullptr)
  {
    // No ledger: the engine keeps no positions to measure against.
    return FillLimit{want, want, want, false, false, CancelReason::ReduceOnlyNotReducing};
  }
  return pairFillLimit(maker.accountId, maker.side, maker.reduceOnly, taker.accountId, taker.side,
                       taker.reduceOnly, want);
}

// fillLimit against a state the sweep has not reached yet: `deltas` holds the
// position change each account would already have taken on. Used by the
// all-or-none precheck, which has to ask about every maker in the crossing
// range as though the earlier ones had already printed.
template <class Book>
FillLimit MatchingEngine<Book>::fillLimitDry(const RestingOrder& maker, const NewOrder& taker, Quantity want,
                                             const PositionDeltas& deltas) const
{
  if (ledger_ == nullptr)
  {
    return FillLimit{want, want, want, false, false, CancelReason::ReduceOnlyNotReducing};
  }
  return pairFillLimit(maker.accountId, maker.side, maker.reduceOnly, taker.accountId, taker.side,
                       taker.reduceOnly, want, deltas.of(maker.accountId),
                       deltas.of(taker.accountId));
}

// fillLimit over two legs described directly -- used by the auction uncross,
// where BOTH legs are resting orders and neither is an incoming NewOrder.
template <class Book>
FillLimit MatchingEngine<Book>::pairFillLimit(uint64_t makerAcct, Side makerSide, bool makerReduceOnly,
                                              uint64_t takerAcct, Side takerSide, bool takerReduceOnly, Quantity want,
                                              int64_t makerPosDeltaRaw, int64_t takerPosDeltaRaw) const
{
  const int64_t makerPosQ = clearing_.positionQty(makerAcct) + makerPosDeltaRaw;
  const int64_t takerPosQ = clearing_.positionQty(takerAcct) + takerPosDeltaRaw;
  return credit_.pairFillLimit(makerPosQ, makerSide, makerReduceOnly, takerPosQ, takerSide,
                               takerReduceOnly, want, cfg_);
}

// True if `clOrdId` was already used by `account` inside the dedup window
// (see engine/clordid_window.h) and the caller must therefore refuse the
// submission; false and newly registered otherwise. Factored out of onNew so
// a Quote -- one submission that becomes two child orders (see onQuote) --
// can register its clientOrderId ONCE for both legs instead of racing
// itself: the second leg's onNew would otherwise find the first leg's insert
// already sitting in the current generation and refuse a submission that
// never repeated anything.
template <class Book>
bool MatchingEngine<Book>::clOrdIdDuplicate(uint64_t account, uint64_t clOrdId)
{
  return clOrdIds_.duplicate(account, clOrdId, now_.raw(), cfg_.clOrdIdWindowNs);
}

// `clOrdIdChecked` is true only for the two calls onQuote makes on behalf
// of one Quote's legs: the dedup registration already happened once, for
// the quote as a whole, before either leg was built.
template <class Book>
void MatchingEngine<Book>::onNew(NewOrder o, bool clOrdIdChecked)
{
  // Entitlement first: an order the counterparty may not send should not
  // consume a clientOrderId, link an OCO group or reach any later gate.
  if (const RejectReason r = credit_.admissionGate(o, session_.auction()); r != RejectReason::None)
  {
    credit_.countAdmissionReject();
    sink_(OrderRejected{o.id, o.symbol, r, o.accountId, o.clientOrderId});
    return;
  }
  // clientOrderId dedup (per account, window = engine session/uptime; see
  // docs/venue/matching.md). Registered on receipt, BEFORE any other gate:
  // a resend of an already-seen clOrdId must reject deterministically even
  // when the first instance has long filled or canceled -- that is the whole
  // point (double execution after an ambiguous disconnect). Replay-safe: the
  // index is rebuilt by the same submits during journal replay.
  if (!clOrdIdChecked && clOrdIdDuplicate(o.accountId, o.clientOrderId))
  {
    sink_(OrderRejected{o.id, o.symbol, RejectReason::DuplicateClientOrderId, o.accountId, o.clientOrderId});
    return;
  }
  if (o.ocoGroup > 0)
  {
    oco_.link(o.id, o.ocoGroup);  // link before matching so a taker fill triggers OCO too
  }
  // Any early exit before the order commits (parks as a stop, or passes every
  // gate and reaches matching/resting) must unlink it from its OCO group --
  // otherwise a rejected leg lingers in the group and later cancels a reused
  // id. `committed` is set once the order is live; the guard cleans up the rest.
  bool committed = false;
  struct OcoCleanup
  {
    MatchingEngine* self;
    OrderId id;
    bool linked;
    const bool* committed;
    ~OcoCleanup()
    {
      if (linked && !*committed)
      {
        self->oco_.unlink(id);
      }
    }
  } ocoCleanup{this, o.id, o.ocoGroup > 0, &committed};
  // POST_ONLY is spelled two ways -- a time in force and a flag -- and the
  // matcher reads only the flag. Every wire decoder sets the flag, so this
  // normalization exists for the in-process caller that chose the other
  // spelling and would otherwise get an ordinary aggressor.
  if (o.tif == TimeInForce::POST_ONLY)
  {
    o.postOnly = true;
  }
  if (o.peg != PegRef::None)
  {
    o.type = OrderType::LIMIT;  // a peg is a passive limit priced off the book
    o.price = Price::fromRaw(pegTargetRaw(o.side, o.peg, o.pegOffsetRaw));
  }
  if (isConditional(o.type))
  {
    // Conditional orders branch off before validate(), which is written for
    // an order carrying a live limit price. They still have a price (the
    // trigger) and a quantity, and both must obey the instrument -- an
    // off-tick or out-of-band trigger, or a sub-lot size, used to be parked
    // happily and only surfaced when the stop fired.
    if (const RejectReason r = credit_.validateConditional(o, cfg_); r != RejectReason::None)
    {
      sink_(OrderRejected{o.id, o.symbol, r, o.accountId, o.clientOrderId});
      return;
    }
    committed = onStop(o);  // parked in the stop book -> committed (keep OCO link)
    return;
  }
  if (cfg_.maxOpenOrders > 0)
  {
    // Ingress DoS / risk gate: cap live resting orders per account. Once at
    // the cap the account must cancel before adding more.
    if (pub_.accountOrderCount(o.accountId) >= cfg_.maxOpenOrders)
    {
      sink_(OrderRejected{o.id, o.symbol, RejectReason::TooManyOpenOrders, o.accountId, o.clientOrderId});
      return;
    }
  }
  // Perp risk gate (reduce-only cap + position-limit check). The single
  // source of truth shared with the triggered-stop and modify paths -- see
  // perpRiskGate. Runs BEFORE validate so the fat-finger size check sees the
  // reduce-only-capped quantity, matching the pre-refactor behaviour. Spot:
  // a no-op.
  if (const RejectReason r = perpRiskGate(o); r != RejectReason::None)
  {
    sink_(OrderRejected{o.id, o.symbol, r, o.accountId, o.clientOrderId});
    return;
  }
  if (const RejectReason r = validate(o); r != RejectReason::None)
  {
    sink_(OrderRejected{o.id, o.symbol, r, o.accountId, o.clientOrderId});
    return;
  }
  if (!credit_.reserveFunds(o, cfg_, ledger_))
  {
    sink_(OrderRejected{o.id, o.symbol, credit_.creditReason(), o.accountId, o.clientOrderId});
    credit_.resetCreditReason();  // reset for the next order
    return;                       // pre-trade buying-power (ledger reservation or credit hook)
  }
  committed = true;  // past all reject gates: the order will match/rest, and any
                     // OCO resolution is now owned by processOco / forgetOrder.

  if (session_.auction())
  {
    // Pre-open / auction: accumulate without matching (a crossed book is
    // allowed). Market orders rest at the band edge so they always execute in
    // the uncross.
    Price restPx = o.price;
    if (o.type == OrderType::MARKET)
    {
      restPx = (o.side == Side::BUY) ? cfg_.maxPrice : cfg_.minPrice;
    }
    // Everything an order carries into continuous trading it also carries
    // into an auction. The uncross prices on displayed-plus-hidden depth, so
    // an iceberg's peak was never load-bearing for price discovery -- the one
    // thing the peak does is limit what the public feed sees, and that is
    // exactly what a book entry built without it gives away. A good-till-date
    // order that never reaches the expiry book never expires at all, in the auction
    // or after it.
    RestingOrder ro{o.id, o.accountId, restPx, o.quantity, o.side};
    ro.clientOrderId = o.clientOrderId;
    ro.reduceOnly = o.reduceOnly;
    ro.postOnly = o.postOnly;
    ro.lastLook = o.lastLook && cfg_.lastLookWindowNs.count() > 0;
    if (o.visibleQuantity.raw() > 0 && o.visibleQuantity < o.quantity)
    {
      ro.peak = o.visibleQuantity;
      ro.leaves = o.visibleQuantity;
      ro.hidden = o.quantity - o.visibleQuantity;
    }
    book_.addResting(o.side, ro);
    trackResting(o.id, o.accountId, o.stp);
    if (o.tif == TimeInForce::GTD && static_cast<bool>(o.expiryNs))
    {
      expiry_.set(o.id, o.expiryNs);
    }
    if (o.peg != PegRef::None)
    {
      pegs_.set(o.id, PegBook::Peg{o.side, o.peg, o.pegOffsetRaw});
    }
    // T059: pre-open accumulation never matches, so this order has filled
    // nothing yet -- cumQty is always 0.
    sink_(OrderAccepted{o.id, o.symbol, o.side, restPx, o.quantity, true, ro.leaves, o.accountId,
                        o.clientOrderId, Quantity{}});
    return;
  }

  // LULD band, captured from the pre-trade reference (matching below moves the
  // last price). Absent before the first trade -- documented pre-first-trade
  // window where no band exists yet.
  int64_t luldLo = 0, luldHi = 0;
  const bool luldOn = luldBand(luldLo, luldHi);

  if (luldOn && o.type != OrderType::MARKET)
  {
    // A limit order priced outside the band is rejected pre-trade and trips the
    // pause -- it never executes or rests out of band.
    if ((o.side == Side::BUY && luldHi < o.price.raw()) ||
        (o.side == Side::SELL && o.price.raw() < luldLo))
    {
      releaseReservation(o.id);
      sink_(OrderRejected{o.id, o.symbol, RejectReason::LuldBreach, o.accountId, o.clientOrderId});
      tripLuldHalt();
      return;
    }
  }

  const MatchOutcome out =
      matcher_.cross(o, book_, [this]()
                     { return ++tradeSeq_; }, emit_);
  stampFreshHolds();

  if (out.reject != RejectReason::None)
  {
    releaseReservation(o.id);  // post-only-would-cross / FOK-unfulfillable: free the reserve
    sink_(OrderRejected{o.id, o.symbol, out.reject, o.accountId, o.clientOrderId});
    return;
  }
  if (out.residualRests)
  {
    RestingOrder ro{o.id, o.accountId, o.price, out.leaves, o.side};
    ro.clientOrderId = o.clientOrderId;
    ro.lastLook = o.lastLook && cfg_.lastLookWindowNs.count() > 0;  // window 0 = feature off
    ro.reduceOnly = o.reduceOnly;                                   // carried so a later modify preserves it
    ro.postOnly = o.postOnly;                                       // same reason
    // T059: this order may have partially filled itself (as aggressor)
    // before its residual rests -- out.filled is that fill. Stamped onto the
    // RestingOrder now so a later report (cancel, exec, modify) on this
    // order carries the real running total instead of starting over at 0.
    ro.cumQty = out.filled;
    if (o.visibleQuantity.raw() > 0 && o.visibleQuantity < out.leaves)
    {
      ro.peak = o.visibleQuantity;    // iceberg: show a peak, hide the rest
      ro.leaves = o.visibleQuantity;  // displayed
      ro.hidden = out.leaves - o.visibleQuantity;
    }
    book_.addResting(o.side, ro);
    trackResting(o.id, o.accountId, o.stp);
    if (o.tif == TimeInForce::GTD && static_cast<bool>(o.expiryNs))
    {
      expiry_.set(o.id, o.expiryNs);
    }
    if (o.peg != PegRef::None)
    {
      pegs_.set(o.id, PegBook::Peg{o.side, o.peg, o.pegOffsetRaw});
    }
    // Public feed shows only the displayed peak (ro.leaves); the hidden iceberg
    // reserve (out.leaves - ro.leaves) is not leaked. Non-iceberg: they match.
    sink_(OrderAccepted{o.id, o.symbol, o.side, o.price, out.leaves, true, ro.leaves, o.accountId,
                        o.clientOrderId, out.filled});
  }
  else if (out.residualCanceled)
  {
    // The unfilled residual (IOC/FOK/MARKET/STP-newest) never rests, so its
    // buying-power reservation must be released here -- this raw-sink path does
    // not run the emit_ wrapper's release-on-cancel. Held slices stay
    // reserved: their accept still has to settle (reject releases later).
    releaseReservationExceptHeld(o.id);
    sink_(OrderCanceled{o.id, o.symbol, out.residualCancelReason, o.accountId, o.clientOrderId,
                        out.leaves, out.filled});
  }

  processTriggers();  // this order's trades may have crossed resting stops

  // Post-trade LULD: a MARKET order (no limit to gate it pre-trade) can sweep
  // the book and print outside the band, and a stop cascade in processTriggers
  // can too. The breaching print stands, but it trips the volatility pause so
  // subsequent trading halts -- the exchange-style volatility interruption. A
  // limit order cannot breach here: it never trades through its own in-band
  // limit, so this condition is naturally false for it.
  if (luldOn && hasLast_ && (lastPrice_.raw() > luldHi || lastPrice_.raw() < luldLo))
  {
    tripLuldHalt();
  }
}

// LULD band [loRaw, hiRaw] around the current last price, computed in 128-bit
// to avoid int64 overflow on a high-priced instrument and clamped so the upper
// bound cannot overflow. Returns false when LULD is off or there is no last
// price yet (no reference -> no band).
template <class Book>
bool MatchingEngine<Book>::luldBand(int64_t& loRaw, int64_t& hiRaw) const
{
  if (cfg_.luldBps <= 0 || !hasLast_)
  {
    return false;
  }
  const __int128 prod =
      static_cast<__int128>(lastPrice_.raw()) * static_cast<__int128>(cfg_.luldBps) / 10000;
  const int64_t maxBand = INT64_MAX - lastPrice_.raw();
  const int64_t band = prod > static_cast<__int128>(maxBand) ? maxBand : static_cast<int64_t>(prod);
  loRaw = lastPrice_.raw() - band;
  hiRaw = lastPrice_.raw() + band;
  return true;
}

template <class Book>
void MatchingEngine<Book>::tripLuldHalt()
{
  applySession(engine::SessionEvent::LuldBreach, now_ + cfg_.luldHaltNs);
}

}  // namespace flox::venue
