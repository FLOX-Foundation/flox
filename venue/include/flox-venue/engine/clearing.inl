/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

// MatchingEngine<Book>: the engine's side of linear-perp clearing.
//
// The clearing itself -- positions, funding, liquidation, ADL -- is
// engine::Clearing (engine/clearing.h), a plain class the engine owns. What
// is left in this fragment is the seam: the published methods, which stay on
// the engine and forward; the order-reservation moves clearing hands back
// (consumeOrderIM / releaseOrderIM read reserve_); and the perp settlement
// path, which is a clearing call plus the fee charge.
//
// Included only from flox-venue/matching_engine.h, which declares every member
// defined here. Including it directly gives a fragment with no class to attach
// to, so the include is refused rather than left to fail on the first method.
#ifndef FLOX_VENUE_MATCHING_ENGINE_INL
#error "flox-venue/engine/clearing.inl is a fragment of flox-venue/matching_engine.h; include that instead"
#endif

namespace flox::venue
{

// Perp position queries (signed contracts; average entry).
template <class Book>
int64_t MatchingEngine<Book>::positionQty(uint64_t account) const
{
  return clearing_.positionQty(account);
}

template <class Book>
Price MatchingEngine<Book>::positionEntry(uint64_t account) const
{
  return clearing_.positionEntry(account);
}

// Open interest: the long side of the perp positions the engine tracks for
// this symbol.
template <class Book>
Quantity MatchingEngine<Book>::openInterest() const
{
  return clearing_.openInterest();
}

// Next funding boundary, in sequencer time. The engine supplies the clock;
// the calendar is clearing's.
template <class Book>
SeqNanos MatchingEngine<Book>::nextFundingNs() const noexcept
{
  return clearing_.nextFundingNs(now_);
}

// The live funding interval: the operator-set schedule when there is one,
// otherwise the startup config value it falls back to.
template <class Book>
int64_t MatchingEngine<Book>::fundingIntervalNs() const noexcept
{
  return clearing_.fundingIntervalNs();
}

// Last funding rate the engine applied, at kFundingRateScale.
template <class Book>
int64_t MatchingEngine<Book>::fundingRateRaw() const noexcept
{
  return clearing_.fundingRateRaw();
}

// Operator-set funding calendar. Sequenced as SetFundingSchedule in
// production (journaled, replayed, checkpointed); this direct setter is the
// pre-start wiring / recovery path, like setStpGroup.
template <class Book>
void MatchingEngine<Book>::setFundingSchedule(DurationNs intervalNs, SeqNanos nextFundingNs)
{
  clearing_.setFundingSchedule(intervalNs, nextFundingNs);
  // The calendar is a published field, so a change is news -- but only for an
  // instrument the engine has a mark for. Publishing before the first SetMark
  // would break the feed's standing promise that an unmarked instrument gets
  // no DerivativesUpdate at all (the first mark carries the new schedule
  // anyway).
  if (hasMark_)
  {
    publishDerivatives(markPrice_);
  }
}

// Apply a funding payment (perps). The transfers, the schedule step and the
// liquidation sweep they can trigger are clearing's; the feed update is the
// engine's, and it goes out on both paths exactly as it always did.
template <class Book>
void MatchingEngine<Book>::applyFunding(double rate, Price mark)
{
  clearing_.applyFunding(rate, mark, now_);
  publishDerivatives(mark);
}

// External mark-price update (derivatives). Drives mark-referenced stops and
// maintenance-margin liquidations.
template <class Book>
void MatchingEngine<Book>::setMarkPrice(Price mark)
{
  markPrice_ = mark;
  hasMark_ = true;
  processTriggers();
  clearing_.checkLiquidations(mark);
  // After the consequences, so the published open interest matches the state
  // the mark left behind (a liquidation the mark caused has already closed
  // its position).
  publishDerivatives(mark);
}

// Unrealized PnL of an account's perp position marked at `mark` (quote raw).
template <class Book>
Amount MatchingEngine<Book>::unrealizedPnlRaw(uint64_t account, Price mark) const
{
  return clearing_.unrealizedPnlRaw(account, mark);
}

template <class Book>
int64_t MatchingEngine<Book>::iabs64(int64_t v)
{
  return v < 0 ? -v : v;
}

// Move reserved IM for `qtyRaw` from the order reservation to position margin
// (stays reserved in the ledger; returns the amount). Called back from
// clearing on the opening leg of a perp fill: the reservation is the engine's
// state, not clearing's.
template <class Book>
Amount MatchingEngine<Book>::consumeOrderIM(OrderId orderId, int64_t qtyRaw)
{
  Reservation* r = credit_.find(orderId);
  if (r == nullptr)
  {
    return 0;
  }
  Amount im = imForRaw(qtyRaw, r->limitPriceRaw);
  if (im > r->reservedRaw)
  {
    im = r->reservedRaw;
  }
  r->reservedRaw -= im;
  return im;
}

// A reducing order's own reserved IM was not needed -> release it to available.
template <class Book>
void MatchingEngine<Book>::releaseOrderIM(OrderId orderId, int64_t qtyRaw, uint64_t acct)
{
  Reservation* r = credit_.find(orderId);
  if (r == nullptr)
  {
    return;
  }
  Amount im = imForRaw(qtyRaw, r->limitPriceRaw);
  if (im > r->reservedRaw)
  {
    im = r->reservedRaw;
  }
  r->reservedRaw -= im;
  if (im > 0)
  {
    ledger_->release(acct, cfg_.quoteAsset, im);
  }
}

// An operator correction. Not a trade: see the note on AdjustPosition for
// why no PnL is realized, no fee charged and no margin moved.
template <class Book>
void MatchingEngine<Book>::onAdjustPosition(const AdjustPosition& a)
{
  clearing_.adjustPosition(a);
}

template <class Book>
void MatchingEngine<Book>::settlePerp(const Trade& t)
{
  const bool takerBuys = (t.takerSide == Side::BUY);
  const OrderId buyerId = takerBuys ? t.takerId : t.makerId;
  const uint64_t buyerAcct = takerBuys ? t.takerAccount : t.makerAccount;
  const OrderId sellerId = takerBuys ? t.makerId : t.takerId;
  const uint64_t sellerAcct = takerBuys ? t.makerAccount : t.takerAccount;
  clearing_.updatePerpPosition(buyerAcct, buyerId, true, t.quantity.raw(), t.price.raw());
  clearing_.updatePerpPosition(sellerAcct, sellerId, false, t.quantity.raw(), t.price.raw());
  if (fees_.enabled())
  {
    fees_.settle(t, cfg_, now_.raw(), *ledger_, venueAccount_, sink_);
  }
}

// Close a position on someone else's decision. The engine sees one symbol;
// a portfolio-margin model sees the whole basket and is the only party that
// can judge an account solvent or not across instruments. Same settlement
// path as the engine's own sweep, so both produce identical events and the
// replay cannot tell them apart.
template <class Book>
void MatchingEngine<Book>::onForceClose(const ForceClosePosition& fc)
{
  if (ledger_ == nullptr)
  {
    sink_(OrderRejected{0, cfg_.id, RejectReason::NoLedgerBound, fc.accountId});
    return;
  }
  if (!cfg_.linearPerp || !hasMark_)
  {
    sink_(OrderRejected{0, cfg_.id, RejectReason::UnknownOrder, fc.accountId});
    return;
  }
  // Nothing open is a no-op, not an error -- forceClose returns on its own.
  clearing_.forceClose(fc.accountId, markPrice_);
}

}  // namespace flox::venue
