/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

// MatchingEngine<Book>: the resting-order lifecycle -- stops, triggers, modify, cancel.
//
// Included only from flox-venue/matching_engine.h, which declares every member
// defined here. Including it directly gives a fragment with no class to attach
// to, so the include is refused rather than left to fail on the first method.
#ifndef FLOX_VENUE_MATCHING_ENGINE_INL
#error "flox-venue/engine/orders.inl is a fragment of flox-venue/matching_engine.h; include that instead"
#endif

namespace flox::venue
{

// Conditional order (stop / take-profit / trailing): park in the stop book
// until the last-trade price crosses the trigger.
// Park a conditional (stop / take-profit / trailing) order. Returns true if it
// was accepted into the stop book, false if rejected -- the caller uses this to
// decide whether to keep or unlink an OCO group membership.
template <class Book>
bool MatchingEngine<Book>::onStop(const NewOrder& o)
{
  if (o.symbol != cfg_.id)
  {
    sink_(OrderRejected{o.id, o.symbol, RejectReason::UnknownSymbol, o.accountId, o.clientOrderId});
    return false;
  }
  if (session_.delisted())
  {
    sink_(OrderRejected{o.id, o.symbol, RejectReason::InstrumentDelisted, o.accountId, o.clientOrderId});
    return false;
  }
  if (session_.closed())
  {
    sink_(OrderRejected{o.id, o.symbol, RejectReason::MarketClosed, o.accountId, o.clientOrderId});
    return false;
  }
  if (cfg_.halted)
  {
    sink_(OrderRejected{o.id, o.symbol, RejectReason::Halted, o.accountId, o.clientOrderId});
    return false;
  }
  if (o.quantity.raw() <= 0)
  {
    sink_(OrderRejected{o.id, o.symbol, RejectReason::InvalidQuantity, o.accountId, o.clientOrderId});
    return false;
  }
  const bool trailing = (o.type == OrderType::TRAILING_STOP);
  if (!trailing && o.triggerPrice.raw() <= 0)
  {
    sink_(OrderRejected{o.id, o.symbol, RejectReason::InvalidPrice, o.accountId, o.clientOrderId});
    return false;
  }
  if (trailing && o.trailingOffset.raw() <= 0)
  {
    sink_(OrderRejected{o.id, o.symbol, RejectReason::InvalidPrice, o.accountId, o.clientOrderId});
    return false;
  }
  if (isLimitStop(o.type) && o.price.raw() <= 0)
  {
    sink_(OrderRejected{o.id, o.symbol, RejectReason::InvalidPrice, o.accountId, o.clientOrderId});
    return false;
  }
  if (book_.contains(o.id) || stops_.contains(o.id))
  {
    sink_(OrderRejected{o.id, o.symbol, RejectReason::DuplicateOrderId, o.accountId, o.clientOrderId});
    return false;
  }

  Price initTrig{};
  if (trailing)
  {
    if (hasLast_)
    {
      initTrig = (o.side == Side::SELL)
                     ? Price::fromRaw(lastPrice_.raw() - o.trailingOffset.raw())
                     : Price::fromRaw(lastPrice_.raw() + o.trailingOffset.raw());
    }
  }
  else
  {
    initTrig = o.triggerPrice;
  }
  stops_.add(o, initTrig, trailing);
  // GTD binds a conditional order as much as a resting one: a stop whose
  // deadline passes before it ever triggers has to expire, not wait forever
  // for a price that may never come.
  if (o.tif == TimeInForce::GTD && static_cast<bool>(o.expiryNs))
  {
    expiry_.set(o.id, o.expiryNs);
  }
  // A pending stop has not triggered, so it has filled nothing -- cumQty
  // is always 0.
  sink_(OrderAccepted{o.id, o.symbol, o.side, o.triggerPrice, o.quantity, false, Quantity{},
                      o.accountId, o.clientOrderId, Quantity{}});  // pending, not on book
  processTriggers();                                               // may already be in-the-money
  return true;
}

// Fire every stop the current last price crosses, cascading: each fired stop
// trades, which can move the last price and trigger further stops.
template <class Book>
std::optional<Price> MatchingEngine<Book>::triggerReference() const
{
  if (cfg_.triggerRef == TriggerRef::Mark)
  {
    return hasMark_ ? std::optional<Price>{markPrice_} : std::nullopt;
  }
  return hasLast_ ? std::optional<Price>{lastPrice_} : std::nullopt;
}

template <class Book>
void MatchingEngine<Book>::processTriggers()
{
  // A stop re-enters matching here rather than through onNew, so the
  // instrument-state check that guards new orders has to be repeated -- it
  // is not inherited. Without this a mark update fires stops straight
  // through a halt, a LULD pause, a closed session or a pre-open auction:
  // the trigger reference keeps moving even when trading does not.
  if (tradingStatus() != TradingStatus::Trading)
  {
    return;
  }
  auto ref = triggerReference();
  if (!ref)
  {
    return;
  }
  stops_.updateTrailing(*ref);
  while (auto agg = stops_.popTriggered(*ref))
  {
    sink_(OrderTriggered{agg->id, cfg_.id, *ref, agg->accountId});
    // Perp risk gates: a triggered stop re-enters matching HERE, not through
    // onNew, so it must run the same reduce-only cap + position-limit checks --
    // else a reduce-only stop opens an uncollateralized (im=0) position and a
    // plain stop bypasses maxPositionQty. Reject (and free nothing -- not yet
    // reserved) if the reduce-only cap leaves nothing or the cap is breached.
    if (const RejectReason r = perpRiskGate(*agg); r != RejectReason::None)
    {
      sink_(OrderRejected{agg->id, cfg_.id, r, agg->accountId, agg->clientOrderId});
      ref = triggerReference();
      if (!ref)
      {
        break;
      }
      stops_.updateTrailing(*ref);
      continue;
    }
    // Reserve buying power now that the stop is a live aggressor -- else an
    // underfunded triggered stop would settle via unchecked debit and create
    // value (buyer receives base it can't pay for).
    if (!reserveFunds(*agg))
    {
      sink_(OrderRejected{agg->id, cfg_.id, RejectReason::InsufficientFunds, agg->accountId, agg->clientOrderId});
      ref = triggerReference();
      if (!ref)
      {
        break;
      }
      stops_.updateTrailing(*ref);
      continue;
    }
    const MatchOutcome out =
        matcher_.cross(*agg, book_, [this]()
                       { return ++tradeSeq_; }, emit_);
    stampFreshHolds();
    if (out.reject != RejectReason::None)
    {
      releaseReservation(agg->id);
      sink_(OrderRejected{agg->id, cfg_.id, out.reject, agg->accountId, agg->clientOrderId});
    }
    else if (out.residualRests)
    {
      RestingOrder rro{agg->id, agg->accountId, agg->price, out.leaves, agg->side};
      rro.clientOrderId = agg->clientOrderId;
      rro.reduceOnly = agg->reduceOnly;
      // The triggered stop crossed as an aggressor before its residual
      // rests -- out.filled is what it filled of itself. Stamped onto the
      // RestingOrder for the same reason as validate.inl's residualRests
      // branch: a later report on this order must not start its cumQty over
      // at 0.
      rro.cumQty = out.filled;
      if (const RejectReason why = restOnBook(agg->side, rro); why != RejectReason::None)
      {
        // Same rule as the submit path: a residual the book refused is a
        // reject when the triggered order printed nothing, and a cancel once
        // it has, because a reject cannot follow its own executions.
        if (out.filled.isZero())
        {
          releaseReservation(agg->id);
          sink_(OrderRejected{agg->id, cfg_.id, why, agg->accountId, agg->clientOrderId});
        }
        else
        {
          releaseReservationExceptHeld(agg->id);
          sink_(OrderCanceled{agg->id, cfg_.id, CancelReason::BookRefused, agg->accountId,
                              agg->clientOrderId, out.leaves, out.filled});
        }
      }
      else
      {
        trackResting(agg->id, agg->accountId, agg->stp);
        sink_(OrderAccepted{agg->id, cfg_.id, agg->side, agg->price, out.leaves, true, Quantity{},
                            agg->accountId, agg->clientOrderId, out.filled});
      }
    }
    else if (out.residualCanceled)
    {
      releaseReservationExceptHeld(agg->id);  // held slices stay reserved
      sink_(OrderCanceled{agg->id, cfg_.id, out.residualCancelReason, agg->accountId,
                          agg->clientOrderId, out.leaves, out.filled});
    }
    ref = triggerReference();  // trades may have moved the last-price reference
    if (!ref)
    {
      break;
    }
    stops_.updateTrailing(*ref);
  }
}

template <class Book>
void MatchingEngine<Book>::onModify(const ModifyOrder& m)
{
  if (m.symbol != cfg_.id)
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::UnknownSymbol, m.accountId, true});
    return;
  }
  if (admissionDenies(m.accountId, AdmissionDeny::DenyAmend))
  {
    credit_.countAdmissionReject();
    sink_(CancelRejected{m.id, m.symbol, RejectReason::AmendNotPermitted, m.accountId, true});
    return;
  }
  // Ownership BEFORE any side effect: rejecting the order's holds is itself a
  // change to the owner's position, so a stranger must not get that far.
  if (ownershipRefused(m.id, m.accountId))
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::NotOrderOwner, m.accountId, true});
    return;
  }
  // The amend is DECIDED before anything is touched. Rejecting the order's
  // holds (below) reshapes both the order and its reservation, so an amend
  // that is then refused would have destroyed a hold the order still needs --
  // the order survives the refusal and its holds do not.
  //
  // The resting record cannot be read this early: a maker whose whole size is
  // held out of the book is absent from it until the holds resolve, and
  // rejectHoldsFor is what puts it back. So existence comes from the tracking
  // index, which knows a held-out order is still live, and the checks that
  // read the amend itself run here. The one that cannot is the price of an
  // amend that names none: that price is the record's own, and it is checked
  // below, once the record is readable again.
  const auto priceRefusal = [this](Price p) -> RejectReason
  {
    if (p.raw() <= 0)
    {
      return RejectReason::InvalidPrice;
    }
    if (!cfg_.tickSize.isZero() && (p.raw() % cfg_.tickSize.raw()) != 0)
    {
      return RejectReason::TickSizeViolation;
    }
    if (!cfg_.minPrice.isZero() && p < cfg_.minPrice)
    {
      return RejectReason::InvalidPrice;
    }
    if (!cfg_.maxPrice.isZero() && cfg_.maxPrice < p)
    {
      return RejectReason::InvalidPrice;
    }
    return RejectReason::None;
  };
  if (!pub_.tracked(m.id))
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::UnknownOrder, m.accountId, true});
    return;
  }
  const uint64_t owner = ownerOf(m.id);
  if (m.newQty.raw() <= 0)
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::InvalidQuantity, m.accountId, true});
    return;
  }
  const bool amendNamesAPrice = m.newPrice.raw() != 0;
  if (amendNamesAPrice)
  {
    if (const RejectReason r = priceRefusal(m.newPrice); r != RejectReason::None)
    {
      sink_(CancelRejected{m.id, m.symbol, r, owner, true});
      return;
    }
  }
  if (!cfg_.lotSize.isZero() && (m.newQty.raw() % cfg_.lotSize.raw()) != 0)
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::LotSizeViolation, owner, true});
    return;
  }

  // Accepted. Resolve (reject) any last-look holds referencing this order
  // before reshaping it: both modify paths re-shape the order and its
  // reservation, and a hold left behind would later settle against a
  // reservation that no longer covers it. This restores the held slice to the
  // book, so the resting record is read only now.
  rejectHoldsFor(m.id);
  const RestingOrder* cur = book_.find(m.id);
  if (cur == nullptr)
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::UnknownOrder, m.accountId, true});
    return;
  }

  const Side side = cur->side;
  const uint64_t acct = cur->accountId;
  const Quantity curLeaves = cur->leaves;
  const Quantity curHidden = cur->hidden;
  const Quantity curPeak = cur->peak;
  const bool curReduceOnly = cur->reduceOnly;
  const bool curPostOnly = cur->postOnly;
  const bool curLastLook = cur->lastLook;
  const Price curPrice = cur->price;
  // Captured before the cancel below invalidates `cur`. An amend keeps the
  // identifier the submitter gave the original order, for the same reason it
  // keeps postOnly: the order is the same order, and the submitter is still
  // reconciling against the name it chose.
  const uint64_t curClientOrderId = cur->clientOrderId;
  // The order's running cumQty from before this modify. A reduce-in-
  // place never trades (unaffected); a re-enter starts a fresh cross() whose
  // own MatchOutcome::filled knows nothing about fills from the order's
  // earlier life, so this is added back onto every report the re-enter path
  // emits below.
  const Quantity curCumQty = cur->cumQty;
  const Price newPrice = amendNamesAPrice ? m.newPrice : curPrice;

  // An amend that names no price keeps the order's own, which was checked when
  // the order was admitted -- but the instrument can have been retuned since
  // (SetBands, a new tick), and a kept price that no longer obeys it is
  // refused the same as any other. This is the one gate the holds are already
  // resolved for: the price it reads only becomes readable with them gone.
  if (!amendNamesAPrice)
  {
    if (const RejectReason r = priceRefusal(newPrice); r != RejectReason::None)
    {
      sink_(CancelRejected{m.id, m.symbol, r, acct, true});
      return;
    }
  }
  // The book's own band, for the same reason submit asks: an amend to a price
  // the book has no level for would take the order off the book and then fail
  // to put it back. Refused here, the original order is still resting.
  if (!book_.canRest(newPrice))
  {
    sink_(CancelRejected{m.id, m.symbol, RejectReason::InvalidPrice, acct, true});
    return;
  }

  // Same price and shrinking: reduce in place, keeping time priority. Release
  // the reservation for the freed quantity (proportional -- works for spot
  // quote/base and perp IM alike) so the trader regains that buying power.
  // Iceberg orders are excluded: their `leaves` is only the displayed peak,
  // not the full remaining (hidden reserve lives in `curHidden`), so neither
  // the proportional release (denominator would be the peak, not the total)
  // nor book_.reduce (leaves the hidden reserve resting) is correct here.
  // They fall through to the re-enter path, which releases the full old
  // reservation and re-reserves at the new quantity.
  if (newPrice == curPrice && m.newQty <= curLeaves && curHidden.isZero())
  {
    book_.reduce(m.id, m.newQty);
    releaseReservationPro(m.id, curLeaves.raw(), m.newQty.raw());
    sink_(OrderModified{m.id, m.symbol, newPrice, m.newQty, true, acct, curClientOrderId,
                        curCumQty});
    return;
  }

  // Price change or size increase: re-enter at the tail (lost priority). The
  // old reservation is released and buying power is RE-CHECKED at the new
  // price/qty -- a reprice-up must not leave the order under-collateralized.
  book_.cancel(m.id);
  releaseReservation(m.id);
  NewOrder re;
  re.id = m.id;
  re.clientOrderId = curClientOrderId;
  re.symbol = cfg_.id;
  re.side = side;
  re.type = OrderType::LIMIT;
  re.price = newPrice;
  re.quantity = m.newQty;
  re.tif = TimeInForce::GTC;
  re.accountId = acct;
  re.reduceOnly = curReduceOnly;  // preserve reduce-only across the modify
  // A modify re-enters matching as a fresh aggressor, so it must carry the
  // self-trade prevention the original order was admitted with. Rebuilding
  // the order from the resting record alone would silently drop it.
  re.stp = stpOf(m.id);
  // Same reasoning for every other control the order was admitted with.
  // post-only is the one that costs money when it goes missing: the amended
  // order re-enters as a plain aggressor and lifts the book the original was
  // guaranteed never to touch. The iceberg peak is the one that costs
  // secrecy: without it the whole remaining size re-rests as displayed.
  re.postOnly = curPostOnly;
  re.lastLook = curLastLook;
  re.visibleQuantity = curPeak;

  // Perp risk gate: a modified perp order re-enters matching HERE, not through
  // onNew, so it must run the same reduce-only cap + position-cap checks (spot:
  // perpRiskGate is a no-op). reduceOnly is carried from the resting record onto
  // `re` above, so perpRiskGate re-caps it to the current position -- a modify
  // can neither grow past maxPositionQty nor flip a reduce-only order.
  if (const RejectReason r = perpRiskGate(re); r != RejectReason::None)
  {
    forgetOrder(m.id);  // order was already canceled above -> stays gone
    sink_(OrderRejected{m.id, m.symbol, r, acct, curClientOrderId, curCumQty});
    return;
  }
  if (!reserveFunds(re))  // cannot fund the modified order -> reject; order is gone
  {
    forgetOrder(m.id);
    sink_(OrderRejected{m.id, m.symbol, RejectReason::InsufficientFunds, acct, curClientOrderId,
                        curCumQty});
    return;
  }
  const MatchOutcome out =
      matcher_.cross(re, book_, [this]()
                     { return ++tradeSeq_; }, emit_, curCumQty);
  stampFreshHolds();
  // The amended order left the book at the top of this path, so every way the
  // match can end has to say where it went. Reporting only the resting case
  // acks a working order that is not on the book, leaves its reservation
  // frozen and keeps its slot in the per-account open-order cap -- a phantom
  // the owner cannot cancel, because cancel answers UnknownOrder.
  if (out.reject != RejectReason::None)
  {
    releaseReservation(m.id);
    forgetOrder(m.id);
    sink_(OrderRejected{m.id, m.symbol, out.reject, acct, curClientOrderId, curCumQty});
    return;
  }
  if (out.residualCanceled)
  {
    // Self-trade prevention or a fill-time risk block killed the re-entering
    // order. Held slices stay reserved: their accept still has to settle.
    // out.filled is only what this re-cross filled; curCumQty is what
    // the order filled in its life BEFORE the modify -- both count toward
    // the order's real running total.
    releaseReservationExceptHeld(m.id);
    forgetOrder(m.id);
    sink_(OrderCanceled{m.id, m.symbol, out.residualCancelReason, acct, curClientOrderId,
                        out.leaves, curCumQty + out.filled});
    processTriggers();
    return;
  }
  if (out.residualRests)
  {
    RestingOrder mro{m.id, acct, newPrice, out.leaves, side};
    mro.clientOrderId = curClientOrderId;
    mro.reduceOnly = re.reduceOnly;
    mro.postOnly = re.postOnly;
    // Same reasoning as the residualCanceled branch above.
    mro.cumQty = curCumQty + out.filled;
    mro.lastLook = re.lastLook && cfg_.lastLookWindowNs.count() > 0;
    if (re.visibleQuantity.raw() > 0 && re.visibleQuantity < out.leaves)
    {
      mro.peak = re.visibleQuantity;
      mro.leaves = re.visibleQuantity;
      mro.hidden = out.leaves - re.visibleQuantity;
    }
    if (restOnBook(side, mro) != RejectReason::None)
    {
      // The amend already lifted the order off the book, so there is no
      // original left to keep: the order is gone and its owner is told so,
      // rather than being sent an OrderModified about an order on no book.
      releaseReservationExceptHeld(m.id);
      forgetOrder(m.id);
      sink_(OrderCanceled{m.id, m.symbol, CancelReason::BookRefused, acct, curClientOrderId,
                          out.leaves, curCumQty + out.filled});
      processTriggers();
      return;
    }
    trackResting(m.id, acct, re.stp);
  }
  sink_(OrderModified{m.id, m.symbol, newPrice, out.leaves, false, acct, curClientOrderId,
                      curCumQty + out.filled});
  processTriggers();  // a reprice-into-cross may have moved the last price
}

