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
// Included only from flox-venue/matching_engine.h, which declares every member
// defined here. Including it directly gives a fragment with no class to attach
// to, so the include is refused rather than left to fail on the first method.
#ifndef FLOX_VENUE_MATCHING_ENGINE_INL
#error "flox-venue/engine/session.inl is a fragment of flox-venue/matching_engine.h; include that instead"
#endif

namespace flox::venue
{

// Current trading state, derived from the engine's own session / halt /
// pause / auction flags -- the same value the transition events publish.
// Closed wins over everything: a closed session is the instrument's outermost
// state, and the halt or auction phase underneath it is preserved untouched
// so reopening returns to exactly the state the close interrupted.
template <class Book>
TradingStatus MatchingEngine<Book>::tradingStatus() const noexcept
{
  if (delisted_)
  {
    return TradingStatus::Delisted;
  }
  if (closed_)
  {
    return TradingStatus::Closed;
  }
  if (auctionMode_)
  {
    return TradingStatus::AuctionPreOpen;
  }
  if (cfg_.halted)
  {
    return static_cast<bool>(haltUntil_) ? TradingStatus::LuldPause : TradingStatus::Halted;
  }
  return TradingStatus::Trading;
}

// Session state (see AdminAction::CloseSession / OpenSession).
template <class Book>
bool MatchingEngine<Book>::sessionClosed() const noexcept
{
  return closed_;
}

// Withdrawn from trading (see AdminAction::Delist / Relist).
template <class Book>
bool MatchingEngine<Book>::delisted() const noexcept
{
  return delisted_;
}

// Control-plane hook (and the AdminCmd Halt/Resume path). Clears any timed
// pause deadline on resume, so the state the feed publishes is the state the
// engine is actually in.
template <class Book>
void MatchingEngine<Book>::setHalted(bool halted)
{
  cfg_.halted = halted;
  if (!halted)
  {
    haltUntil_ = SeqNanos{};
  }
  publishStatus(TradingStatusReason::Administrative);
}

// Operator emergency stop: halt the symbol (reject new orders) AND pull the
// entire resting book -- every live limit order and pending conditional --
// releasing reservations. Used on a fat-finger event or system anomaly.
template <class Book>
void MatchingEngine<Book>::haltAndCancelAll()
{
  cfg_.halted = true;
  haltUntil_ = SeqNanos{};  // an operator halt has no deadline, unlike the LULD pause
  // The halt reaches the feed BEFORE the flood of cancels it causes, so a
  // subscriber reads them as consequences of a halt rather than as an
  // unexplained mass cancel.
  publishStatus(TradingStatusReason::Administrative);
  cancelEntireBook(CancelReason::VenueHalt);
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
  std::vector<OrderId> resting;
  // order: collected here, id-sorted below -- each surviving id publishes
  // an OrderCanceled
  for (const auto& [acct, ids] : byAccount_)
  {
    (void)acct;
    resting.insert(resting.end(), ids.begin(), ids.end());
  }
  std::sort(resting.begin(), resting.end());  // deterministic cancel order (layout-independent)
  for (OrderId id : resting)
  {
    if (auto ro = book_.cancel(id))
    {
      const uint64_t acct = ownerOf(id);
      releaseReservation(id);
      forgetOrder(id);
      sink_(OrderCanceled{id, cfg_.id, reason, acct, ro->clientOrderId});
    }
  }
  for (OrderId id : stops_.ids())
  {
    const uint64_t acct = stops_.accountOf(id);
    const uint64_t clOrd = stops_.clientOrderIdOf(id);
    if (stops_.cancel(id))
    {
      releaseReservation(id);
      sink_(OrderCanceled{id, cfg_.id, reason, acct, clOrd});
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
  closed_ = true;
  publishStatus(TradingStatusReason::Session);
}

template <class Book>
void MatchingEngine<Book>::openSession()
{
  closed_ = false;
  publishStatus(TradingStatusReason::Session);
}

// Withdraw the instrument from trading. Unlike a halt or a closed session,
// this carries no promise of a return, so leaving orders resting would leave
// them waiting for an open that is not coming -- the book is pulled on the
// way out, through the same path an emergency halt uses.
template <class Book>
void MatchingEngine<Book>::delist()
{
  if (delisted_)
  {
    return;
  }
  delisted_ = true;
  // The status reaches the feed before the cancels it causes, so a subscriber
  // reads them as a consequence rather than as an unexplained mass cancel.
  publishStatus(TradingStatusReason::Administrative);
  cancelEntireBook(CancelReason::VenueHalt);
}

template <class Book>
void MatchingEngine<Book>::relist()
{
  delisted_ = false;
  publishStatus(TradingStatusReason::Administrative);
}

// Pre-open: orders accumulate without matching (a crossed book is allowed).
template <class Book>
void MatchingEngine<Book>::beginPreOpen()
{
  auctionMode_ = true;
  publishStatus(TradingStatusReason::Auction);
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
  cfg_.halted = false;
  haltUntil_ = SeqNanos{};
  auctionMode_ = true;
  publishStatus(TradingStatusReason::Auction);
}

// Run the (opening / closing) uncross auction: match everything at the single
// volume-maximizing price, then resume continuous trading.
template <class Book>
void MatchingEngine<Book>::openContinuous()
{
  // The uncross is a state of its own for exactly as long as it runs: a
  // subscriber must be able to attribute the burst of fills to it rather than
  // to continuous trading that has not resumed yet.
  emitStatus(TradingStatus::AuctionUncross, TradingStatusReason::Auction, 0);
  runAuction();
  auctionMode_ = false;
  publishStatus(TradingStatusReason::Auction);
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
        book_.cancel(blocked);
        releaseReservation(blocked);
        forgetOrder(blocked);
        sink_(OrderCanceled{blocked, cfg_.id, lim.reason, blockedAcct, blockedClOrd});
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
