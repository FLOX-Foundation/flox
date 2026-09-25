/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

// MatchingEngine<Book>: the engine's side of the last-look seam.
//
// The holds themselves -- their state, their decisions and their outcomes --
// live in engine::LastLook (engine/last_look.h), which is not a template.
// What is left here is what only the template can supply: the implementation
// of engine::LastLook::Host over this Book, the three engine-side helpers it
// leans on (the reference price, the perp re-check, the reservation release),
// and the delegates the rest of the engine calls.
//
// Included only from flox-venue/matching_engine.h, which declares every member
// defined here. Including it directly gives a fragment with no class to attach
// to, so the include is refused rather than left to fail on the first method.
#ifndef FLOX_VENUE_MATCHING_ENGINE_INL
#error "flox-venue/engine/last_look.inl is a fragment of flox-venue/matching_engine.h; include that instead"
#endif

namespace flox::venue
{

// ---- engine::LastLook::Host over this Book ------------------------------

template <class Book>
const RestingOrder* MatchingEngine<Book>::LastLookHost::findResting(OrderId id) const
{
  return e_.book_.find(id);
}

template <class Book>
std::optional<RestingOrder> MatchingEngine<Book>::LastLookHost::takeResting(OrderId id)
{
  return e_.book_.cancel(id);
}

// Answers whether the order is back on the book. A restore puts a quantity
// back that a hold had taken off it, and a book that refuses (its pool filled
// while the hold was open) would otherwise leave the owner an order the venue
// does not have -- LastLook cancels it instead.
template <class Book>
bool MatchingEngine<Book>::LastLookHost::reinsertTail(Side side, const RestingOrder& o)
{
  return e_.restOnBook(side, o) == RejectReason::None;
}

template <class Book>
void MatchingEngine<Book>::LastLookHost::publish(const OutboundEvent& ev)
{
  e_.sink_(ev);
}

template <class Book>
void MatchingEngine<Book>::LastLookHost::publishTracked(const OutboundEvent& ev)
{
  e_.emit_(ev);
}

template <class Book>
engine::LastLookConfig MatchingEngine<Book>::LastLookHost::lastLookConfig() const
{
  return engine::LastLookConfig{e_.cfg_.id, e_.cfg_.lastLookWindowNs,
                                e_.cfg_.lastLookToleranceRaw, e_.cfg_.lastLookAcceptOnTimeout};
}

template <class Book>
int64_t MatchingEngine<Book>::LastLookHost::referenceRaw() const
{
  return e_.referenceRaw();
}

template <class Book>
bool MatchingEngine<Book>::LastLookHost::holdStillAllowed(const Held& h) const
{
  return e_.holdStillAllowed(h);
}

template <class Book>
uint64_t MatchingEngine<Book>::LastLookHost::nextTradeSeq()
{
  return ++e_.tradeSeq_;
}

template <class Book>
void MatchingEngine<Book>::LastLookHost::releaseHeldLeg(OrderId id, Quantity qty)
{
  e_.releaseHeldLeg(id, qty);
}

template <class Book>
void MatchingEngine<Book>::LastLookHost::cleanupOrderIfDone(OrderId id)
{
  e_.cleanupOrderIfDone(id);
}

template <class Book>
void MatchingEngine<Book>::LastLookHost::rememberStp(OrderId id, STPMode stp)
{
  e_.stp_.track(id, stp);
}

template <class Book>
void MatchingEngine<Book>::LastLookHost::adoptRestingTaker(const Held& h)
{
  e_.trackResting(h.taker, h.takerAccount, e_.stpOf(h.taker));
  if (h.takerTif == TimeInForce::GTD && static_cast<bool>(h.takerExpiryNs))
  {
    e_.expiry_.set(h.taker, h.takerExpiryNs);
  }
}

// ---- what only the engine can answer ------------------------------------

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

// ---- delegates ----------------------------------------------------------

// Open last-look holds (approximate cross-thread gauge for the idle sweeper).
template <class Book>
uint64_t MatchingEngine<Book>::openHolds() const noexcept
{
  return lastLook_.openCount();
}

// Whether a hold with this id is currently open. Read-only, and -- unlike
// openHolds(), which reads an atomic mirror -- backed directly by the hold
// table (an unordered_map, not synchronized): call only from the thread the
// engine belongs to.
template <class Book>
bool MatchingEngine<Book>::hasHold(uint64_t heldId) const
{
  return lastLook_.has(heldId);
}

// Enumerate every open hold as fn(const Held&), in unspecified order. Lets
// an external component (e.g. one restoring its own hold set from a
// checkpoint) verify identity, not just count. Read-only; same
// thread-ownership rule as hasHold().
template <class Book>
template <class Fn>
void MatchingEngine<Book>::forEachHold(Fn&& fn) const
{
  lastLook_.forEach(std::forward<Fn>(fn));
}

// Pro-rata defensive-path counter (see Matcher::crossProRata): a resting
// lastLook maker met by a pro-rata allocation was skipped, not filled firm.
template <class Book>
uint64_t MatchingEngine<Book>::skippedLastLookProRata() const noexcept
{
  return matcher_.skippedLastLookProRata();
}

// Last-look accepts turned into rejects because the fill would have breached
// a perp risk limit by the time the maker answered (see LastLook::resolve).
template <class Book>
uint64_t MatchingEngine<Book>::riskRejectedHolds() const noexcept
{
  return lastLook_.riskRejected();
}

// Per-maker last-look conduct; see engine::LastLook::stats().
template <class Book>
std::unordered_map<uint64_t, LastLookStats> MatchingEngine<Book>::lastLookStats() const
{
  return lastLook_.stats();
}

// Holds refused by the venue's own tolerance rather than by the maker.
template <class Book>
uint64_t MatchingEngine<Book>::toleranceRejectedHolds() const noexcept
{
  return lastLook_.toleranceRejected();
}

template <class Book>
void MatchingEngine<Book>::createHeld(const RestingOrder& maker, Quantity fill,
                                      const NewOrder& taker, Quantity takerCumSoFar)
{
  lastLook_.create(lastLookHost_, maker, fill, taker, now_, takerCumSoFar);
}

template <class Book>
void MatchingEngine<Book>::stampFreshHolds()
{
  lastLook_.stampFresh(lastLookHost_);
}

template <class Book>
void MatchingEngine<Book>::rejectHoldsFor(OrderId id)
{
  lastLook_.rejectFor(lastLookHost_, id);
}

template <class Book>
void MatchingEngine<Book>::rejectHoldsForAccount(uint64_t account)
{
  lastLook_.rejectForAccount(lastLookHost_, account);
}

template <class Book>
void MatchingEngine<Book>::rejectAllHolds()
{
  lastLook_.rejectAll(lastLookHost_);
}

template <class Book>
void MatchingEngine<Book>::onLastLookDecision(const LastLookDecision& d)
{
  lastLook_.onDecision(lastLookHost_, d);
}

template <class Book>
void MatchingEngine<Book>::expireHolds()
{
  lastLook_.expire(lastLookHost_, now_);
}

}  // namespace flox::venue
