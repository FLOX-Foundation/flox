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
  if (auto it = byAccount_.find(acct); it != byAccount_.end())
  {
    // order: sorted below, before the vector is handed out
    for (OrderId id : it->second)
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
  Amount t = 0;
  // order: not observable -- Amount sum of posted margin
  for (const auto& [acct, p] : positions_)
  {
    (void)acct;
    t += p.margin;
  }
  return t;
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
  const Amount notional = notionalRaw(priceRaw, qtyRaw, cfg_.priceScale, cfg_.qtyScale);
  return notional * cfg_.initialMarginBps / 10000;
}

template <class Book>
bool MatchingEngine<Book>::reserveFunds(const NewOrder& o)
{
  // Permission first, funding second. An external risk owner answers a
  // question the engine cannot ("is this account good for it across every
  // instrument it holds?"), so it has to be asked whether or not this engine
  // also posts collateral -- both money branches below used to return before
  // the hook was ever reached, so binding a ledger silently disabled it.
  if (credit_)
  {
    const CreditDecision d = credit_(CreditRequest{o.id, o.accountId, cfg_.id, o.side, o.type,
                                                   o.price, o.quantity, o.reduceOnly});
    if (!d.allowed)
    {
      creditReason_ = d.reason;  // surfaced by the caller's reject
      return false;
    }
  }
  if (ledger_ != nullptr && cfg_.linearPerp)
  {
    // Derivatives: reserve initial margin in quote collateral (reduce-only
    // reserves nothing -- it frees position margin instead).
    //
    // An unpriced (market / triggered-stop) order is bounded by the price
    // band, and for a linear perp that bound is the band's TOP on BOTH sides.
    // Nothing is delivered here: the exposure is notional, and notional --
    // hence initial margin -- grows with price whether the account is long or
    // short. Bounding a perp SELL at minPrice (which is the correct worst case
    // for a SPOT seller, who delivers base and whose quote proceeds only grow
    // with price -- see the spot branch below) under-reserves it by the whole
    // maxPrice/minPrice ratio, and consumeOrderIM then caps the position's
    // margin at that under-reserved number.
    const int64_t limitRaw = (o.type == OrderType::LIMIT) ? o.price.raw() : cfg_.maxPrice.raw();
    // A market/stop order with no price band (limitRaw == 0) cannot be bounded
    // for margin: reserving 0 IM would let it open a position with no
    // collateral. Reject rather than admit an uncollateralized fill.
    if (!o.reduceOnly && o.type != OrderType::LIMIT && limitRaw <= 0)
    {
      return false;
    }
    const Amount im = o.reduceOnly ? 0 : imForRaw(o.quantity.raw(), limitRaw);
    if (im > 0 && !ledger_->reserve(o.accountId, cfg_.quoteAsset, im))
    {
      return false;
    }
    reserve_[o.id] = Reservation{o.accountId, cfg_.quoteAsset, im, limitRaw, o.side};
    return true;
  }
  if (ledger_ != nullptr)
  {
    AssetId asset;
    Amount amt;
    int64_t limitRaw;
    if (o.side == Side::BUY)
    {
      const Price px = (o.type == OrderType::LIMIT || cfg_.maxPrice.raw() == 0) ? o.price
                                                                                : cfg_.maxPrice;
      // A market/stop buy with no price band (px == 0) cannot bound its quote
      // spend, so reserving amountOf(0) would gate nothing and let the fill
      // drive the account's reserved balance negative. Reject it instead.
      if (px.raw() <= 0)
      {
        return false;
      }
      asset = cfg_.quoteAsset;
      amt = notionalRaw(px.raw(), o.quantity.raw(), cfg_.priceScale, cfg_.qtyScale);
      limitRaw = px.raw();
    }
    else
    {
      asset = cfg_.baseAsset;
      amt = amountOf(o.quantity);
      limitRaw = o.price.raw();
    }
    if (!ledger_->reserve(o.accountId, asset, amt))
    {
      return false;
    }
    reserve_[o.id] = Reservation{o.accountId, asset, amt, limitRaw, o.side};
    return true;
  }
  return true;
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
  if (ledger_ == nullptr)
  {
    return;
  }
  auto it = reserve_.find(id);
  if (it == reserve_.end())
  {
    return;
  }
  if (it->second.reservedRaw > 0)
  {
    ledger_->release(it->second.account, it->second.asset, it->second.reservedRaw);
  }
  reserve_.erase(it);
}

