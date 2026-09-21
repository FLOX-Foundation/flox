/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

// MatchingEngine<Book>: linear-perp clearing -- positions, funding, liquidation, ADL.
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
  auto it = positions_.find(account);
  return it == positions_.end() ? 0 : it->second.qtyRaw;
}

template <class Book>
Price MatchingEngine<Book>::positionEntry(uint64_t account) const
{
  auto it = positions_.find(account);
  return it == positions_.end() ? Price{} : Price::fromRaw(it->second.entryRaw);
}

// Open interest: the long side of the perp positions the engine tracks for
// this symbol. Every contract has a long and a short leg, so the long side is
// the open interest; summing signed quantities would give zero. Integer sum
// over an unordered map -- addition is associative, so the result does not
// depend on the map's layout.
template <class Book>
Quantity MatchingEngine<Book>::openInterest() const
{
  int64_t oi = 0;
  // order: not observable -- int64 sum of the long legs
  for (const auto& [acct, p] : positions_)
  {
    (void)acct;
    if (p.qtyRaw > 0)
    {
      oi += p.qtyRaw;
    }
  }
  return Quantity::fromRaw(oi);
}

// Next funding boundary, in sequencer time. A schedule set by the operator
// (SetFundingSchedule) is a FACT: it is engine state, hashed, checkpointed
// and advanced by each ApplyFunding, so what the feed publishes is what the
// venue will actually settle on. With no schedule set the value falls back to
// the historical derivation from (now, SymbolConfig::fundingIntervalNs) --
// a computation over startup config, kept so an engine that never learned a
// schedule behaves exactly as it did before the command existed. 0 = neither
// a schedule nor a configured interval, i.e. the venue does not fund this
// instrument.
template <class Book>
SeqNanos MatchingEngine<Book>::nextFundingNs() const noexcept
{
  if (static_cast<bool>(nextFundingNs_))
  {
    return nextFundingNs_;
  }
  if (cfg_.fundingIntervalNs.count() <= 0)
  {
    return SeqNanos{};
  }
  // Boundary alignment is modular math on one domain's raw ticks; the result
  // is restated as the same domain.
  return SeqNanos::fromRaw((now_.raw() / cfg_.fundingIntervalNs.count() + 1) *
                           cfg_.fundingIntervalNs.count());
}

// The live funding interval: the operator-set schedule when there is one,
// otherwise the startup config value it falls back to.
template <class Book>
int64_t MatchingEngine<Book>::fundingIntervalNs() const noexcept
{
  return (fundingIntervalNs_.count() > 0 ? fundingIntervalNs_ : cfg_.fundingIntervalNs).count();
}

// Last funding rate the engine applied, at kFundingRateScale. Carried by the
// checkpoint (RestoreFunding), so a restored engine publishes the real rate
// instead of 0 until the next ApplyFunding.
template <class Book>
int64_t MatchingEngine<Book>::fundingRateRaw() const noexcept
{
  return fundingRateRaw_;
}

