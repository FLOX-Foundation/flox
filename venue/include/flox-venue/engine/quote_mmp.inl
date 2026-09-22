/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

// MatchingEngine<Book>: two-sided market-maker quotes and market-maker protection.
//
// Included only from flox-venue/matching_engine.h, which declares every member
// defined here. Including it directly gives a fragment with no class to attach
// to, so the include is refused rather than left to fail on the first method.
#ifndef FLOX_VENUE_MATCHING_ENGINE_INL
#error "flox-venue/engine/quote_mmp.inl is a fragment of flox-venue/matching_engine.h; include that instead"
#endif

namespace flox::venue
{

// Market-maker protection: if `qtyLimit` is filled for `account` within
// `windowNs`, all its resting orders are pulled.
template <class Book>
void MatchingEngine<Book>::setMmp(uint64_t account, Quantity qtyLimit, DurationNs windowNs)
{
  mmp_.configure(account, qtyLimit, windowNs);
}

template <class Book>
void MatchingEngine<Book>::onQuote(const Quote& q)
{
  if (q.symbol != cfg_.id)
  {
    return;
  }
  applyQuote(q, /*clOrdIdChecked=*/false);
}

// A market maker's whole set of levels on one symbol, replaced in one command.
//
// Every rung goes through applyQuote, which is the Quote path itself and not a
// copy of it: a ladder of K rungs does what K Quotes with the same parameters
// do, in the same order, down to the ids the legs are given and the events
// that leave. The loop runs the full block rather than only the live rungs,
// because the rungs past the live ones are quotes with no quantity on either
// side -- which is how a ladder that got shorter takes down the levels it
// stopped naming. On a level nothing rests at, such a rung cancels nothing
// and publishes nothing.
//
// The clientOrderId is deduplicated ONCE, for the ladder as a whole, for the
// reason a Quote registers it once for its two legs: a ladder is one
// submission naming 2K children, not K submissions, and checking it per rung
// would make the ladder refuse its own second level.
template <class Book>
void MatchingEngine<Book>::onQuoteLadder(const QuoteLadder& l)
{
  if (l.symbol != cfg_.id)
  {
    return;
  }
  if (clOrdIdDuplicate(l.accountId, l.clientOrderId))
  {
    sink_(OrderRejected{l.bidIdBase, l.symbol, RejectReason::DuplicateClientOrderId, l.accountId,
                        l.clientOrderId});
    return;
  }
  for (uint8_t i = 0; i < kQuoteLadderLevels; ++i)
  {
    applyQuote(engine::QuoteLadderLegs::at(l, i), /*clOrdIdChecked=*/true);
  }
}

// The quote path both commands run. `clOrdIdChecked` is true for a ladder's
// rungs only: the ladder registered the name once, above, before any rung was
// built.
template <class Book>
void MatchingEngine<Book>::applyQuote(const Quote& q, bool clOrdIdChecked)
{
  if (admissionDenies(q.accountId, AdmissionDeny::DenyQuote))
  {
    credit_.countAdmissionReject();
    sink_(OrderRejected{q.bidId, q.symbol, RejectReason::QuoteNotPermitted, q.accountId});
    return;
  }
  // A quote replaces the orders its two ids name, so each id is a cancel
  // command in disguise and answers to the same ownership rule. Refuse the
  // whole quote rather than half of it: a maker that gets one side replaced
  // and the other refused is quoting a book it did not ask for.
  if (ownershipRefused(q.bidId, q.accountId) || ownershipRefused(q.askId, q.accountId))
  {
    sink_(OrderRejected{q.bidId, q.symbol, RejectReason::NotOrderOwner, q.accountId});
    return;
  }
  // clientOrderId dedup for the quote AS A WHOLE, registered once, before
  // either leg is built: a quote is one submission naming two children, not
  // two submissions, so it consumes one dedup slot. Checking it per-leg
  // through onNew would make the quote refuse its own second leg -- the
  // first leg's insert would already be sitting in `cur` by the time the
  // second one asked. A genuine resend (this account replaying the same
  // clOrdId) is still refused, same as any other duplicate, and the prior
  // quote is left resting untouched.
  if (!clOrdIdChecked && clOrdIdDuplicate(q.accountId, q.clientOrderId))
  {
    sink_(OrderRejected{q.bidId, q.symbol, RejectReason::DuplicateClientOrderId, q.accountId,
                        q.clientOrderId});
    return;
  }
  rejectHoldsFor(q.bidId);  // a replaced quote may carry open holds
  rejectHoldsFor(q.askId);
  if (auto ro = book_.cancel(q.bidId))
  {
    const uint64_t acct = ownerOf(q.bidId);
    releaseReservation(q.bidId);
    forgetOrder(q.bidId);
    sink_(OrderCanceled{q.bidId, cfg_.id, CancelReason::UserRequested, acct, ro->clientOrderId,
                        ro->leaves + ro->hidden, ro->cumQty});
  }
  if (auto ro = book_.cancel(q.askId))
  {
    const uint64_t acct = ownerOf(q.askId);
    releaseReservation(q.askId);
    forgetOrder(q.askId);
    sink_(OrderCanceled{q.askId, cfg_.id, CancelReason::UserRequested, acct, ro->clientOrderId,
                        ro->leaves + ro->hidden, ro->cumQty});
  }
  // What the quote asks for, read off it once: the bid leg, then the ask
  // leg, each carrying every field the quote carried (engine::QuoteLegs). The
  // dedup slot was consumed above, for the quote as a whole, so neither leg
  // asks for one of its own.
  std::array<NewOrder, 2> legs;
  const uint8_t n = engine::QuoteLegs::build(q, cfg_.id, legs);
  for (uint8_t i = 0; i < n; ++i)
  {
    onNew(legs[i], /*clOrdIdChecked=*/true);
  }
}

template <class Book>
void MatchingEngine<Book>::onTradeObserved(const Trade& t)
{
  // Hot path: skip the per-trade hash lookups entirely unless MMP or OCO is
  // actually in use (the common case is neither).
  if (mmp_.armed())
  {
    mmpAdd(t.makerAccount, t.quantity);
    mmpAdd(t.takerAccount, t.quantity);
  }
  // OCO: a fill on either side wins its group; sibling cancellation is deferred
  // to after matching (processOco) so we never mutate the book mid-match.
  if (!oco_.empty())
  {
    oco_.noteFill(t.makerId, t.takerId);
  }
}

template <class Book>
void MatchingEngine<Book>::mmpAdd(uint64_t account, Quantity qty)
{
  mmp_.add(account, qty, now_);
}

template <class Book>
void MatchingEngine<Book>::mmpEnforce()
{
  for (uint64_t acc : mmp_.breached())
  {
    cancelAllForAccount(acc);
    mmp_.rearm(acc);
    sink_(MmpTriggered{acc, cfg_.id});
  }
  mmp_.clearBreached();
}

}  // namespace flox::venue
