/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

// MatchingEngine<Book>: fees, reservations and the settlement ledger.
//
// Included only from flox-venue/matching_engine.h, which declares every member
// defined here. Including it directly gives a fragment with no class to attach
// to, so the include is refused rather than left to fail on the first method.
#ifndef FLOX_VENUE_MATCHING_ENGINE_INL
#error "flox-venue/engine/ledger_fees.inl is a fragment of flox-venue/matching_engine.h; include that instead"
#endif

namespace flox::venue
{

template <class Book>
void MatchingEngine<Book>::setFeeSchedule(flox::FeeSchedule fees)
{
  fees_ = std::move(fees);
  feesEnabled_ = true;
}

// Bind a settlement ledger. When set, order entry reserves buying power
// (quote for a bid, base for an ask), fills settle base<->quote and fees, and
// cancels release the remainder. `venueAccount` receives net fees.
template <class Book>
void MatchingEngine<Book>::setLedger(Ledger* ledger, uint64_t venueAccount)
{
  ledger_ = ledger;
  venueAccount_ = venueAccount;
  // One place either is set, so the component's mirror cannot drift from it.
  clearing_.setLedger(ledger, venueAccount);
}

// Client reconnect reconciliation: the account's live resting orders and perp
// position, open orders in id order. Balances come from the ledger; combine
// at the gateway. Off the hot path (a query, not order flow).
//
// openOrders is SORTED, not merely collected. The ids come out of
// byAccount_'s per-account std::unordered_set, so their traversal order is a
// bucket-layout artifact. session_verbs.h answers AccountSnapshotRequest by
// encoding this vector straight to the wire, one OrderAccepted frame per
// entry, so an unsorted enumeration makes the FRAME SEQUENCE a reconnecting
// client receives depend on which standard library the venue was built
// against, while the set of orders reported is identical either way -- the
// same divergence StopBook::ids() carried into the emergency-cancel stream.
// pendingStops needs no sort: StopBook::forEachPending walks the two
// trigger-ordered multimaps and the trailing vector.
template <class Book>
typename MatchingEngine<Book>::AccountSnapshot MatchingEngine<Book>::snapshotAccount(uint64_t acct) const
{
  AccountSnapshot s;
  if (const auto* own = pub_.ordersOf(acct))
  {
    const std::unordered_set<OrderId>& ids = *own;
    // order: sorted below, before the vector is handed out
    for (OrderId id : ids)
    {
      if (const RestingOrder* r = book_.find(id))
      {
        s.openOrders.push_back(
            {id, r->side, r->price, Quantity::fromRaw(r->leaves.raw() + r->hidden.raw())});
      }
    }
    std::sort(s.openOrders.begin(), s.openOrders.end(),
              [](const OrderView& a, const OrderView& b)
              { return a.id < b.id; });
  }
  stops_.forEachPending(
      [&](const NewOrder& o, Price trigger)
      {
        if (o.accountId == acct)
        {
          s.pendingStops.push_back({o.id, o.side, o.type, trigger, o.quantity});
        }
      });
  s.positionQty = positionQty(acct);
  s.positionEntry = positionEntry(acct);
  return s;
}

// Total posted position margin across accounts (quote raw). After all resting
// orders are drained, this must equal the ledger's total reserved -- every
// reserved unit is backed by either a live order or an open position.
template <class Book>
Amount MatchingEngine<Book>::totalPositionMargin() const
{
  return clearing_.totalPositionMargin();
}

// Trades that printed but could not be settled without creating value, so
// nothing moved (see reportUnsettled). Must stay at zero: a non-zero value
// means a fill reached clearing with neither a reservation nor the balance to
// pay for it, and the venue refused to invent the difference.
template <class Book>
uint64_t MatchingEngine<Book>::unsettledTrades() const noexcept
{
  return unsettledTrades_;
}

template <class Book>
Ledger* MatchingEngine<Book>::ledger() const noexcept
{
  return ledger_;
}

template <class Book>
uint64_t MatchingEngine<Book>::venueAccount() const noexcept
{
  return venueAccount_;
}

template <class Book>
void MatchingEngine<Book>::emitFees(const Trade& t)
{
  if (!feesEnabled_)
  {
    return;
  }
  const double notional =
      static_cast<double>(
          notionalRaw(t.price.raw(), t.quantity.raw(), cfg_.priceScale, cfg_.qtyScale)) /
      kMoneyScale;
  sink_(FeeCharged{t.makerId, cfg_.id, Volume::fromDouble(fees_.feeFor(now_.raw(), notional, true)),
                   true, t.makerAccount});
  sink_(FeeCharged{t.takerId, cfg_.id, Volume::fromDouble(fees_.feeFor(now_.raw(), notional, false)),
                   false, t.takerAccount});
}

template <class Book>
Amount MatchingEngine<Book>::imForRaw(int64_t qtyRaw, int64_t priceRaw) const
{
  return credit_.imForRaw(qtyRaw, priceRaw, cfg_);
}

template <class Book>
bool MatchingEngine<Book>::reserveFunds(const NewOrder& o)
{
  return credit_.reserveFunds(o, cfg_, ledger_);
}

// Deposits/withdrawals are sequenced commands, not direct Ledger calls, so
// the WAL is the single source of truth for balances: a replay from an empty
// ledger reproduces them. Without a ledger bound they are no-ops.
template <class Book>
void MatchingEngine<Book>::onDeposit(const Deposit& d)
{
  // A money command against an engine that holds no ledger is answered, not
  // swallowed: silence is indistinguishable from a lost command, and the
  // sender has no other way to learn the funds went nowhere.
  if (ledger_ == nullptr)
  {
    sink_(OrderRejected{0, cfg_.id, RejectReason::NoLedgerBound, d.accountId});
    return;
  }
  if (d.amountRaw <= 0)
  {
    return;
  }
  ledger_->deposit(d.accountId, d.asset, static_cast<Amount>(d.amountRaw));
  // The owner sees the credit land. Deterministic (post-event ledger state),
  // so it is part of the event hash; journal replay re-emits it and the
  // shard's recovery suppression keeps replayed/snapshot-restored copies off
  // the wire.
  emitBalance(d.accountId, d.asset, BalanceReason::Deposit);
}

template <class Book>
void MatchingEngine<Book>::onWithdraw(const Withdraw& w)
{
  if (ledger_ == nullptr)
  {
    sink_(OrderRejected{0, cfg_.id, RejectReason::NoLedgerBound, w.accountId});
    return;
  }
  if (w.amountRaw <= 0)
  {
    return;
  }
  // debit is all-or-nothing (checks available >= amount): an uncovered
  // withdraw changes nothing, so conservation and replay stay intact.
  const bool ok = ledger_->debit(w.accountId, w.asset, static_cast<Amount>(w.amountRaw));
  emitBalance(w.accountId, w.asset,
              ok ? BalanceReason::Withdraw : BalanceReason::WithdrawRejected);
}

template <class Book>
void MatchingEngine<Book>::emitBalance(uint64_t account, AssetId asset, BalanceReason reason)
{
  sink_(BalanceUpdate{account, asset, static_cast<int64_t>(ledger_->available(account, asset)),
                      static_cast<int64_t>(ledger_->reserved(account, asset)), reason});
}

template <class Book>
void MatchingEngine<Book>::releaseReservation(OrderId id)
{
  credit_.releaseReservation(id, ledger_);
}

// Any open last-look hold referencing this order (as maker or taker)?
template <class Book>
bool MatchingEngine<Book>::hasHoldsFor(OrderId id) const
{
  return lastLook_.referencesOrder(id);
}

// releaseReservation, but keep the slice backing this order's open held
// fills reserved: a canceled residual must not strip the collateral an
// accept still needs to settle from (the unchecked-debit fallback would
// credit the counterparty against a possibly failing debit). The hold table
// is the engine's, so the held quantity is measured here and the money is
// left to engine::Credit.
template <class Book>
void MatchingEngine<Book>::releaseReservationExceptHeld(OrderId id)
{
  if (ledger_ == nullptr)
  {
    return;
  }
  const Quantity heldQty = lastLook_.heldQtyFor(id);
  credit_.releaseReservationExceptHeld(id, heldQty, cfg_, ledger_);
}

// After the last hold on an order resolves: if the order is gone from the
// book (and the stop book), free any leftover reservation and drop tracking.
// Complements the emit_ wrapper, which defers this cleanup while holds are
// open (see the complete-with-holds note there).
template <class Book>
void MatchingEngine<Book>::cleanupOrderIfDone(OrderId id)
{
  if (book_.contains(id) || stops_.contains(id) || hasHoldsFor(id))
  {
    return;
  }
  releaseReservation(id);
  forgetOrder(id);
}

// A printed trade could not be settled without creating value, so nothing was
// moved. Counted and logged rather than event-carried: the counter is a
// diagnostic (like droppedSnapshotRecords), and adding an event here would
// change the outbound stream on a path that must stay unreachable.
template <class Book>
void MatchingEngine<Book>::reportUnsettled(const Trade& t, uint64_t account, const char* why)
{
  ++unsettledTrades_;
  std::fprintf(stderr,
               "flox-venue: trade %llu on symbol %u NOT settled (%s, account %llu) -- "
               "no value moved\n",
               static_cast<unsigned long long>(t.tradeId), static_cast<unsigned>(cfg_.id), why,
               static_cast<unsigned long long>(account));
}

template <class Book>
void MatchingEngine<Book>::settleTrade(const Trade& t)
{
  if (ledger_ == nullptr)
  {
    // A position is EXPOSURE, not cash: it is what reduce-only, the position
    // cap and open interest are computed from, and none of those are money
    // questions. So a ledgerless perp still tracks positions -- only the
    // settlement (PnL, margin, fees) is skipped. Leaving the positions empty
    // here is what used to make reduce-only reject unconditionally, degrade
    // the position cap to a per-order cap, and publish open interest as a
    // flat zero.
    if (cfg_.linearPerp)
    {
      const bool takerBuys = (t.takerSide == Side::BUY);
      clearing_.updatePerpPosition(takerBuys ? t.takerAccount : t.makerAccount,
                                   takerBuys ? t.takerId : t.makerId, true, t.quantity.raw(),
                                   t.price.raw());
      clearing_.updatePerpPosition(takerBuys ? t.makerAccount : t.takerAccount,
                                   takerBuys ? t.makerId : t.takerId, false, t.quantity.raw(),
                                   t.price.raw());
    }
    emitFees(t);  // no settlement: fee events only
    return;
  }
  if (cfg_.linearPerp)
  {
    settlePerp(t);
    return;
  }
  const Amount notional =
      notionalRaw(t.price.raw(), t.quantity.raw(), cfg_.priceScale, cfg_.qtyScale);
  const Amount qtyRaw = amountOf(t.quantity);
  const bool takerBuys = (t.takerSide == Side::BUY);
  const OrderId buyerId = takerBuys ? t.takerId : t.makerId;
  const uint64_t buyerAcct = takerBuys ? t.takerAccount : t.makerAccount;
  const OrderId sellerId = takerBuys ? t.makerId : t.takerId;
  const uint64_t sellerAcct = takerBuys ? t.makerAccount : t.takerAccount;

  Reservation* rb = credit_.find(buyerId);
  Reservation* rs = credit_.find(sellerId);
  const bool buyerUnreserved = (rb == nullptr);
  const bool sellerUnreserved = (rs == nullptr);

  // VALUE INTEGRITY. A leg with no reservation has to settle straight out of
  // `available`, and Ledger::debit is all-or-nothing: it can refuse. Crediting
  // the counterparty regardless -- which is what discarding the debit's result
  // amounts to -- CREATES the credited amount out of nothing. So both
  // unreserved legs are funded first, each checked, before anything is
  // credited: if either refuses, the settlement is abandoned as a unit (the
  // first debit, if it happened, is credited back by exactly the same amount),
  // the trade is counted as unsettled and nothing moves. Every path that
  // reaches here is supposed to hold a reservation; this is the floor under
  // any path that ever stops doing so.
  if (buyerUnreserved && !ledger_->debit(buyerAcct, cfg_.quoteAsset, notional))
  {
    reportUnsettled(t, buyerAcct, "buyer cannot fund quote");
    return;
  }
  if (sellerUnreserved && !ledger_->debit(sellerAcct, cfg_.baseAsset, qtyRaw))
  {
    if (buyerUnreserved)
    {
      ledger_->credit(buyerAcct, cfg_.quoteAsset, notional);  // exact undo of the debit above
    }
    reportUnsettled(t, sellerAcct, "seller cannot deliver base");
    return;
  }

  // Past this point the settlement cannot fail: what is left is reserved
  // spending (already ring-fenced) and credits.
  // Buyer: pay quote from reserved (refund over-reservation), receive base.
  if (!buyerUnreserved)
  {
    const Amount limitNotional = notionalRaw(rb->limitPriceRaw, t.quantity.raw(),
                                             cfg_.priceScale, cfg_.qtyScale);
    ledger_->spendReserved(buyerAcct, cfg_.quoteAsset, notional);
    if (limitNotional > notional)
    {
      ledger_->release(buyerAcct, cfg_.quoteAsset, limitNotional - notional);
    }
    rb->reservedRaw -= limitNotional;
  }
  ledger_->credit(buyerAcct, cfg_.baseAsset, qtyRaw);

  // Seller: deliver base from reserved, receive quote.
  if (!sellerUnreserved)
  {
    ledger_->spendReserved(sellerAcct, cfg_.baseAsset, qtyRaw);
    rs->reservedRaw -= qtyRaw;
  }
  ledger_->credit(sellerAcct, cfg_.quoteAsset, notional);

  if (feesEnabled_)
  {
    const double notionalD =
        static_cast<double>(
            notionalRaw(t.price.raw(), t.quantity.raw(), cfg_.priceScale, cfg_.qtyScale)) /
        kMoneyScale;
    chargeFee(t.makerId, t.makerAccount, fees_.feeFor(now_.raw(), notionalD, true), true);
    chargeFee(t.takerId, t.takerAccount, fees_.feeFor(now_.raw(), notionalD, false), false);
  }
}

template <class Book>
void MatchingEngine<Book>::chargeFee(OrderId id, uint64_t acct, double feeD, bool maker)
{
  const Amount fee = static_cast<Amount>(Volume::fromDouble(feeD).raw());
  // Signed move: participant -fee, venue +fee (conserves value; fee<0 = rebate).
  ledger_->credit(acct, cfg_.quoteAsset, -fee);
  ledger_->credit(venueAccount_, cfg_.quoteAsset, fee);
  sink_(FeeCharged{id, cfg_.id, Volume::fromDouble(feeD), maker, acct});
}

}  // namespace flox::venue
