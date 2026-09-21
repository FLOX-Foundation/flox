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
  credit_ = std::move(c);
}

// Admission profile of one account. A default-constructed profile clears the
// entry (back to "everything permitted"), which keeps the table canonical
// for the checkpoint state hash.
template <class Book>
void MatchingEngine<Book>::setAdmissionProfile(uint64_t account, const AdmissionProfile& p)
{
  if (p.allowedTypes == 0 && p.allowedTif == 0 && p.deny == 0)
  {
    admission_.erase(account);
    return;
  }
  admission_[account] = p;
}

template <class Book>
const std::unordered_map<uint64_t, AdmissionProfile>& MatchingEngine<Book>::admissionProfiles() const noexcept
{
  return admission_;
}

// Orders refused because the sender was not entitled to send them. Non-zero
// means a counterparty is sending what it may not -- visible immediately
// rather than a week later as a position nobody can explain.
template <class Book>
uint64_t MatchingEngine<Book>::admissionRejects() const noexcept
{
  return admissionRejects_;
}

// Instrument conformance for a conditional order: the trigger is a price and
// the size is a size, so tick, band and lot apply exactly as they do to a
// resting limit. State and duplicate checks already ran before this point.
template <class Book>
RejectReason MatchingEngine<Book>::validateConditional(const NewOrder& o) const
{
  if (o.quantity.raw() <= 0)
  {
    return RejectReason::InvalidQuantity;
  }
  if (!cfg_.lotSize.isZero() && (o.quantity.raw() % cfg_.lotSize.raw()) != 0)
  {
    return RejectReason::InvalidQuantity;
  }
  const int64_t trig = o.triggerPrice.raw();
  if (trig > 0)
  {
    if (!cfg_.tickSize.isZero() && (trig % cfg_.tickSize.raw()) != 0)
    {
      return RejectReason::InvalidPrice;
    }
    if (!cfg_.minPrice.isZero() && trig < cfg_.minPrice.raw())
    {
      return RejectReason::InvalidPrice;
    }
    if (!cfg_.maxPrice.isZero() && trig > cfg_.maxPrice.raw())
    {
      return RejectReason::InvalidPrice;
    }
  }
  // A stop-LIMIT also carries the limit price it will rest at.
  if (o.type == OrderType::STOP_LIMIT || o.type == OrderType::TAKE_PROFIT_LIMIT)
  {
    if (!cfg_.tickSize.isZero() && (o.price.raw() % cfg_.tickSize.raw()) != 0)
    {
      return RejectReason::InvalidPrice;
    }
    if (!cfg_.minPrice.isZero() && o.price.raw() < cfg_.minPrice.raw())
    {
      return RejectReason::InvalidPrice;
    }
    if (!cfg_.maxPrice.isZero() && o.price.raw() > cfg_.maxPrice.raw())
    {
      return RejectReason::InvalidPrice;
    }
  }
  return RejectReason::None;
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
  if (delisted_)
  {
    return RejectReason::InstrumentDelisted;
  }
  if (closed_)
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
  if (!held_.empty())
  {
    // order: not observable -- a predicate scan that returns on the first
    // hold naming this id
    for (const auto& [hid, h] : held_)
    {
      (void)hid;
      if (h.maker == o.id || h.taker == o.id)
      {
        return RejectReason::DuplicateOrderId;
      }
    }
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
// which fails the build if a path stops calling it.
template <class Book>
RejectReason MatchingEngine<Book>::admissionGate(const NewOrder& o) const
{
  auto it = admission_.find(o.accountId);
  if (it == admission_.end())
  {
    return RejectReason::None;  // no profile: everything permitted
  }
  const AdmissionProfile& p = it->second;
  if (p.allowedTypes != 0 && (p.allowedTypes & (1u << static_cast<uint32_t>(o.type))) == 0)
  {
    return RejectReason::OrderTypeNotPermitted;
  }
  // Tested before the TIF list so the answer names the policy rather than the
  // field: "you may not leave an order resting" tells the counterparty what
  // to change, "this time in force is not allowed" leaves it guessing which
  // of the allowed ones is safe.
  //
  // Refused on admission, not killed on the way out: an order that could rest
  // must never be accepted from a counterparty that does not track resting
  // orders. POST_ONLY exists only to rest; GTC and GTD outlive the message
  // that carried them.
  //
  // A call auction rests EVERYTHING it admits -- there is no matching to be
  // immediate about, so an IOC accumulates like any other order and sits in
  // the book until the uncross. A counterparty that does not track resting
  // orders therefore cannot participate in one at all: its order would sit
  // there for the length of the auction while it believes the order either
  // filled or died on arrival, and at the uncross it can trade against that
  // counterparty's own other side.
  if ((p.deny & AdmissionDeny::DenyResting) != 0 &&
      (auctionMode_ || o.tif == TimeInForce::GTC || o.tif == TimeInForce::GTD ||
       o.tif == TimeInForce::POST_ONLY || o.postOnly))
  {
    return RejectReason::RestingNotPermitted;
  }
  if (p.allowedTif != 0 && (p.allowedTif & (1u << static_cast<uint32_t>(o.tif))) == 0)
  {
    return RejectReason::TimeInForceNotPermitted;
  }
  return RejectReason::None;
}

template <class Book>
bool MatchingEngine<Book>::admissionDenies(uint64_t account, uint8_t bit) const
{
  auto it = admission_.find(account);
  return it != admission_.end() && (it->second.deny & bit) != 0;
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
    const auto pit = positions_.find(o.accountId);
    const int64_t posQ = (pit == positions_.end()) ? 0 : pit->second.qtyRaw;
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
    const auto pit = positions_.find(o.accountId);
    const int64_t posQ = (pit == positions_.end()) ? 0 : pit->second.qtyRaw;
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
  auto it = byAccount_.find(account);
  if (it == byAccount_.end())
  {
    return 0;
  }
  int64_t sum = 0;
  // order: not observable -- int64 sum of the account's reduce-only leaves
  for (OrderId id : it->second)
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

// How much of one leg's prospective fill the perp risk limits still allow,
// measured against the position the account holds RIGHT NOW. `reason` names
// the limit that cut it (meaningful only when the result is below `want`).
template <class Book>
int64_t MatchingEngine<Book>::legFillLimit(uint64_t account, Side side, bool reduceOnly, int64_t want,
                                           CancelReason& reason, int64_t posDeltaRaw) const
{
  const auto pit = positions_.find(account);
  const int64_t posQ = ((pit == positions_.end()) ? 0 : pit->second.qtyRaw) + posDeltaRaw;
  int64_t allowed = want;
  if (reduceOnly)
  {
    // A reduce-only order may only close what is open on the other side. Its
    // reserved IM is 0 by construction, so any part of it that opened a
    // position would open it with NO margin at all.
    const int64_t reducible = (side == Side::BUY && posQ < 0)    ? -posQ
                              : (side == Side::SELL && posQ > 0) ? posQ
                                                                 : 0;
    if (reducible < allowed)
    {
      allowed = reducible;
      reason = CancelReason::ReduceOnlyNotReducing;
    }
  }
  if (!cfg_.maxPositionQty.isZero())
  {
    // Room left before the RESULTING position breaches the cap. Checking the
    // incoming order alone (the admission gate) lets several orders, each
    // under the cap, settle into a position past it.
    const int64_t room =
        (side == Side::BUY) ? cfg_.maxPositionQty.raw() - posQ : cfg_.maxPositionQty.raw() + posQ;
    if (room < allowed)
    {
      allowed = room;
      reason = CancelReason::PositionLimitExceeded;
    }
  }
  return allowed < 0 ? 0 : allowed;
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
  FillLimit out;
  out.qty = want;
  out.makerQty = want;
  out.takerQty = want;
  CancelReason makerReason = CancelReason::ReduceOnlyNotReducing;
  CancelReason takerReason = CancelReason::ReduceOnlyNotReducing;
  const int64_t makerAllowed = legFillLimit(makerAcct, makerSide, makerReduceOnly, want.raw(),
                                            makerReason, makerPosDeltaRaw);
  const int64_t takerAllowed = legFillLimit(takerAcct, takerSide, takerReduceOnly, want.raw(),
                                            takerReason, takerPosDeltaRaw);
  out.makerQty = Quantity::fromRaw(makerAllowed);
  out.takerQty = Quantity::fromRaw(takerAllowed);
  // The maker is the leg reported as blocked when both are: it is the one the
  // matcher can act on (pull it from the book) without killing an aggressor
  // that may still trade elsewhere.
  if (makerAllowed <= takerAllowed)
  {
    out.qty = out.makerQty;
    out.makerBlocked = makerAllowed <= 0;
    out.reason = makerReason;
  }
  else
  {
    out.qty = out.takerQty;
    out.takerBlocked = takerAllowed <= 0;
    out.reason = takerReason;
  }
  return out;
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
  if (const RejectReason r = admissionGate(o); r != RejectReason::None)
  {
    ++admissionRejects_;
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
    orderOco_[o.id] = o.ocoGroup;  // link before matching so a taker fill triggers OCO too
    ocoMembers_[o.ocoGroup].push_back(o.id);
  }
  // Any early exit before the order commits (parks as a stop, or passes every
  // gate and reaches matching/resting) must unlink it from its OCO group --
  // otherwise a rejected leg lingers in ocoMembers_ and later cancels a reused
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
        self->unlinkOco(id);
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
    if (const RejectReason r = validateConditional(o); r != RejectReason::None)
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
    auto it = byAccount_.find(o.accountId);
    if (it != byAccount_.end() && it->second.size() >= cfg_.maxOpenOrders)
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
  if (!reserveFunds(o))
  {
    sink_(OrderRejected{o.id, o.symbol, creditReason_, o.accountId, o.clientOrderId});
    creditReason_ = RejectReason::InsufficientFunds;  // reset for the next order
    return;                                           // pre-trade buying-power (ledger reservation or credit hook)
  }
  committed = true;  // past all reject gates: the order will match/rest, and any
                     // OCO resolution is now owned by processOco / forgetOrder.

  if (auctionMode_)
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
    sink_(OrderAccepted{o.id, o.symbol, o.side, restPx, o.quantity, true, ro.leaves, o.accountId, o.clientOrderId});
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
    sink_(OrderAccepted{o.id, o.symbol, o.side, o.price, out.leaves, true, ro.leaves, o.accountId, o.clientOrderId});
  }
  else if (out.residualCanceled)
  {
    // The unfilled residual (IOC/FOK/MARKET/STP-newest) never rests, so its
    // buying-power reservation must be released here -- this raw-sink path does
    // not run the emit_ wrapper's release-on-cancel. Held slices stay
    // reserved: their accept still has to settle (reject releases later).
    releaseReservationExceptHeld(o.id);
    sink_(OrderCanceled{o.id, o.symbol, out.residualCancelReason, o.accountId, o.clientOrderId});
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
  cfg_.halted = true;  // trip a timed volatility pause
  haltUntil_ = now_ + cfg_.luldHaltNs;
  publishStatus(TradingStatusReason::LuldBreach);
}

}  // namespace flox::venue
