/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

// MatchingEngine<Book>: last look -- holds, decisions and their outcomes.
//
// Included only from flox-venue/matching_engine.h, which declares every member
// defined here. Including it directly gives a fragment with no class to attach
// to, so the include is refused rather than left to fail on the first method.
#ifndef FLOX_VENUE_MATCHING_ENGINE_INL
#error "flox-venue/engine/last_look.inl is a fragment of flox-venue/matching_engine.h; include that instead"
#endif

namespace flox::venue
{

// Open last-look holds (approximate cross-thread gauge for the idle sweeper).
template <class Book>
uint64_t MatchingEngine<Book>::openHolds() const noexcept
{
  return heldOpen_.load(std::memory_order_relaxed);
}

// Whether a hold with this id is currently open. Read-only, and -- unlike
// openHolds(), which reads an atomic mirror -- backed directly by held_
// (an unordered_map, not synchronized): call only from the thread the
// engine belongs to.
template <class Book>
bool MatchingEngine<Book>::hasHold(uint64_t heldId) const
{
  return held_.contains(heldId);
}

// Enumerate every open hold as fn(const Held&), in unspecified order. Lets
// an external component (e.g. one restoring its own hold set from a
// checkpoint) verify identity, not just count. Read-only; same
// thread-ownership rule as hasHold().
template <class Book>
template <class Fn>
void MatchingEngine<Book>::forEachHold(Fn&& fn) const
{
  // order: not observable -- forEachHold reports an identity SET (a caller
  // restoring its own holds checks membership, not sequence)
  for (const auto& [hid, h] : held_)
  {
    (void)hid;
    fn(h);
  }
}

// Pro-rata defensive-path counter (see Matcher::crossProRata): a resting
// lastLook maker met by a pro-rata allocation was skipped, not filled firm.
template <class Book>
uint64_t MatchingEngine<Book>::skippedLastLookProRata() const noexcept
{
  return matcher_.skippedLastLookProRata();
}

// Last-look accepts turned into rejects because the fill would have breached
// a perp risk limit by the time the maker answered (see resolveHeld).
template <class Book>
uint64_t MatchingEngine<Book>::riskRejectedHolds() const noexcept
{
  return riskRejectedHolds_;
}

// Per-maker last-look conduct.
//
// The reject RATE on its own says nothing: a maker with a wide tolerance and
// a maker cherry-picking its fills can post the same number. What separates
// them is which way the price had moved when they refused. A maker applying a
// symmetric rule refuses roughly as often when the move favoured it as when
// it did not; one taking the free option refuses almost only when it was
// losing. That single split is what makes the behaviour visible without
// anyone having to see the maker's code.
template <class Book>
const std::unordered_map<uint64_t, LastLookStats>& MatchingEngine<Book>::lastLookStats() const noexcept
{
  return lastLookStats_;
}

// Holds refused by the venue's own tolerance rather than by the maker.
template <class Book>
uint64_t MatchingEngine<Book>::toleranceRejectedHolds() const noexcept
{
  return toleranceRejectedHolds_;
}

template <class Book>
void MatchingEngine<Book>::createHeld(const RestingOrder& maker, Quantity fill, const NewOrder& taker)
{
  const uint64_t id = ++heldSeq_;
  Held h{id,
         taker.id,
         taker.accountId,
         taker.side,
         maker.id,
         maker.accountId,
         maker.price,
         fill,
         now_ + cfg_.lastLookWindowNs,
         // Stamped once the matching pass finishes, not here. See
         // stampFreshHolds().
         0};
  // The taker is an aggressor now, but its residual may rest once the hold
  // resolves -- and a resting order's STP mode is what an auction reads.
  // Capture it here, where the mode is still in hand.
  if (taker.stp != STPMode::None)
  {
    stp_.track(taker.id, taker.stp);
  }
  h.takerTif = taker.tif;
  h.takerType = taker.type;
  h.takerPrice = taker.price;
  h.takerExpiryNs = taker.expiryNs;
  h.makerReduceOnly = maker.reduceOnly;
  h.makerClientOrderId = maker.clientOrderId;
  h.takerClientOrderId = taker.clientOrderId;
  h.takerReduceOnly = taker.reduceOnly;
  held_[id] = h;
  freshHolds_.push_back(id);
  heldOpen_.store(held_.size(), std::memory_order_relaxed);
  // Called BEFORE the matcher reserves the qty out of the book, so the
  // maker's post-hold displayed size is computed here the same way a normal
  // fill would (partial peak -> remainder shown; full peak -> iceberg refill).
  const Quantity displayAfter =
      (fill < maker.leaves) ? (maker.leaves - fill)
                            : ((maker.peak < maker.hidden) ? maker.peak : maker.hidden);
  sink_(FillHeld{id, cfg_.id, maker.id, taker.id, maker.price, fill, displayAfter,
                 maker.accountId, taker.accountId, h.takerSide, taker.clientOrderId});
  // NOTE: the maker stays tracked (orderAccount_/byAccount_) even when the
  // hold empties its displayed size and fillBest removes it from the book --
  // the id is still live (a reject restores it) and mass-cancel paths must
  // still find it.
}

// Release one leg's buying-power reservation for a held quantity that will NOT
// trade (last-look reject / timeout). Mirrors the per-fill decrement settleTrade
// would have applied, but returns the funds to `available` instead of spending
// them -- so both parties are made whole for a fill that never happened, and no
// reservation is stranded in `reserved` forever. Cleans up a fully-released,
// no-longer-resting order's reservation + tracking maps.
template <class Book>
void MatchingEngine<Book>::releaseHeldLeg(OrderId id, Quantity qty)
{
  if (ledger_ == nullptr)
  {
    return;
  }
  Reservation* r = credit_.find(id);
  if (r == nullptr)
  {
    return;
  }
  Amount rel = credit_.backingFor(*r, qty, cfg_);
  if (rel > r->reservedRaw)
  {
    rel = r->reservedRaw;
  }
  if (rel > 0)
  {
    ledger_->release(r->account, r->asset, rel);
  }
  r->reservedRaw -= rel;
  if (r->reservedRaw <= 0 && !book_.contains(id))
  {
    credit_.erase(id);
    forgetOrder(id);
  }
}

template <class Book>
void MatchingEngine<Book>::resolveHeld(typename std::unordered_map<uint64_t, Held>::iterator it, bool accept)
{
  const Held h = it->second;
  held_.erase(it);
  heldOpen_.store(held_.size(), std::memory_order_relaxed);
  // An accept settles a fill that was risk-checked when the hold was created,
  // possibly a whole window ago. Re-measure it against the position as it is
  // now: a perp fill that would open or flip a reduce-only leg (with no margin
  // behind it) or carry an account past the position cap must not print just
  // because the maker said yes late. The hold is rejected instead -- the
  // honest outcome, since the venue cannot part-accept a hold: liquidity is
  // restored to both legs exactly as a maker reject would.
  if (accept && !holdStillAllowed(h))
  {
    ++riskRejectedHolds_;
    accept = false;
  }
  // Symmetric price tolerance, applied by the venue on magnitude alone.
  const int64_t moveRaw = referenceMoveSinceHold(h);
  if (cfg_.lastLookToleranceRaw > 0)
  {
    const int64_t mag = moveRaw < 0 ? -moveRaw : moveRaw;
    if (mag > cfg_.lastLookToleranceRaw)
    {
      ++toleranceRejectedHolds_;
      accept = false;
    }
  }
  recordHoldOutcome(h, moveRaw, accept);
  if (accept)
  {
    emit_(Trade{++tradeSeq_, cfg_.id, h.price, h.qty, h.maker, h.taker, h.takerSide,
                h.makerAccount, h.takerAccount});
    const RestingOrder* mk = book_.find(h.maker);
    const Quantity makerLeaves = mk ? Quantity::fromRaw(mk->leaves.raw() + mk->hidden.raw()) : Quantity{};
    const Quantity makerDisp = mk ? mk->leaves : Quantity{};  // displayed peak for public feed
    emit_(OrderExecuted{h.maker, cfg_.id, h.qty, makerLeaves, false, makerLeaves.isZero(), h.price,
                        makerDisp, h.makerAccount, h.makerClientOrderId});
    emit_(OrderExecuted{h.taker, cfg_.id, h.qty, Quantity{}, true, false, h.price, Quantity{},
                        h.takerAccount, h.takerClientOrderId});
  }
  else
  {
    // Reject / timeout: the held qty does not trade, and liquidity must not
    // be destroyed. The maker's displayed qty returns to its price level (at
    // the TAIL, as-if re-entered -- see docs/venue/matching.md); its
    // reservation was never touched and keeps backing it. The taker residual
    // follows its TIF: GTC/GTD rests, IOC/FOK/MARKET is canceled with the
    // matching residual reason (that leg's buying power is released). The
    // cancel paths resolve holds BEFORE removing an order, so a leg that is
    // absent from the book here was fully held out of it -- never "gone".
    restoreMakerHeld(h);
    restoreTakerHeld(h);
    sink_(FillRejected{h.id, cfg_.id, h.taker, h.maker, h.price, h.qty, h.takerAccount,
                       h.makerAccount, h.takerClientOrderId});
  }
  // Whichever way the hold resolved: if this was the last hold on a leg and
  // that leg no longer rests, free its leftover reservation and tracking
  // (deferred from the emit_ wrapper while holds were open).
  cleanupOrderIfDone(h.taker);
  cleanupOrderIfDone(h.maker);
}

// Where the market is, for judging a held fill: the mid of the book when both
// sides are quoted, and the last trade only when they are not.
//
// The two are not interchangeable here. A trade prints half a spread off the
// mid, on whichever side the aggressor took, so consecutive prints move by
// the spread even in a market that has not moved at all. Over a hold window
// measured in milliseconds that is most of what the last price does, and a
// tolerance or a conduct statistic built on it is reading the spread.
//
// It also decides whether the statistic has any power. A maker prices off its
// own view of the market, not off this venue's tape; measuring its behaviour
// against a series it never looked at classifies its refusals at random. In
// this deployment that is the normal case rather than the exception -- the
// liquidity provider aggregates several venues and this one is a fraction of
// what it sees. The mid is the closest thing here to what it is actually
// looking at.
//
// Zero when neither is available, which is the only honest answer before
// anything has traded or been quoted: it means unmeasurable, not unmoved.
// The reference a hold is judged from has to describe the book as it stands
// FOR THE DURATION of the hold, which is not the book that existed the
// instant before it opened.
//
// Opening a hold reserves the maker's quantity out of the book, so the side
// the aggressor hit loses its touch and the mid steps away from the
// aggressor. Stamping before that and comparing after makes the hold's own
// mechanism look like a market move -- always in the same direction, since a
// buyer always removes an ask. In an example run with buy-only probe flow
// this put 1,204 holds in the adverse bucket against 556 favourable, on a
// market whose moves were symmetric by construction, and it flattened the
// conduct statistic it was feeding.
//
// So the stamp waits until the matching pass is over and the book has
// settled. Both ends of the comparison then describe the same book.
template <class Book>
void MatchingEngine<Book>::stampFreshHolds()
{
  if (freshHolds_.empty())
  {
    return;
  }
  const int64_t ref = referenceRaw();
  for (uint64_t id : freshHolds_)
  {
    auto it = held_.find(id);
    if (it != held_.end())
    {
      it->second.refAtHoldRaw = ref;
    }
  }
  freshHolds_.clear();
}

template <class Book>
int64_t MatchingEngine<Book>::referenceRaw() const
{
  const auto bb = book_.bestBid();
  const auto ba = book_.bestAsk();
  if (bb && ba)
  {
    return (bb->raw() + ba->raw()) / 2;
  }
  return hasLast_ ? lastPrice_.raw() : 0;
}

// How far the reference has moved since the hold was taken, signed so that a
// positive value means it moved AGAINST the maker: it sold and the price rose,
// or it bought and the price fell. Zero when there is no reference to compare
// against.
template <class Book>
int64_t MatchingEngine<Book>::referenceMoveSinceHold(const Held& h) const
{
  const int64_t nowRaw = referenceRaw();
  if (nowRaw == 0 || h.refAtHoldRaw == 0)
  {
    return 0;
  }
  const int64_t delta = nowRaw - h.refAtHoldRaw;
  // takerSide is the aggressor's. A taker buying leaves the maker short, so a
  // rising price hurts the maker.
  return h.takerSide == Side::BUY ? delta : -delta;
}

template <class Book>
void MatchingEngine<Book>::recordHoldOutcome(const Held& h, int64_t moveRaw, bool accepted)
{
  LastLookStats& st = lastLookStats_[h.makerAccount];
  ++st.held;
  if (accepted)
  {
    ++st.accepted;
  }
  else
  {
    ++st.rejected;
  }
  if (moveRaw > 0)
  {
    ++st.adverse;
    if (!accepted)
    {
      ++st.rejectedAdverse;
    }
  }
  else if (moveRaw < 0)
  {
    ++st.favourable;
    if (!accepted)
    {
      ++st.rejectedFavourable;
    }
  }
}

// Would settling this hold in full still pass the perp risk limits? Both legs
// are measured on the live position (see fillLimit); spot and ledgerless
// engines have no positions, so nothing constrains them.
template <class Book>
bool MatchingEngine<Book>::holdStillAllowed(const Held& h) const
{
  if (!cfg_.linearPerp || ledger_ == nullptr)
  {
    return true;
  }
  const Side makerSide = (h.takerSide == Side::BUY) ? Side::SELL : Side::BUY;
  CancelReason ignored = CancelReason::ReduceOnlyNotReducing;
  if (legFillLimit(h.makerAccount, makerSide, h.makerReduceOnly, h.qty.raw(), ignored) <
      h.qty.raw())
  {
    return false;
  }
  return legFillLimit(h.takerAccount, h.takerSide, h.takerReduceOnly, h.qty.raw(), ignored) >=
         h.qty.raw();
}

// Return a rejected hold's qty to the maker: back onto its price level at the
// tail (as-if re-entered). If the hold consumed the whole displayed size the
// order left the book -- rebuild it from the hold record.
template <class Book>
void MatchingEngine<Book>::restoreMakerHeld(const Held& h)
{
  if (auto ro = book_.cancel(h.maker); ro.has_value())
  {
    ro->leaves += h.qty;  // the returned slice was displayed when it was held
    book_.addResting(ro->side, *ro);
    sink_(OrderModified{h.maker, cfg_.id, ro->price, ro->leaves, false, h.makerAccount,
                        ro->clientOrderId});
  }
  else
  {
    const Side makerSide = (h.takerSide == Side::BUY) ? Side::SELL : Side::BUY;
    RestingOrder rebuilt{h.maker, h.makerAccount, h.price, h.qty, makerSide};
    rebuilt.clientOrderId = h.makerClientOrderId;
    rebuilt.lastLook = true;
    rebuilt.reduceOnly = h.makerReduceOnly;
    book_.addResting(makerSide, rebuilt);
    // Still tracked in orderAccount_/byAccount_: a fully-held maker is never
    // forgotten while its hold is open (see createHeld).
    sink_(OrderModified{h.maker, cfg_.id, h.price, h.qty, false, h.makerAccount,
                        h.makerClientOrderId});
  }
}

// Return a rejected hold's qty to the taker per its TIF.
template <class Book>
void MatchingEngine<Book>::restoreTakerHeld(const Held& h)
{
  const bool rests = h.takerType == OrderType::LIMIT &&
                     (h.takerTif == TimeInForce::GTC || h.takerTif == TimeInForce::GTD);
  if (rests)
  {
    // Reservation keeps backing the restored resting quantity.
    if (auto ro = book_.cancel(h.taker); ro.has_value())
    {
      ro->leaves += h.qty;  // combine with the already-resting remainder, tail requeue
      book_.addResting(ro->side, *ro);
      sink_(OrderModified{h.taker, cfg_.id, ro->price, ro->leaves, false, h.takerAccount,
                          ro->clientOrderId});
    }
    else
    {
      RestingOrder rebuilt{h.taker, h.takerAccount, h.takerPrice, h.qty, h.takerSide};
      rebuilt.clientOrderId = h.takerClientOrderId;
      book_.addResting(h.takerSide, rebuilt);
      trackResting(h.taker, h.takerAccount, stpOf(h.taker));
      if (h.takerTif == TimeInForce::GTD && static_cast<bool>(h.takerExpiryNs))
      {
        expiry_.set(h.taker, h.takerExpiryNs);
      }
      sink_(OrderAccepted{h.taker, cfg_.id, h.takerSide, h.takerPrice, h.qty, true, h.qty,
                          h.takerAccount, h.takerClientOrderId});
    }
    return;
  }
  // IOC / FOK / MARKET: the residual never rests -- release the taker's held
  // buying power and cancel it with the reason its TIF would have produced.
  releaseHeldLeg(h.taker, h.qty);
  const CancelReason reason = (h.takerType == OrderType::MARKET) ? CancelReason::MarketResidual
                              : (h.takerTif == TimeInForce::FOK)
                                  ? CancelReason::FillOrKillResidual
                                  : CancelReason::ImmediateOrCancelResidual;
  sink_(OrderCanceled{h.taker, cfg_.id, reason, h.takerAccount, h.takerClientOrderId});
}

// Deterministically resolve (reject) every open hold that references `id` as
// maker or taker. MUST run before any path that permanently removes the order
// or reshapes its reservation (cancel/modify/quote-replace/expiry/OCO/peg):
// a hold left behind would let a later accept settle with no backing
// reservation (unchecked debit -> conservation breach).
template <class Book>
void MatchingEngine<Book>::rejectHoldsFor(OrderId id)
{
  if (held_.empty())
  {
    return;
  }
  std::vector<uint64_t> due;
  // order: the due holds are id-sorted below, before any is resolved
  for (const auto& [hid, h] : held_)
  {
    if (h.maker == id || h.taker == id)
    {
      due.push_back(hid);
    }
  }
  std::sort(due.begin(), due.end());  // deterministic resolution/event order
  for (uint64_t hid : due)
  {
    if (auto it = held_.find(hid); it != held_.end())
    {
      resolveHeld(it, false);
    }
  }
}

// Account-scope variant for mass-cancel / MMP / liquidation.
template <class Book>
void MatchingEngine<Book>::rejectHoldsForAccount(uint64_t account)
{
  if (held_.empty())
  {
    return;
  }
  std::vector<uint64_t> due;
  // order: the due holds are id-sorted below, before any is resolved
  for (const auto& [hid, h] : held_)
  {
    if (h.makerAccount == account || h.takerAccount == account)
    {
      due.push_back(hid);
    }
  }
  std::sort(due.begin(), due.end());
  for (uint64_t hid : due)
  {
    if (auto it = held_.find(hid); it != held_.end())
    {
      resolveHeld(it, false);
    }
  }
}

template <class Book>
void MatchingEngine<Book>::rejectAllHolds()
{
  if (held_.empty())
  {
    return;
  }
  std::vector<uint64_t> due;
  due.reserve(held_.size());
  // order: the due holds are id-sorted below, before any is resolved
  for (const auto& [hid, h] : held_)
  {
    (void)h;
    due.push_back(hid);
  }
  std::sort(due.begin(), due.end());
  for (uint64_t hid : due)
  {
    if (auto it = held_.find(hid); it != held_.end())
    {
      resolveHeld(it, false);
    }
  }
}

template <class Book>
void MatchingEngine<Book>::onLastLookDecision(const LastLookDecision& d)
{
  auto it = held_.find(d.heldId);
  if (it == held_.end())
  {
    sink_(OrderRejected{d.heldId, cfg_.id, RejectReason::UnknownOrder, d.accountId});
    return;
  }
  // Ownership: only the maker whose quote is held may decide its fate.
  if (d.accountId != it->second.makerAccount)
  {
    sink_(OrderRejected{d.heldId, cfg_.id, RejectReason::NotOrderOwner, d.accountId});
    return;
  }
  resolveHeld(it, d.accept);
}

template <class Book>
void MatchingEngine<Book>::expireHolds()
{
  if (held_.empty())
  {
    return;
  }
  std::vector<uint64_t> due;
  // order: the due holds are id-sorted below -- a timeout-accept assigns
  // ++tradeSeq_, so the resolution order feeds the event stream
  for (const auto& [id, h] : held_)
  {
    if (now_ >= h.deadline)
    {
      due.push_back(id);
    }
  }
  // Deterministic order: a timeout-accept assigns ++tradeSeq_, so the resolution
  // order feeds the event stream -- must not depend on held_ (unordered_map) layout.
  std::sort(due.begin(), due.end());
  for (uint64_t id : due)
  {
    auto it = held_.find(id);
    if (it != held_.end())
    {
      resolveHeld(it, cfg_.lastLookAcceptOnTimeout);
    }
  }
}

}  // namespace flox::venue