template <class Book>
void MatchingEngine<Book>::onCancel(const CancelOrder& c)
{
  if (c.symbol != cfg_.id)
  {
    sink_(CancelRejected{c.id, c.symbol, RejectReason::UnknownSymbol, c.accountId, false});
    return;
  }
  if (admissionDenies(c.accountId, AdmissionDeny::DenyCancel))
  {
    credit_.countAdmissionReject();
    sink_(CancelRejected{c.id, c.symbol, RejectReason::CancelNotPermitted, c.accountId, false});
    return;
  }
  if (ownershipRefused(c.id, c.accountId))
  {
    sink_(CancelRejected{c.id, c.symbol, RejectReason::NotOrderOwner, c.accountId, false});
    return;
  }
  // Cancel-while-held: deterministically resolve (reject) the order's holds
  // BEFORE removing it. The reject restores held quantity to the book (so the
  // cancel below removes ALL of it and releases the full reservation), and a
  // later accept of the same heldId is impossible (UnknownOrder) -- without
  // this, releaseReservation would strip the held slice and the accept would
  // settle through the unchecked-debit path, breaking conservation.
  rejectHoldsFor(c.id);
  const uint64_t restingAcct = ownerOf(c.id);
  const uint64_t stopAcct = stops_.accountOf(c.id);
  const uint64_t stopClOrd = stops_.clientOrderIdOf(c.id);
  // Captured before stops_.cancel() below erases the entry: a pending
  // stop never partially fills, so its full submitted quantity IS its
  // LeavesQty on this cancel.
  const Quantity stopQty = stops_.quantityOf(c.id);
  if (auto ro = book_.cancel(c.id))
  {
    releaseReservation(c.id);
    forgetOrder(c.id);
    sink_(OrderCanceled{c.id, c.symbol, CancelReason::UserRequested, restingAcct,
                        ro->clientOrderId, ro->leaves + ro->hidden, ro->cumQty});
  }
  else if (stops_.cancel(c.id))
  {
    // A pending stop holds no reservation, but it does hold an OCO
    // membership: leaving it behind means a later trigger of the sibling
    // looks for an order that no longer exists.
    oco_.unlink(c.id);
    forgetOrder(c.id);
    sink_(OrderCanceled{c.id, c.symbol, CancelReason::UserRequested, stopAcct, stopClOrd, stopQty,
                        Quantity{}});
  }
  else
  {
    sink_(CancelRejected{c.id, c.symbol, RejectReason::UnknownOrder, c.accountId, false});
  }
}

// Whether `actor` is barred from acting on the order `id` names.
//
// An order id is one global namespace and the client picks the numbers in it,
// so an id proves nothing about who sent the command that carries it. What
// does carry the authorization claim is accountId: GatewaySession overwrites
// it with the session's authenticated account on every account-bearing
// command, precisely so a client cannot write someone else's number there.
// Checking the claim against the order it names is the other half of that,
// and without it the id alone is the capability -- modify, cancel and quote
// all address orders by id.
//
// accountId 0 is the "unbound / trusted-transport" sentinel the session
// itself documents: an in-process embedder, a replay driver and the
// single-tenant configuration all act as 0 and keep full control.
//
// An id nobody owns is not refused here. It is unknown, and the caller's own
// unknown-order path says so -- answering "not yours" to an id that does not
// exist would turn every rejection into an existence oracle.
template <class Book>
bool MatchingEngine<Book>::ownershipRefused(OrderId id, uint64_t actor) const
{
  if (actor == 0)
  {
    return false;
  }
  if (const uint64_t* owner = pub_.trackedOwner(id))
  {
    return *owner != actor;
  }
  const uint64_t stopAcct = stops_.accountOf(id);
  return stopAcct != 0 && stopAcct != actor;
}

}  // namespace flox::venue