// Any open last-look hold referencing this order (as maker or taker)?
template <class Book>
bool MatchingEngine<Book>::hasHoldsFor(OrderId id) const
{
  if (held_.empty())
  {
    return false;
  }
  // order: not observable -- a predicate scan, first match wins
  for (const auto& [hid, h] : held_)
  {
    (void)hid;
    if (h.maker == id || h.taker == id)
    {
      return true;
    }
  }
  return false;
}

// releaseReservation, but keep the slice backing this order's open held
// fills reserved: a canceled residual must not strip the collateral an
// accept still needs to settle from (the unchecked-debit fallback would
// credit the counterparty against a possibly failing debit).
template <class Book>
void MatchingEngine<Book>::releaseReservationExceptHeld(OrderId id)
{
  if (ledger_ == nullptr)
  {
    return;
  }
  Quantity heldQty{};
  // order: not observable -- Quantity sum of the held slices
  for (const auto& [hid, h] : held_)
  {
    (void)hid;
    if (h.taker == id || h.maker == id)
    {
      heldQty += h.qty;
    }
  }
  if (heldQty.isZero())
  {
    releaseReservation(id);
    return;
  }
  auto it = reserve_.find(id);
  if (it == reserve_.end())
  {
    return;
  }
  Amount keep;
  if (cfg_.linearPerp)
  {
    keep = imForRaw(heldQty.raw(), it->second.limitPriceRaw);
  }
  else if (it->second.side == Side::BUY)
  {
    keep = notionalRaw(it->second.limitPriceRaw, heldQty.raw(), cfg_.priceScale, cfg_.qtyScale);
  }
  else
  {
    keep = amountOf(heldQty);
  }
  if (keep > it->second.reservedRaw)
  {
    keep = it->second.reservedRaw;
  }
  const Amount rel = it->second.reservedRaw - keep;
  if (rel > 0)
  {
    ledger_->release(it->second.account, it->second.asset, rel);
    it->second.reservedRaw = keep;
  }
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
    // settlement (PnL, margin, fees) is skipped. Leaving positions_ empty
    // here is what used to make reduce-only reject unconditionally, degrade
    // the position cap to a per-order cap, and publish open interest as a
    // flat zero.
    if (cfg_.linearPerp)
    {
      const bool takerBuys = (t.takerSide == Side::BUY);
      updatePerpPosition(takerBuys ? t.takerAccount : t.makerAccount,
                         takerBuys ? t.takerId : t.makerId, true, t.quantity.raw(),
                         t.price.raw());
      updatePerpPosition(takerBuys ? t.makerAccount : t.takerAccount,
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

  auto rb = reserve_.find(buyerId);
  auto rs = reserve_.find(sellerId);
  const bool buyerUnreserved = (rb == reserve_.end());
  const bool sellerUnreserved = (rs == reserve_.end());

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
    const Amount limitNotional = notionalRaw(rb->second.limitPriceRaw, t.quantity.raw(),
                                             cfg_.priceScale, cfg_.qtyScale);
    ledger_->spendReserved(buyerAcct, cfg_.quoteAsset, notional);
    if (limitNotional > notional)
    {
      ledger_->release(buyerAcct, cfg_.quoteAsset, limitNotional - notional);
    }
    rb->second.reservedRaw -= limitNotional;
  }
  ledger_->credit(buyerAcct, cfg_.baseAsset, qtyRaw);

  // Seller: deliver base from reserved, receive quote.
  if (!sellerUnreserved)
  {
    ledger_->spendReserved(sellerAcct, cfg_.baseAsset, qtyRaw);
    rs->second.reservedRaw -= qtyRaw;
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
