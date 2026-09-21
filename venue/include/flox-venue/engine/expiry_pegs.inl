/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

// MatchingEngine<Book>: GTD expiry, pegged orders and OCO.
//
// Included only from flox-venue/matching_engine.h, which declares every member
// defined here. Including it directly gives a fragment with no class to attach
// to, so the include is refused rather than left to fail on the first method.
#ifndef FLOX_VENUE_MATCHING_ENGINE_INL
#error "flox-venue/engine/expiry_pegs.inl is a fragment of flox-venue/matching_engine.h; include that instead"
#endif

namespace flox::venue
{

// GTD: cancel resting orders whose expiry time has passed (deterministic by
// sequencer-ts). Runs on every submit before the command is processed.
template <class Book>
void MatchingEngine<Book>::expireOrders()
{
  if (expiry_.empty())
  {
    return;
  }
  std::vector<OrderId> due;
  expiry_.collectDue(now_, due);  // which orders are due, and in what order
  for (OrderId id : due)
  {
    expiry_.erase(id);
    rejectHoldsFor(id);  // expiry removes the order: resolve holds first
    const uint64_t stopClOrd = stops_.clientOrderIdOf(id);
    if (auto ro = book_.cancel(id))  // still resting -> expire it
    {
      const uint64_t acct = ownerOf(id);
      releaseReservation(id);
      forgetOrder(id);
      sink_(OrderCanceled{id, cfg_.id, CancelReason::Expired, acct, ro->clientOrderId});
    }
    else if (stops_.cancel(id))  // never triggered -> expire the conditional
    {
      const uint64_t acct = ownerOf(id);
      unlinkOco(id);
      forgetOrder(id);
      sink_(OrderCanceled{id, cfg_.id, CancelReason::Expired, acct, stopClOrd});
    }
  }
}

// Peg price for `side` tracking `ref` (+ signed offset), tick-aligned, clamped
// to not cross the opposite touch and to stay inside the price band.
template <class Book>
int64_t MatchingEngine<Book>::pegTargetRaw(Side side, PegRef ref, int64_t offsetRaw) const
{
  const auto bb = book_.bestBid();
  const auto ba = book_.bestAsk();
  PegBook::Market m;
  m.hasBid = bb.has_value();
  m.bidRaw = bb ? bb->raw() : 0;
  m.hasAsk = ba.has_value();
  m.askRaw = ba ? ba->raw() : 0;
  m.lastRaw = hasLast_ ? lastPrice_.raw() : ((cfg_.minPrice.raw() + cfg_.maxPrice.raw()) / 2);
  m.tickRaw = cfg_.tickSize.raw();
  m.minPriceRaw = cfg_.minPrice.raw();
  m.maxPriceRaw = cfg_.maxPrice.raw();
  return PegBook::targetRaw(side, ref, offsetRaw, m);
}

// Re-price pegged orders to track the book at each submit boundary. Repricing
// re-queues the order (time priority resets) and, for real-money settlement,
// re-reserves buying power at the new price (cancels the peg if unaffordable).
template <class Book>
void MatchingEngine<Book>::repeg()
{
  if (pegs_.empty())
  {
    return;
  }
  std::vector<OrderId> ids;
  pegs_.sortedIds(ids);  // which pegs are repriced, and in what order
  for (OrderId id : ids)
  {
    const PegBook::Peg* spec = pegs_.find(id);
    if (spec == nullptr)
    {
      continue;
    }
    const PegBook::Peg pg = *spec;
    // A peg reprice releases and re-reserves buying power at the new price;
    // an open hold against the old price/reservation would settle against a
    // reservation that no longer covers it -- resolve holds first.
    rejectHoldsFor(id);
    // Remove the order from the book BEFORE computing its peg target: otherwise
    // pegTargetRaw reads best-bid/ask/mid INCLUDING this order's own resting
    // quantity, so a peg that is the touch references itself and ratchets one
    // tick toward the opposite touch on every submit (creep + priority churn +
    // OrderModified spam) even when the real market never moved. Cancelling
    // first mirrors the creation path, where the order isn't resting yet.
    auto ro = book_.cancel(id);
    if (!ro)
    {
      pegs_.erase(id);
      continue;  // already filled / gone
    }
    const int64_t target = pegTargetRaw(pg.side, pg.ref, pg.offsetRaw);
    if (ro->price.raw() == target)
    {
      book_.addResting(ro->side, *ro);  // unchanged -> put it back
      continue;
    }
    // The reprice is re-funded whether or not a ledger is bound: with no
    // ledger reserveFunds still consults the setCreditCheck hook, and
    // skipping the whole block here was the one path where a repriced peg
    // escaped a check that submit and stop-trigger both apply.
    {
      releaseReservation(id);  // no-op without a ledger
      NewOrder synth;
      synth.id = id;
      synth.symbol = cfg_.id;
      synth.side = ro->side;
      synth.type = OrderType::LIMIT;
      synth.price = Price::fromRaw(target);
      synth.quantity = Quantity::fromRaw(ro->leaves.raw() + ro->hidden.raw());
      synth.accountId = ro->accountId;
      if (!reserveFunds(synth))  // cannot fund the reprice -> drop the peg
      {
        forgetOrder(id);
        pegs_.erase(id);
        sink_(OrderCanceled{id, cfg_.id, CancelReason::UserRequested, ro->accountId, ro->clientOrderId});
        continue;
      }
    }
    RestingOrder nr = *ro;
    nr.price = Price::fromRaw(target);
    book_.addResting(nr.side, nr);
    sink_(OrderModified{id, cfg_.id, Price::fromRaw(target), nr.leaves, false, nr.accountId, nr.clientOrderId});
  }
}

// OCO: after matching, cancel the losing siblings of every group that had a
// fill this submit. The filled ("winner") order is left alone.
template <class Book>
void MatchingEngine<Book>::processOco()
{
  if (ocoPending_.empty())
  {
    return;
  }
  for (const auto& [group, winner] : ocoPending_)
  {
    auto git = ocoMembers_.find(group);
    if (git == ocoMembers_.end())
    {
      continue;  // already resolved this submit
    }
    std::vector<OrderId> members = git->second;  // copy: cancel mutates maps
    // Deterministic sibling order by id: group membership is a SET (the
    // insertion order is not state -- a checkpoint restore rebuilds it in
    // canonical book order), so the cancel/event order must not depend on it.
    std::sort(members.begin(), members.end());
    ocoMembers_.erase(git);
    for (OrderId id : members)
    {
      orderOco_.erase(id);
      if (id != winner)
      {
        cancelOcoSibling(id);
      }
    }
  }
  ocoPending_.clear();
}

template <class Book>
void MatchingEngine<Book>::cancelOcoSibling(OrderId id)
{
  rejectHoldsFor(id);  // the sibling leaves for good: resolve its holds first
  const uint64_t restingAcct = ownerOf(id);
  const uint64_t stopAcct = stops_.accountOf(id);
  const uint64_t stopClOrd = stops_.clientOrderIdOf(id);
  if (auto ro = book_.cancel(id))
  {
    releaseReservation(id);
    forgetOrder(id);
    sink_(OrderCanceled{id, cfg_.id, CancelReason::OcoTriggered, restingAcct,
                        ro->clientOrderId});
  }
  else if (stops_.cancel(id))
  {
    sink_(OrderCanceled{id, cfg_.id, CancelReason::OcoTriggered, stopAcct, stopClOrd});
  }
}

}  // namespace flox::venue
