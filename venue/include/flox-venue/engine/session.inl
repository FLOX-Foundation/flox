/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

// MatchingEngine<Book>: trading status, halt, delisting, pre-open and auctions.
//
// The STATE and the transitions between states live in engine::Session
// (engine/session.h), which is not a template -- none of it touches the book.
// What is left here is the part that does: publishing a transition to the
// feed, pulling the resting book when a transition demands it, and the
// uncross itself.
//
// Included only from flox-venue/matching_engine.h, which declares every member
// defined here. Including it directly gives a fragment with no class to attach
// to, so the include is refused rather than left to fail on the first method.
#ifndef FLOX_VENUE_MATCHING_ENGINE_INL
#error "flox-venue/engine/session.inl is a fragment of flox-venue/matching_engine.h; include that instead"
#endif

namespace flox::venue
{

// Current trading state, derived from the engine's own session / halt /
// pause / auction flags -- the same value the transition events publish. The
// ranking (delisted over closed over auction over halt) is engine::Session's,
// stated once there; cfg_.halted is passed in because the flag is config the
// engine owns.
template <class Book>
TradingStatus MatchingEngine<Book>::tradingStatus() const noexcept
{
  return session_.status(cfg_.halted);
}

// Session state (see AdminAction::CloseSession / OpenSession).
template <class Book>
bool MatchingEngine<Book>::sessionClosed() const noexcept
{
  return session_.closed();
}

// Withdrawn from trading (see AdminAction::Delist / Relist).
template <class Book>
bool MatchingEngine<Book>::delisted() const noexcept
{
  return session_.delisted();
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
  emitStatus(s, reason, session_.publishedUntil(s));
}

template <class Book>
void MatchingEngine<Book>::emitStatus(TradingStatus status, TradingStatusReason reason, int64_t untilNs)
{
  if (!session_.publishes(status, untilNs))
  {
    return;
  }
  sink_(TradingStatusChanged{cfg_.id, status, reason, untilNs});
}

// One row of engine/session.h's transition table. The session moves its own
// flags and says what caused the move; the two things it cannot do for itself
// happen here -- the new state reaches the feed, and the book effect the row
// asks for runs after it, so a subscriber reads the cancels as a consequence
// of the transition rather than as an unexplained mass cancel.
//
// A refused row (Delist on a delisted instrument, the pause deadline not yet
// reached) publishes nothing and touches nothing: it is not a transition.
template <class Book>
void MatchingEngine<Book>::applySession(engine::SessionEvent e, SeqNanos deadline)
{
  const engine::SessionOutcome out = session_.apply(e, cfg_.halted, now_, deadline);
  if (!out.fired)
  {
    return;
  }
  publishStatus(out.reason);
  if (out.effect == engine::SessionEffect::CancelBookAfter)
  {
    cancelEntireBook(CancelReason::VenueHalt);
  }
}

// Control-plane hook (and the AdminCmd Halt/Resume path). The Resume row
// clears any timed pause deadline with the halt, so the state the feed
// publishes is the state the engine is actually in.
template <class Book>
void MatchingEngine<Book>::setHalted(bool halted)
{
  applySession(halted ? engine::SessionEvent::Halt : engine::SessionEvent::Resume);
}

// Operator emergency stop: halt the symbol (reject new orders) AND pull the
// entire resting book -- every live limit order and pending conditional --
// releasing reservations. Used on a fat-finger event or system anomaly. The
// row clears the deadline: an operator halt has no deadline, unlike the LULD
// pause.
template <class Book>
void MatchingEngine<Book>::haltAndCancelAll()
{
  applySession(engine::SessionEvent::HaltAndCancelAll);
}

// Pull every resting and pending order. Shared by the emergency halt and by
// delisting, because "nothing survives" has exactly one correct
// implementation and a second copy is how the two drift apart.
template <class Book>
void MatchingEngine<Book>::cancelEntireBook(CancelReason reason)
{
  // Resolve every open hold first: restored quantity lands back on the book
  // and is then swept by the loop below, so nothing survives.
  rejectAllHolds();
  for (OrderId id : pub_.cancelOrderAll())
  {
    if (auto ro = book_.cancel(id))
    {
      const uint64_t acct = ownerOf(id);
      releaseReservation(id);
      forgetOrder(id);
      pub_.publishCanceled(id, reason, acct, ro->clientOrderId, ro->leaves + ro->hidden,
                           ro->cumQty);
    }
  }
  for (OrderId id : stops_.ids())
  {
    const uint64_t acct = stops_.accountOf(id);
    const uint64_t clOrd = stops_.clientOrderIdOf(id);
    // Captured before stops_.cancel() below erases the entry (T058).
    const Quantity stopQty = stops_.quantityOf(id);
    if (stops_.cancel(id))
    {
      releaseReservation(id);
      sink_(OrderCanceled{id, cfg_.id, reason, acct, clOrd, stopQty, Quantity{}});
    }
  }
}

// Close the trading session: new orders are rejected with MarketClosed, and
// the resting book STANDS (a close is not a cancel-all -- an operator who
// wants the book pulled has HaltAndCancelAll). The halt / auction state
// underneath is deliberately untouched, so a close during a halt reopens
// still halted: the session boundary must not silently clear an exception
// state an operator raised for a reason.
//
// The engine owns the STATE, never the calendar: nothing here fires on a
// clock. The schedule that decides when to send CloseSession / OpenSession is
// the operator's / control plane's (docs/venue/runtime.md).
template <class Book>
void MatchingEngine<Book>::closeSession()
{
  applySession(engine::SessionEvent::CloseSession);
}

template <class Book>
void MatchingEngine<Book>::openSession()
{
  applySession(engine::SessionEvent::OpenSession);
}

// Withdraw the instrument from trading. Unlike a halt or a closed session,
// this carries no promise of a return, so leaving orders resting would leave
// them waiting for an open that is not coming -- the book is pulled on the
// way out, through the same path an emergency halt uses.
template <class Book>
void MatchingEngine<Book>::delist()
{
  // Guarded by the table's NotAlreadyDelisted: delisting a delisted
  // instrument is not a transition, so it publishes nothing and pulls
  // nothing (the book is already empty, and a second sweep would still emit).
  applySession(engine::SessionEvent::Delist);
}

template <class Book>
void MatchingEngine<Book>::relist()
{
  applySession(engine::SessionEvent::Relist);
}

// Pre-open: orders accumulate without matching (a crossed book is allowed).
template <class Book>
void MatchingEngine<Book>::beginPreOpen()
{
  applySession(engine::SessionEvent::BeginPreOpen);
}

// Dispatch a sequenced operator action (see AdminCmd). Routing admin actions
// through the command stream (not direct method calls) is what makes them
// survive journal replay / HA -- the auction uncross fills and the emergency
// cancel-all are reproduced at the same point relative to the orders.
template <class Book>
void MatchingEngine<Book>::onAdmin(AdminAction a)
{
  switch (a)
  {
    case AdminAction::BeginPreOpen:
      beginPreOpen();
      break;
    case AdminAction::OpenContinuous:
      openContinuous();
      break;
    case AdminAction::ResumeAuction:
      resumeWithAuction();
      break;
    case AdminAction::HaltAndCancelAll:
      haltAndCancelAll();
      break;
    case AdminAction::Halt:
      setHalted(true);
      break;
    case AdminAction::Resume:
      setHalted(false);
      break;
    case AdminAction::CloseSession:
      closeSession();
      break;
    case AdminAction::OpenSession:
      openSession();
      break;
    case AdminAction::Delist:
      delist();
      break;
    case AdminAction::Relist:
      relist();
      break;
  }
}

// Resume a halted symbol through a re-opening auction: clear the halt (stop
// rejecting) and enter pre-open accumulation. Orders build a (possibly crossed)
// book without matching until the operator calls openContinuous(), which
// uncrosses at the single volume-maximizing price and switches to continuous.
// This is how venues reopen after a halt -- never straight into continuous.
template <class Book>
void MatchingEngine<Book>::resumeWithAuction()
{
  applySession(engine::SessionEvent::ResumeAuction);
}

// Run the (opening / closing) uncross auction: match everything at the single
// volume-maximizing price, then resume continuous trading.
template <class Book>
void MatchingEngine<Book>::openContinuous()
{
  // The uncross is a state of its own for exactly as long as it runs: a
  // subscriber must be able to attribute the burst of fills to it rather than
  // to continuous trading that has not resumed yet. It runs BEFORE the flags
  // move, which is what the row's UncrossBefore effect records.
  emitStatus(TradingStatus::AuctionUncross, TradingStatusReason::Auction, 0);
  runAuction();
  applySession(engine::SessionEvent::OpenContinuous);
}

// Explicit uncross without changing session mode (closing auction, etc.).
template <class Book>
void MatchingEngine<Book>::runAuction()
{
  std::vector<std::pair<Price, Quantity>> bids;
  std::vector<std::pair<Price, Quantity>> asks;
  book_.levels(Side::BUY, bids);
  book_.levels(Side::SELL, asks);
  if (bids.empty() || asks.empty())
  {
    return;
  }
  // Candidate prices = every distinct order price. Pick the one maximizing
  // executable volume; tie-break on smallest demand/supply imbalance.
  std::vector<Price> cands;
  for (const auto& b : bids)
  {
    cands.push_back(b.first);
  }
  for (const auto& a : asks)
  {
    cands.push_back(a.first);
  }
  std::sort(cands.begin(), cands.end(), [](Price x, Price y)
            { return x.raw() < y.raw(); });
  cands.erase(std::unique(cands.begin(), cands.end(),
                          [](Price x, Price y)
                          { return x.raw() == y.raw(); }),
              cands.end());

  bool have = false;
  Price bestP{};
  Quantity bestExec{};
  __int128 bestImb = 0;
  for (Price p : cands)
  {
    Quantity dem{};
    for (const auto& b : bids)
    {
      if (!(b.first < p))
      {
        dem += b.second;  // bid price >= p
      }
    }
    Quantity sup{};
    for (const auto& a : asks)
    {
      if (!(p < a.first))
      {
        sup += a.second;  // ask price <= p
      }
    }
    const Quantity exec = (dem < sup) ? dem : sup;
    if (exec.raw() == 0)
    {
      continue;
    }
    __int128 imb = static_cast<__int128>(dem.raw()) - static_cast<__int128>(sup.raw());
    if (imb < 0)
    {
      imb = -imb;
    }
    if (!have || bestExec < exec || (exec.raw() == bestExec.raw() && imb < bestImb))
    {
      have = true;
      bestExec = exec;
      bestP = p;
      bestImb = imb;
    }
  }
  if (!have)
  {
    return;
  }

  // Uncross at bestP: repeatedly match best bid vs best ask, all at bestP.
  const Price P = bestP;
  while (true)
  {
    RestingOrder* bid = book_.peekBest(Side::BUY);
    RestingOrder* ask = book_.peekBest(Side::SELL);
    if (bid == nullptr || ask == nullptr || bid->price < P || P < ask->price)
    {
      break;
    }
    Quantity fill = (bid->leaves < ask->leaves) ? bid->leaves : ask->leaves;
    const OrderId bidId = bid->id;
    const OrderId askId = ask->id;
    const uint64_t bAcct = bid->accountId;
    const uint64_t aAcct = ask->accountId;
    // Read off the records now: the consume and cancel below can take either
    // leg out of the book before the reports are emitted.
    const uint64_t bClOrd = bid->clientOrderId;
    const uint64_t aClOrd = ask->clientOrderId;
    const Quantity bLeavesTotal = bid->leaves + bid->hidden;  // T058
    const Quantity aLeavesTotal = ask->leaves + ask->hidden;
    const Quantity bCum = bid->cumQty;
    const Quantity aCum = ask->cumQty;
    // Self-trade prevention. An auction has no aggressor -- both legs are
    // resting -- so the verdict is reached from the two recorded modes rather
    // than from an aggressor's (StpState::auctionVerdict); what is left here
    // is carrying it out against the book.
    if (stpScope(bAcct) == stpScope(aAcct))
    {
      const StpState::AuctionVerdict v =
          StpState::auctionVerdict(stpOf(bidId), stpOf(askId));
      if (v.engaged)
      {
        if (v.decrement)
        {
          // Trim both legs by the overlap; no print, and whatever is left of
          // the larger leg stays in the auction.
          decrementForStp(bidId, fill);
          decrementForStp(askId, fill);
          continue;
        }
        if (v.cancelBid)
        {
          cancelForStp(bidId, bAcct);
        }
        if (v.cancelAsk)
        {
          cancelForStp(askId, aAcct);
        }
        continue;  // this pair never prints
      }
    }

    // The uncross prints its own fills instead of going through the matcher,
    // so it applies the fill-time perp limits itself: an auction is a fill
    // moment like any other, and a reduce-only order must not flip a position
    // (with no margin) just because it was filled here.
    if (cfg_.linearPerp && ledger_ != nullptr)
    {
      const FillLimit lim = pairFillLimit(aAcct, Side::SELL, ask->reduceOnly, bAcct, Side::BUY,
                                          bid->reduceOnly, fill);
      if (lim.qty.isZero())
      {
        // Nothing this pair may trade: pull the blocked leg so the uncross
        // moves on to the next order at this price (and terminates).
        const OrderId blocked = lim.makerBlocked ? askId : bidId;
        const uint64_t blockedAcct = lim.makerBlocked ? aAcct : bAcct;
        const uint64_t blockedClOrd = lim.makerBlocked ? aClOrd : bClOrd;
        const Quantity blockedLeaves = lim.makerBlocked ? aLeavesTotal : bLeavesTotal;
        const Quantity blockedCum = lim.makerBlocked ? aCum : bCum;
        book_.cancel(blocked);
        releaseReservation(blocked);
        forgetOrder(blocked);
        sink_(OrderCanceled{blocked, cfg_.id, lim.reason, blockedAcct, blockedClOrd, blockedLeaves,
                            blockedCum});
        continue;
      }
      fill = lim.qty;
    }
    emit_(Trade{++tradeSeq_, cfg_.id, P, fill, askId, bidId, Side::BUY, aAcct, bAcct});
    book_.consumeById(bidId, fill);
    book_.consumeById(askId, fill);
    const RestingOrder* b2 = book_.find(bidId);
    const RestingOrder* a2 = book_.find(askId);
    const Quantity bl = b2 ? Quantity::fromRaw(b2->leaves.raw() + b2->hidden.raw()) : Quantity{};
    const Quantity al = a2 ? Quantity::fromRaw(a2->leaves.raw() + a2->hidden.raw()) : Quantity{};
    // Post-consume displayed peak (b2/a2 already refilled) for the public feed.
    const Quantity bDisp = b2 ? b2->leaves : Quantity{};
    const Quantity aDisp = a2 ? a2->leaves : Quantity{};
    emit_(OrderExecuted{bidId, cfg_.id, fill, bl, false, bl.isZero(), P, bDisp, bAcct, bClOrd});
    emit_(OrderExecuted{askId, cfg_.id, fill, al, false, al.isZero(), P, aDisp, aAcct, aClOrd});
  }
}

}  // namespace flox::venue
