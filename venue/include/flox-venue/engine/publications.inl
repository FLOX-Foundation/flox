/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

// MatchingEngine<Book>: instrument-wide publications and per-account resting-order tracking.
//
// Included only from flox-venue/matching_engine.h, which declares every member
// defined here. Including it directly gives a fragment with no class to attach
// to, so the include is refused rather than left to fail on the first method.
#ifndef FLOX_VENUE_MATCHING_ENGINE_INL
#error "flox-venue/engine/publications.inl is a fragment of flox-venue/matching_engine.h; include that instead"
#endif

namespace flox::venue
{

// Firm-group STP: register an account under a firm/group id so self-trade
// prevention fires across all of a firm's accounts, not just the same account
// (group 0 removes the membership). Runtime mutations MUST arrive as the
// sequenced SetStpGroup command (journaled, snapshot-carried, hashed) --
// this direct setter is for pre-start() wiring and the recovery path.
template <class Book>
void MatchingEngine<Book>::setStpGroup(uint64_t account, uint64_t group)
{
  matcher_.setStpGroup(account, group);
}

// Emit the state the engine is now in, if it differs from the last one
// published. Every halt / pause / auction transition routes through here, so
// the feed carries transitions and only transitions: no duplicate on a
// re-halt of an already halted symbol, and nothing to infer downstream.
template <class Book>
void MatchingEngine<Book>::publishStatus(TradingStatusReason reason)
{
  const TradingStatus s = tradingStatus();
  // The deadline belongs to the timed pause and to nothing else. A state that
  // is not the pause publishes 0 even when a pause deadline is still stored
  // underneath it (a closed session or an auction phase over a paused
  // instrument) -- a subscriber must never be handed an expiry for a state
  // that does not expire.
  emitStatus(s, reason, s == TradingStatus::LuldPause ? haltUntil_.raw() : 0);
}

template <class Book>
void MatchingEngine<Book>::emitStatus(TradingStatus status, TradingStatusReason reason, int64_t untilNs)
{
  if (statusPublished_ && status == lastStatus_ && untilNs == lastStatusUntil_)
  {
    return;
  }
  statusPublished_ = true;
  lastStatus_ = status;
  lastStatusUntil_ = untilNs;
  sink_(TradingStatusChanged{cfg_.id, status, reason, untilNs});
}

// A settlement just happened, so the calendar moves on: one whole interval
// past the boundary that was settled, and further whole intervals if the
// settlement ran late enough that one step would still leave the boundary in
// the past (an operator catching up after an outage must not leave a stale
// "next funding" in the feed). Only an operator-set schedule moves -- with no
// schedule the published value is derived from `now` and moves by itself.
template <class Book>
void MatchingEngine<Book>::advanceFundingSchedule()
{
  if (fundingIntervalNs_.count() <= 0 || nextFundingNs_.raw() <= 0)
  {
    return;
  }
  nextFundingNs_ += fundingIntervalNs_;
  if (nextFundingNs_ <= now_)
  {
    const DurationNs behind = now_ - nextFundingNs_;
    nextFundingNs_ += DurationNs{(behind.count() / fundingIntervalNs_.count() + 1) *
                                 fundingIntervalNs_.count()};
  }
}

// Emit the derivatives layer the engine knows: the mark it was just given,
// the last funding rate it applied, the next funding boundary of the
// configured schedule and the live open interest.
template <class Book>
void MatchingEngine<Book>::publishDerivatives(Price mark)
{
  sink_(DerivativesUpdated{cfg_.id, mark, fundingRateRaw_, nextFundingNs(), openInterest()});
}

// Owner of a live tracked order (0 if unknown) -- read BEFORE forgetOrder so
// async cancel events can be routed to the owner's session.
template <class Book>
uint64_t MatchingEngine<Book>::ownerOf(OrderId id) const noexcept
{
  auto it = orderAccount_.find(id);
  return it == orderAccount_.end() ? 0 : it->second;
}

// Self-trade-prevention mode recorded for an order, or None if it asked for
// none. Reading it back is how a re-entering order keeps the control it was
// admitted with.
template <class Book>
STPMode MatchingEngine<Book>::stpOf(OrderId id) const
{
  auto it = orderStp_.find(id);
  return it == orderStp_.end() ? STPMode::None : it->second;
}

// Self-trade-prevention scope: the firm group if the account is in one, else
// the account itself. Read off the matcher's table so the two can never
// disagree about who counts as the same trader.
template <class Book>
uint64_t MatchingEngine<Book>::stpScope(uint64_t account) const
{
  const auto& groups = matcher_.stpGroups();
  if (groups.empty())
  {
    return account;
  }
  auto it = groups.find(account);
  return it == groups.end() ? account : it->second;
}

// Cancel one leg of a self-matching auction pair, with the same discipline
// every other engine-side cancel follows: free the reservation, drop the
// tracking, tell the owner.
template <class Book>
void MatchingEngine<Book>::cancelForStp(OrderId id, uint64_t account)
{
  const auto ro = book_.cancel(id);
  releaseReservation(id);
  forgetOrder(id);
  sink_(OrderCanceled{id, cfg_.id, CancelReason::SelfTradePrevention, account,
                      ro ? ro->clientOrderId : 0});
}

// Trim one leg by the overlapping quantity without printing. A leg trimmed
// to nothing is canceled outright, which is also what keeps the uncross loop
// making progress.
template <class Book>
void MatchingEngine<Book>::decrementForStp(OrderId id, Quantity by)
{
  const RestingOrder* r = book_.find(id);
  if (r == nullptr)
  {
    return;
  }
  const uint64_t acct = r->accountId;
  // Displayed peak plus hidden reserve: the auction decrement measures what
  // the order holds, exactly as the continuous one does.
  const int64_t fromRaw = r->leaves.raw() + r->hidden.raw();
  const int64_t toRaw = fromRaw - by.raw();
  if (toRaw <= 0)
  {
    cancelForStp(id, acct);
    return;
  }
  book_.reduceTotal(id, Quantity::fromRaw(toRaw));
  releaseReservationPro(id, fromRaw, toRaw);
}

// Every path that puts an order on the book comes through here, and the STP
// mode is a required argument on purpose: a new rest path cannot compile
// without saying what self-trade prevention the order carries.
template <class Book>
void MatchingEngine<Book>::trackResting(OrderId id, uint64_t account, STPMode stp)
{
  orderAccount_[id] = account;
  byAccount_[account].insert(id);
  if (stp != STPMode::None)
  {
    orderStp_[id] = stp;
  }
  else
  {
    orderStp_.erase(id);  // an id can be reused after the previous order left
  }
}

template <class Book>
void MatchingEngine<Book>::forgetOrder(OrderId id)
{
  auto it = orderAccount_.find(id);
  if (it == orderAccount_.end())
  {
    return;
  }
  auto ba = byAccount_.find(it->second);
  if (ba != byAccount_.end())
  {
    ba->second.erase(id);
  }
  orderAccount_.erase(it);
  expiry_.erase(id);
  unlinkOco(id);
  pegged_.erase(id);
  orderStp_.erase(id);
}

// Remove an order from its OCO group, keeping orderOco_ and ocoMembers_ in
// sync. The fill path (processOco) erases ocoMembers_ itself; EVERY other exit
// (cancel / expiry / MMP / halt / liquidation / reject) must route through
// here, or a departed leg lingers in ocoMembers_ and later cancels a reused
// OrderId when the surviving sibling resolves (and the group vector leaks).
// Free the reservation covering the quantity an order just lost. Any path
// that shrinks a resting order owes this: buying power held against size
// that no longer rests is the account's money, frozen for nothing.
template <class Book>
void MatchingEngine<Book>::releaseReservationPro(OrderId id, int64_t fromQtyRaw, int64_t toQtyRaw)
{
  if (ledger_ == nullptr || fromQtyRaw <= 0 || toQtyRaw >= fromQtyRaw)
  {
    return;
  }
  auto it = reserve_.find(id);
  if (it == reserve_.end())
  {
    return;
  }
  const Amount freed = static_cast<Amount>(static_cast<__int128>(it->second.reservedRaw) *
                                           (fromQtyRaw - toQtyRaw) / fromQtyRaw);
  if (freed <= 0)
  {
    return;
  }
  ledger_->release(it->second.account, it->second.asset, freed);
  it->second.reservedRaw -= freed;
}

template <class Book>
void MatchingEngine<Book>::unlinkOco(OrderId id)
{
  auto it = orderOco_.find(id);
  if (it == orderOco_.end())
  {
    return;
  }
  if (auto gm = ocoMembers_.find(it->second); gm != ocoMembers_.end())
  {
    auto& v = gm->second;
    v.erase(std::remove(v.begin(), v.end(), id), v.end());
    if (v.empty())
    {
      ocoMembers_.erase(gm);
    }
  }
  orderOco_.erase(it);
}

template <class Book>
void MatchingEngine<Book>::cancelAllForAccount(uint64_t account, CancelReason reason)
{
  // Resolve every hold touching this account FIRST (mass-cancel / MMP /
  // liquidation must not leave held fills behind): the account's own held
  // slices are restored and then canceled below; counterparty slices return
  // to the book. Resolving before the id snapshot also catches an order of
  // this account that a restore would re-create.
  rejectHoldsForAccount(account);
  auto it = byAccount_.find(account);
  if (it == byAccount_.end())
  {
    return;
  }
  // order: sorted on the next line, before a single cancel is published
  std::vector<OrderId> ids(it->second.begin(), it->second.end());  // copy: erased in loop
  std::sort(ids.begin(), ids.end());                               // deterministic cancel/event order (layout-independent)
  for (OrderId id : ids)
  {
    if (auto ro = book_.cancel(id))
    {
      releaseReservation(id);
      forgetOrder(id);
      sink_(OrderCanceled{id, cfg_.id, reason, account, ro->clientOrderId});
    }
  }
}

template <class Book>
void MatchingEngine<Book>::onMassCancel(const MassCancel& mc)
{
  if (mc.symbol != 0 && mc.symbol != cfg_.id)
  {
    return;
  }
  cancelAllForAccount(mc.accountId);
}

}  // namespace flox::venue
