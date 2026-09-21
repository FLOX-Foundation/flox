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
  mmpCfg_[account] = MmpCfg{qtyLimit, windowNs};
}

template <class Book>
void MatchingEngine<Book>::onQuote(const Quote& q)
{
  if (q.symbol != cfg_.id)
  {
    return;
  }
  if (admissionDenies(q.accountId, AdmissionDeny::DenyQuote))
  {
    ++admissionRejects_;
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
  if (clOrdIdDuplicate(q.accountId, q.clientOrderId))
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
    sink_(OrderCanceled{q.bidId, cfg_.id, CancelReason::UserRequested, acct,
                        ro->clientOrderId});
  }
  if (auto ro = book_.cancel(q.askId))
  {
    const uint64_t acct = ownerOf(q.askId);
    releaseReservation(q.askId);
    forgetOrder(q.askId);
    sink_(OrderCanceled{q.askId, cfg_.id, CancelReason::UserRequested, acct,
                        ro->clientOrderId});
  }
  if (q.bidQty.raw() > 0)
  {
    NewOrder b;
    b.id = q.bidId;
    b.symbol = cfg_.id;
    b.side = Side::BUY;
    b.type = OrderType::LIMIT;
    b.price = q.bidPrice;
    b.quantity = q.bidQty;
    b.accountId = q.accountId;
    b.stp = q.stp;
    b.lastLook = q.lastLook;
    b.postOnly = q.postOnly;
    b.reduceOnly = q.reduceOnly;
    b.tif = q.tif;
    b.visibleQuantity = q.visibleQuantity;
    b.expiryNs = q.expiryNs;
    // The name the submitter gave the QUOTE, not a per-leg id it never
    // chose -- both legs carry it so their reports can be told apart from
    // any other order and joined back to each other. The dedup slot for
    // this value was already consumed above, once, for the quote as a
    // whole -- skip it here.
    b.clientOrderId = q.clientOrderId;
    onNew(b, /*clOrdIdChecked=*/true);
  }
  if (q.askQty.raw() > 0)
  {
    NewOrder a;
    a.id = q.askId;
    a.symbol = cfg_.id;
    a.side = Side::SELL;
    a.type = OrderType::LIMIT;
    a.price = q.askPrice;
    a.quantity = q.askQty;
    a.accountId = q.accountId;
    a.stp = q.stp;
    a.lastLook = q.lastLook;
    a.postOnly = q.postOnly;
    a.reduceOnly = q.reduceOnly;
    a.tif = q.tif;
    a.visibleQuantity = q.visibleQuantity;
    a.expiryNs = q.expiryNs;
    a.clientOrderId = q.clientOrderId;
    onNew(a, /*clOrdIdChecked=*/true);
  }
}

template <class Book>
void MatchingEngine<Book>::onTradeObserved(const Trade& t)
{
  // Hot path: skip the per-trade hash lookups entirely unless MMP or OCO is
  // actually in use (the common case is neither).
  if (!mmpCfg_.empty())
  {
    mmpAdd(t.makerAccount, t.quantity);
    mmpAdd(t.takerAccount, t.quantity);
  }
  // OCO: a fill on either side wins its group; sibling cancellation is deferred
  // to after matching (processOco) so we never mutate the book mid-match.
  if (!orderOco_.empty())
  {
    if (auto it = orderOco_.find(t.makerId); it != orderOco_.end())
    {
      ocoPending_.emplace_back(it->second, t.makerId);
    }
    if (auto it = orderOco_.find(t.takerId); it != orderOco_.end())
    {
      ocoPending_.emplace_back(it->second, t.takerId);
    }
  }
}

template <class Book>
void MatchingEngine<Book>::mmpAdd(uint64_t account, Quantity qty)
{
  auto cfg = mmpCfg_.find(account);
  if (cfg == mmpCfg_.end())
  {
    return;
  }
  auto& w = mmpFills_[account];
  w.fills.emplace_back(now_, qty);
  w.sumRaw += qty.raw();
  while (!w.fills.empty() && w.fills.front().first <= now_ - cfg->second.windowNs)
  {
    w.sumRaw -= w.fills.front().second.raw();
    w.fills.pop_front();
  }
  if (w.sumRaw >= cfg->second.qtyLimit.raw())  // sum >= limit
  {
    bool queued = false;
    for (uint64_t a : mmpBreached_)
    {
      queued |= (a == account);
    }
    if (!queued)
    {
      mmpBreached_.push_back(account);
    }
  }
}

template <class Book>
void MatchingEngine<Book>::mmpEnforce()
{
  for (uint64_t acc : mmpBreached_)
  {
    cancelAllForAccount(acc);
    auto& w = mmpFills_[acc];  // re-arm: drop the window and its running sum
    w.fills.clear();
    w.sumRaw = 0;
    sink_(MmpTriggered{acc, cfg_.id});
  }
  mmpBreached_.clear();
}

}  // namespace flox::venue