// Operator-set funding calendar. Sequenced as SetFundingSchedule in
// production (journaled, replayed, checkpointed); this direct setter is the
// pre-start wiring / recovery path, like setStpGroup. A non-positive interval
// or boundary clears the schedule, dropping back to the config derivation.
template <class Book>
void MatchingEngine<Book>::setFundingSchedule(DurationNs intervalNs, SeqNanos nextFundingNs)
{
  fundingIntervalNs_ = intervalNs.count() > 0 ? intervalNs : DurationNs{};
  nextFundingNs_ = nextFundingNs.raw() > 0 ? nextFundingNs : SeqNanos{};
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

// Apply a funding payment (perps): each position transfers |notional|*rate
// to/from the clearing pool -- longs pay when rate > 0. `mark` values the leg.
template <class Book>
void MatchingEngine<Book>::applyFunding(double rate, Price mark)
{
  // The rate is known whether or not there is a ledger to settle against, and
  // it is what the derivatives feed publishes -- record it before the early
  // return, or a venue running without a bound ledger would never publish one.
  fundingRateRaw_ = static_cast<int64_t>(rate * static_cast<double>(kFundingRateScale));
  advanceFundingSchedule();
  if (ledger_ == nullptr)
  {
    publishDerivatives(mark);
    return;
  }
  // order: not observable -- each account's funding payment is an
  // independent integer credit; the pool accumulates by addition
  for (auto& [acct, p] : positions_)
  {
    const Amount notional =
        notionalRaw(mark.raw(), iabs64(p.qtyRaw), cfg_.priceScale, cfg_.qtyScale);
    const Amount mag = static_cast<Amount>(static_cast<double>(notional) * rate);
    const Amount signedPay = (p.qtyRaw > 0) ? -mag : mag;  // long pays when rate>0
    ledger_->credit(acct, cfg_.quoteAsset, signedPay);
    ledger_->credit(venueAccount_, cfg_.quoteAsset, -signedPay);
  }
  // Funding is charged to the wallet (`available`); when a max-leverage payer
  // has no free collateral, that drives `available` negative, which the
  // wallet-drag term in checkLiquidations now counts against maintenance
  // equity -- so an unaffordable funding payment triggers liquidation instead
  // of accruing silent bad debt.
  checkLiquidations(mark);
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
  checkLiquidations(mark);
  // After the consequences, so the published open interest matches the state
  // the mark left behind (a liquidation the mark caused has already closed
  // its position).
  publishDerivatives(mark);
}

// Unrealized PnL of an account's perp position marked at `mark` (quote raw).
template <class Book>
Amount MatchingEngine<Book>::unrealizedPnlRaw(uint64_t account, Price mark) const
{
  auto it = positions_.find(account);
  if (it == positions_.end())
  {
    return 0;
  }
  const int64_t sign = it->second.qtyRaw > 0 ? 1 : -1;
  return notionalRaw(mark.raw() - it->second.entryRaw, iabs64(it->second.qtyRaw),
                     cfg_.priceScale, cfg_.qtyScale) *
         sign;
}

// Past the window the older half goes and the newer takes its place. Done
// on touch rather than on a timer: the engine has no timer, and an account
// nobody is trading does not need its window rolled.
template <class Book>
void MatchingEngine<Book>::rotateClOrdIds(ClOrdIdWindow& w, int64_t nowNs) const
{
  if (cfg_.clOrdIdWindowNs <= 0)
  {
    return;  // unbounded: what it always did
  }
  if (w.rotatedAtNs == 0)
  {
    w.rotatedAtNs = nowNs;
    return;
  }
  if (nowNs - w.rotatedAtNs < cfg_.clOrdIdWindowNs)
  {
    return;
  }
  w.prev = std::move(w.cur);
  w.cur.clear();
  w.rotatedAtNs = nowNs;
}

template <class Book>
int64_t MatchingEngine<Book>::iabs64(int64_t v)
{
  return v < 0 ? -v : v;
}

// Move reserved IM for `qtyRaw` from the order reservation to position margin
// (stays reserved in the ledger; returns the amount).
template <class Book>
Amount MatchingEngine<Book>::consumeOrderIM(OrderId orderId, int64_t qtyRaw)
{
  auto it = reserve_.find(orderId);
  if (it == reserve_.end())
  {
    return 0;
  }
  Amount im = imForRaw(qtyRaw, it->second.limitPriceRaw);
  if (im > it->second.reservedRaw)
  {
    im = it->second.reservedRaw;
  }
  it->second.reservedRaw -= im;
  return im;
}

// A reducing order's own reserved IM was not needed -> release it to available.
template <class Book>
void MatchingEngine<Book>::releaseOrderIM(OrderId orderId, int64_t qtyRaw, uint64_t acct)
{
  auto it = reserve_.find(orderId);
  if (it == reserve_.end())
  {
    return;
  }
  Amount im = imForRaw(qtyRaw, it->second.limitPriceRaw);
  if (im > it->second.reservedRaw)
  {
    im = it->second.reservedRaw;
  }
  it->second.reservedRaw -= im;
  if (im > 0)
  {
    ledger_->release(acct, cfg_.quoteAsset, im);
  }
}

// An operator correction. Not a trade: see the note on AdjustPosition for
// why no PnL is realized, no fee charged and no margin moved. All this does
// is make the engine's idea of the position match the one being reconciled
// against, and say so loudly enough that a reader a week later can tell a
// correction from a fill.
template <class Book>
void MatchingEngine<Book>::onAdjustPosition(const AdjustPosition& a)
{
  if (a.qtyDeltaRaw == 0 && a.entryRaw == 0)
  {
    sink_(OrderRejected{0, cfg_.id, RejectReason::AdjustmentEmpty, a.accountId, 0});
    return;
  }
  auto it = positions_.find(a.accountId);
  if (it == positions_.end() && a.entryRaw == 0)
  {
    // Opening a position with no entry price would leave every later PnL
    // computed against zero. Refuse rather than book a number that is wrong
    // in a way nothing downstream can detect.
    sink_(OrderRejected{0, cfg_.id, RejectReason::AdjustmentNeedsEntry, a.accountId, 0});
    return;
  }
  Position& p = positions_[a.accountId];
  p.qtyRaw += a.qtyDeltaRaw;
  if (a.entryRaw != 0)
  {
    p.entryRaw = a.entryRaw;
  }
  if (p.qtyRaw == 0)
  {
    // Flat is flat: an entry price left behind on a zero position is a
    // number that means nothing and reads like it means something.
    p.entryRaw = 0;
  }
  PositionAdjusted ev{};
  ev.account = a.accountId;
  ev.symbol = cfg_.id;
  ev.qtyDeltaRaw = a.qtyDeltaRaw;
  ev.qtyAfterRaw = p.qtyRaw;
  ev.entryAfterRaw = p.entryRaw;
  ev.reason = a.reason;
  std::memcpy(ev.note, a.note, kAdjustNoteLen);
  sink_(ev);
}

template <class Book>
void MatchingEngine<Book>::updatePerpPosition(uint64_t acct, OrderId orderId, bool fillBuy, int64_t qtyRaw,
                                              int64_t priceRaw)
{
  Position& p = positions_[acct];
  const int64_t fillSign = fillBuy ? 1 : -1;
  int64_t remaining = qtyRaw;

  const int64_t posSign = (p.qtyRaw > 0) ? 1 : (p.qtyRaw < 0 ? -1 : 0);
  if (posSign != 0 && posSign != fillSign)
  {
    const int64_t reduceQty = std::min<int64_t>(remaining, iabs64(p.qtyRaw));
    // realized PnL vs entry (long: (price-entry)*qty; short: (entry-price)*qty)
    if (ledger_ != nullptr)
    {
      const Amount pnl =
          notionalRaw(priceRaw - p.entryRaw, reduceQty, cfg_.priceScale, cfg_.qtyScale) * posSign;
      ledger_->credit(acct, cfg_.quoteAsset, pnl);
      ledger_->credit(venueAccount_, cfg_.quoteAsset, -pnl);
      // release position margin for the reduced portion
      const Amount relMargin =
          static_cast<Amount>(static_cast<__int128>(p.margin) * reduceQty / iabs64(p.qtyRaw));
      ledger_->release(acct, cfg_.quoteAsset, relMargin);
      p.margin -= relMargin;
      releaseOrderIM(orderId, reduceQty, acct);
    }
    p.qtyRaw += fillSign * reduceQty;  // toward zero
    remaining -= reduceQty;
    if (p.qtyRaw == 0)
    {
      p.entryRaw = 0;
    }
  }

  if (remaining > 0)
  {
    const Amount im = (ledger_ != nullptr) ? consumeOrderIM(orderId, remaining) : 0;
    const int64_t absOld = iabs64(p.qtyRaw);
    const __int128 num = static_cast<__int128>(absOld) * p.entryRaw +
                         static_cast<__int128>(remaining) * priceRaw;
    p.entryRaw = static_cast<int64_t>(num / (absOld + remaining));
    p.qtyRaw += fillSign * remaining;
    p.margin += im;
  }

  if (p.qtyRaw == 0)
  {
    if (p.margin > 0 && ledger_ != nullptr)
    {
      ledger_->release(acct, cfg_.quoteAsset, p.margin);
    }
    p.margin = 0;
    positions_.erase(acct);
  }
}

template <class Book>
void MatchingEngine<Book>::settlePerp(const Trade& t)
{
  const bool takerBuys = (t.takerSide == Side::BUY);
  const OrderId buyerId = takerBuys ? t.takerId : t.makerId;
  const uint64_t buyerAcct = takerBuys ? t.takerAccount : t.makerAccount;
  const OrderId sellerId = takerBuys ? t.makerId : t.takerId;
  const uint64_t sellerAcct = takerBuys ? t.makerAccount : t.takerAccount;
  updatePerpPosition(buyerAcct, buyerId, true, t.quantity.raw(), t.price.raw());
  updatePerpPosition(sellerAcct, sellerId, false, t.quantity.raw(), t.price.raw());
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
  if (positions_.find(fc.accountId) == positions_.end())
  {
    return;  // nothing open: a no-op, not an error
  }
  forceClose(fc.accountId, markPrice_);
}

// Maintenance-margin sweep: liquidate every position whose equity (posted
// margin + unrealized PnL) has fallen below the maintenance requirement.
// Skipped entirely when an external risk owner drives liquidation: two
// systems closing the same position from different numbers is worse than
// either doing it alone.
template <class Book>
void MatchingEngine<Book>::checkLiquidations(Price mark)
{
  if (cfg_.externalLiquidation)
  {
    return;
  }
  if (ledger_ == nullptr || !cfg_.linearPerp || cfg_.maintenanceMarginBps == 0)
  {
    return;
  }
  std::vector<uint64_t> toLiq;
  // order: the breaching accounts are id-sorted below, before forceClose
  // emits the first Liquidation
  for (const auto& [acct, p] : positions_)
  {
    const Amount uPnl = unrealizedPnlRaw(acct, mark);
    const Amount notional =
        notionalRaw(mark.raw(), iabs64(p.qtyRaw), cfg_.priceScale, cfg_.qtyScale);
    const Amount mmReq = notional * cfg_.maintenanceMarginBps / 10000;
    // A negative wallet (funding/fees charged to `available` with no free
    // collateral to absorb them) drags the maintenance-equity check: otherwise
    // funding could push a max-leverage payer's wallet unboundedly negative
    // with no liquidation (silent bad debt). A healthy (>=0) wallet stays
    // isolated from the position, so positive balances never prevent an
    // otherwise-due liquidation -- the drag only ever tightens the check.
    const Amount wallet = ledger_->available(acct, cfg_.quoteAsset);
    const Amount walletDrag = wallet < 0 ? wallet : 0;
    if (p.margin + uPnl + walletDrag < mmReq)
    {
      toLiq.push_back(acct);
    }
  }
  // Deterministic order: forceClose emits Liquidation events (folded into the
  // determinism hash) and, with ADL on, shared counterparties make the close
  // order matter -- it must not depend on positions_ (unordered_map) layout.
  std::sort(toLiq.begin(), toLiq.end());
  for (uint64_t a : toLiq)
  {
    forceClose(a, mark);
  }
}

// Force-close a position at the mark price: realize PnL through the clearing
// pool, return posted margin, and let the insurance fund (venue account) cover
// any negative-equity (bankruptcy) deficit.
template <class Book>
void MatchingEngine<Book>::forceClose(uint64_t acct, Price mark)
{
  auto it = positions_.find(acct);
  if (it == positions_.end())
  {
    return;
  }
  const Position p = it->second;
  positions_.erase(it);
  // Cancel the account's other resting orders first: their initial margin is
  // locked in `reserved` and is the account's own collateral. Freeing it back
  // to `available` before the bankruptcy test below ensures a total-equity-
  // solvent account covers its own shortfall instead of the insurance fund
  // paying out to it (a mis-socialization invisible to conservation-of-total).
  cancelAllForAccount(acct, CancelReason::Liquidation);
  const int64_t sign = p.qtyRaw > 0 ? 1 : -1;
  const int64_t qtyAbs = iabs64(p.qtyRaw);
  const Amount uPnl =
      notionalRaw(mark.raw() - p.entryRaw, qtyAbs, cfg_.priceScale, cfg_.qtyScale) * sign;
  ledger_->credit(acct, cfg_.quoteAsset, uPnl);
  ledger_->credit(venueAccount_, cfg_.quoteAsset, -uPnl);
  if (p.margin > 0)
  {
    ledger_->release(acct, cfg_.quoteAsset, p.margin);
  }
  bool bankrupt = false;
  const Amount avail = ledger_->available(acct, cfg_.quoteAsset);
  Amount deficit = 0;
  if (avail < 0)
  {
    bankrupt = true;
    deficit = -avail;
    ledger_->credit(acct, cfg_.quoteAsset, -avail);  // insurance fund tops up to zero
    ledger_->credit(venueAccount_, cfg_.quoteAsset, avail);
  }
  sink_(Liquidation{acct, cfg_.id, Quantity::fromRaw(qtyAbs), mark, bankrupt});
  if (bankrupt && cfg_.autoDeleverage)
  {
    autoDeleverageEngine(sign, deficit, mark);  // claw the deficit back from winners
  }
}

// Auto-deleverage the isolated-margin book: recover a bankruptcy deficit from
// the most profitable opposite-side positions (close at mark, haircut their
// gain into the insurance fund) instead of socializing it. Same model as the
// portfolio path (cross_margin.h); every ledger op stays balanced.
template <class Book>
void MatchingEngine<Book>::autoDeleverageEngine(int64_t bankruptSign, Amount deficit, Price mark)
{
  if (deficit <= 0)
  {
    return;
  }
  struct Cand
  {
    uint64_t acct;
    Amount uPnl;
  };
  std::vector<Cand> cands;
  // order: candidates are ranked below by (uPnl, acct), a total order, so
  // the ADL victim does not depend on this traversal
  for (const auto& [oa, pos] : positions_)
  {
    const int64_t s = pos.qtyRaw > 0 ? 1 : (pos.qtyRaw < 0 ? -1 : 0);
    if (s == 0 || s == bankruptSign)
    {
      continue;  // opposite side only
    }
    const Amount up =
        notionalRaw(mark.raw() - pos.entryRaw, pos.qtyRaw, cfg_.priceScale, cfg_.qtyScale);
    if (up > 0)
    {
      cands.push_back({oa, up});
    }
  }
  // Most profitable first; `acct` breaks ties deterministically so the chosen
  // ADL victim (an emitted Liquidation, folded into the determinism hash) does
  // not depend on positions_ iteration order or std::sort's instability.
  std::sort(cands.begin(), cands.end(),
            [](const Cand& x, const Cand& y)
            { return x.uPnl != y.uPnl ? x.uPnl > y.uPnl : x.acct < y.acct; });

  Amount remaining = deficit;
  for (const auto& c : cands)
  {
    if (remaining <= 0)
    {
      break;
    }
    auto it = positions_.find(c.acct);
    if (it == positions_.end())
    {
      continue;
    }
    const Position p = it->second;
    positions_.erase(it);
    const int64_t qtyAbs = iabs64(p.qtyRaw);
    const Amount uPnl =
        notionalRaw(mark.raw() - p.entryRaw, p.qtyRaw, cfg_.priceScale, cfg_.qtyScale);
    ledger_->credit(c.acct, cfg_.quoteAsset, uPnl);  // realize gain at mark
    ledger_->credit(venueAccount_, cfg_.quoteAsset, -uPnl);
    if (p.margin > 0)
    {
      ledger_->release(c.acct, cfg_.quoteAsset, p.margin);
    }
    const Amount haircut = remaining < uPnl ? remaining : uPnl;
    // Debit is all-or-nothing and `available` may be below the haircut, so
    // credit the venue only what is actually confiscated -- otherwise the
    // unconditional credit against a no-op debit mints money. Uncovered
    // remainder falls to the insurance fund (normal ADL waterfall).
    const Amount avail = ledger_->available(c.acct, cfg_.quoteAsset);
    const Amount taken = std::min<Amount>(haircut, avail > 0 ? avail : 0);
    if (taken > 0)
    {
      ledger_->debit(c.acct, cfg_.quoteAsset, taken);
      ledger_->credit(venueAccount_, cfg_.quoteAsset, taken);
    }
    remaining -= taken;
    sink_(Liquidation{c.acct, cfg_.id, Quantity::fromRaw(qtyAbs), mark, /*bankrupt*/ false,
                      /*adl*/ true});
  }
}

}  // namespace flox::venue
