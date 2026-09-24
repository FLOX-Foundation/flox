/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

// MatchingEngine<Book>: snapshot restore and the checkpoint helpers.
//
// Included only from flox-venue/matching_engine.h, which declares every member
// defined here. Including it directly gives a fragment with no class to attach
// to, so the include is refused rather than left to fail on the first method.
#ifndef FLOX_VENUE_MATCHING_ENGINE_INL
#error "flox-venue/engine/checkpoint_restore.inl is a fragment of flox-venue/matching_engine.h; include that instead"
#endif

namespace flox::venue
{

// Recovery-only application of one snapshot record. Existing journaled
// record types (config, deposits) route through the normal submit path;
// Restore* records rebuild state directly, WITHOUT a matching pass -- the
// snapshot book is uncrossed by invariant, and a crossing RestoreOrder
// marks the snapshot corrupt. Returns false when the record proves the
// snapshot corrupt (version mismatch, crossed book, un-backable
// reservation, hash mismatch at SnapshotEnd); the caller then discards the
// generation. NEVER wire this to live traffic: submit() drops snapshot tags
// for exactly that reason.
// Returns whether the record was understood. A snapshot record with no
// branch below returns true anyway -- see the assert at the end.
template <class Book>
bool MatchingEngine<Book>::applySnapshotRecord(const InboundCommand& cmd, int64_t tsNs)
{
  if (const auto* b = std::get_if<SnapshotBegin>(&cmd))
  {
    return applySnapshotBegin(*b);
  }
  if (const auto* r = std::get_if<RestoreOrder>(&cmd))
  {
    return applyRestoreOrder(*r);
  }
  if (const auto* r = std::get_if<RestoreStop>(&cmd))
  {
    return applyRestoreStop(*r);
  }
  if (const auto* r = std::get_if<RestoreBalance>(&cmd))
  {
    return applyRestoreBalance(*r);
  }
  if (const auto* r = std::get_if<RestoreHeld>(&cmd))
  {
    return applyRestoreHeld(*r);
  }
  if (const auto* r = std::get_if<RestorePosition>(&cmd))
  {
    return applyRestorePosition(*r);
  }
  if (const auto* r = std::get_if<RestoreReservation>(&cmd))
  {
    return applyRestoreReservation(*r);
  }
  if (const auto* e = std::get_if<SnapshotEnd>(&cmd))
  {
    return applySnapshotEnd(*e);
  }
  if (const std::optional<bool> handled = applyComponentRestore(cmd); handled.has_value())
  {
    return *handled;
  }
  // Anything not named above is a live command replayed from the snapshot's
  // own segment, and goes through the live path -- which is correct, and is
  // also why a NEW snapshot-only record that nobody added a branch for would
  // be handed to submit(), drop out of its chain, and be reported as applied.
  static_assert(std::variant_size_v<InboundCommand> == 37,
                "new InboundCommand alternative: if it is a snapshot-only record, give it a "
                "branch above or in applyComponentRestore -- falling through to submit() "
                "reports it as applied when it was not");
  submit(cmd, tsNs);  // existing record types apply through the live path
  return true;
}

// The two guards a snapshot has to clear before any of it is believed: the
// record format this build reads, and the constructor configuration the
// writer had. Both name the numbers -- the caller discards the generation
// either way, and an operator reading the log should not have to guess
// whether the file is old, new, or damaged.
template <class Book>
bool MatchingEngine<Book>::applySnapshotBegin(const SnapshotBegin& b)
{
  if (b.formatVersion != kSnapshotFormatVersion)
  {
    std::fprintf(stderr,
                 "flox-venue: snapshot rejected for symbol %llu: contents are format version "
                 "%u, this build reads version %u only\n",
                 static_cast<unsigned long long>(cfg_.id),
                 static_cast<unsigned>(b.formatVersion),
                 static_cast<unsigned>(kSnapshotFormatVersion));
    return false;
  }
  // A snapshot written by an engine with other structural parameters
  // (scales, assets, policy, ...) must not restore -- the raw fixed-point
  // state would be silently reinterpreted.
  if (b.configHash != 0 && b.configHash != configHash())
  {
    std::fprintf(stderr,
                 "flox-venue: snapshot rejected for symbol %llu: constructor-config hash "
                 "mismatch (snapshot %016llx, engine %016llx) -- scales/assets/policy differ "
                 "from the writer's\n",
                 static_cast<unsigned long long>(cfg_.id),
                 static_cast<unsigned long long>(b.configHash),
                 static_cast<unsigned long long>(configHash()));
    return false;
  }
  return true;
}

// The records a component owns: handed to whoever keeps that state, with the
// engine adding only the check the component cannot make for itself -- a peg
// spec must reference a restored resting order, a clOrdId batch must fit the
// wire array. std::nullopt means the record is not one of these.
template <class Book>
std::optional<bool> MatchingEngine<Book>::applyComponentRestore(const InboundCommand& cmd)
{
  if (const auto* r = std::get_if<RestoreOrderStp>(&cmd))
  {
    stp_.restore(r->id, static_cast<STPMode>(r->mode));
    return true;
  }
  if (const auto* r = std::get_if<RestorePeg>(&cmd))
  {
    if (!book_.contains(r->id))
    {
      return false;
    }
    pegs_.set(r->id, PegBook::Peg{r->side, r->ref, r->offsetRaw});
    return true;
  }
  if (const auto* r = std::get_if<RestoreMmpCfg>(&cmd))
  {
    mmp_.restoreCfg(r->account, r->qtyLimit, r->windowNs);
    return true;
  }
  if (const auto* r = std::get_if<RestoreMmpFills>(&cmd))
  {
    return mmp_.restoreFills(*r);
  }
  if (const auto* r = std::get_if<RestoreClOrdIds>(&cmd))
  {
    if (r->count > kClOrdIdBatch)
    {
      return false;
    }
    clOrdIds_.restore(r->account, r->generation, r->ids, r->count, r->rotatedAtNs);
    return true;
  }
  if (const auto* r = std::get_if<RestoreFunding>(&cmd))
  {
    clearing_.restoreFunding(*r);
    return true;
  }
  return std::nullopt;
}

// The exact signed split for one (account, asset). It precedes the
// reservation and position records, and it says so: with the balances exact
// the ledger's reserved side is already in place, so those records must NOT
// re-reserve -- they only rebuild the engine-side tables. A snapshot with no
// balance records at all (v1, Deposit totals) leaves the flag false and keeps
// the re-reservation path.
template <class Book>
bool MatchingEngine<Book>::applyRestoreBalance(const RestoreBalance& r)
{
  if (ledger_ != nullptr)
  {
    ledger_->restore(r.account, r.asset, r.availableRaw, r.reservedRaw);
    exactBalanceRestore_ = true;
  }
  return true;
}

template <class Book>
SeqNanos MatchingEngine<Book>::expiryOf(OrderId id) const
{
  return expiry_.expiryOf(id);
}

// Pending conditional orders with their current triggers, sorted by order id
// (the stop book's internal container order is not state: firing and cancel
// are id-deterministic, so the canonical order for hash/serialization is id).
template <class Book>
std::vector<std::pair<NewOrder, Price>> MatchingEngine<Book>::sortedStops() const
{
  std::vector<std::pair<NewOrder, Price>> v;
  stops_.forEachPending([&](const NewOrder& o, Price trigger)
                        { v.emplace_back(o, trigger); });
  std::sort(v.begin(), v.end(),
            [](const auto& a, const auto& b)
            { return a.first.id < b.first.id; });
  return v;
}

template <class Book>
bool MatchingEngine<Book>::applyRestoreOrder(const RestoreOrder& r)
{
  if (book_.contains(r.id) || stops_.contains(r.id))
  {
    return false;
  }
  // Corruption tripwire: a continuous-trading book without last look is
  // uncrossed by invariant, so a crossing restore marks the file corrupt.
  // Two legal exceptions: a pre-open (auction) book accumulates crossed
  // (the AdminCmd{BeginPreOpen} record earlier in the file has put the
  // session in its auction phase by the time orders restore), and a last-look
  // venue can legitimately hold a crossed book -- a rejected hold restores the maker
  // at its original price on top of a residual that rested through it (see
  // restoreMakerHeld). There the SnapshotEnd stateHash remains the
  // corruption check.
  if (!session_.auction() && cfg_.lastLookWindowNs.count() == 0)
  {
    if (r.side == Side::BUY)
    {
      if (auto ba = book_.bestAsk(); ba && !(r.price < *ba))
      {
        return false;
      }
    }
    else
    {
      if (auto bb = book_.bestBid(); bb && !(*bb < r.price))
      {
        return false;
      }
    }
  }
  RestingOrder ro{r.id, r.accountId, r.price, r.leaves, r.side};
  ro.clientOrderId = r.clientOrderId;
  ro.hidden = r.hidden;
  ro.peak = r.peak;
  ro.lastLook = r.lastLook;
  ro.reduceOnly = r.reduceOnly;
  ro.postOnly = r.postOnly;
  ro.cumQty = r.cumQty;  // T058
  // Straight to the tail of its level, NO matching pass: the canonical write
  // order (levels best-first, FIFO within) makes tail-appends reproduce the
  // exact live book layout.
  book_.addResting(r.side, ro);
  trackResting(r.id, r.accountId, STPMode::None);  // set by the RestoreOrderStp that follows
  if (static_cast<bool>(r.expiryNs))
  {
    expiry_.set(r.id, r.expiryNs);
  }
  if (r.ocoGroup > 0)
  {
    oco_.link(r.id, r.ocoGroup);
  }
  // Buying power is NOT re-derived here: the order's exact reservation
  // arrives as its own RestoreReservation record (live amounts are
  // history-dependent -- partial fills, held slices, STP interactions).
  return true;
}

template <class Book>
bool MatchingEngine<Book>::applyRestoreStop(const RestoreStop& r)
{
  if (!isConditional(r.order.type) || stops_.contains(r.order.id) ||
      book_.contains(r.order.id))
  {
    return false;
  }
  if (r.order.ocoGroup > 0)
  {
    oco_.link(r.order.id, r.order.ocoGroup);  // a parked stop keeps its OCO link
  }
  // No processTriggers: a restore is not a market event; an in-the-money
  // stop at write time would already have fired live.
  stops_.add(r.order, r.trigger, r.order.type == OrderType::TRAILING_STOP);
  // The same registration the live admission path makes (onNew): a GTD
  // conditional that never reaches the expiry book never expires at all, so a
  // restore that only rebuilt the stop book left the recovered venue holding
  // a deadline nothing sweeps.
  if (r.order.tif == TimeInForce::GTD && static_cast<bool>(r.order.expiryNs))
  {
    expiry_.set(r.order.id, r.order.expiryNs);
  }
  return true;
}

template <class Book>
bool MatchingEngine<Book>::applyRestoreHeld(const RestoreHeld& r)
{
  if (lastLook_.has(r.heldId) || r.qty.raw() <= 0)
  {
    return false;
  }
  Held h{r.heldId, r.taker, r.takerAccount, r.takerSide, r.maker,
         r.makerAccount, r.price, r.qty, r.deadline};
  h.takerTif = r.takerTif;
  h.takerType = r.takerType;
  h.takerPrice = r.takerPrice;
  h.takerExpiryNs = r.takerExpiryNs;
  h.makerReduceOnly = r.makerReduceOnly;
  h.takerReduceOnly = r.takerReduceOnly;
  h.refAtHoldRaw = r.refAtHoldRaw;
  // The names the submitters gave the two legs. Without them a restored hold
  // resolves under zeroes -- and, because stateHash folds a non-zero id in,
  // the reconstructed state does not hash to what the writer measured and the
  // whole generation is discarded as corrupt.
  h.makerClientOrderId = r.makerClientOrderId;
  h.takerClientOrderId = r.takerClientOrderId;
  h.makerCumQtyAtHold = r.makerCumQtyAtHold;
  h.takerCumQtyAtHold = r.takerCumQtyAtHold;
  lastLook_.insertRestored(h);
  // Tracking follows the recorded live truth rather than being re-derived: a
  // held maker stays tracked even fully off the book (see createHeld), and
  // the flag also carries snapshots written before the matcher's own removals
  // learned to resolve holds first. Idempotent when the maker also rests
  // (RestoreOrder tracked it). The taker is tracked only while resting.
  // Reservations backing the legs arrive as RestoreReservation records --
  // nothing is re-derived here.
  if (r.makerTracked)
  {
    trackResting(h.maker, h.makerAccount, stpOf(h.maker));
  }
  return true;
}

// Restore one buying-power reservation entry EXACTLY as recorded. With
// exact RestoreBalance splits in the snapshot the ledger's reserved side is
// already in place, so only the engine-side table is rebuilt; a v1
// (Deposit-total) snapshot instead moves the amount available -> reserved
// through the ledger, and a total that cannot back its recorded reservation
// marks the snapshot corrupt.
template <class Book>
bool MatchingEngine<Book>::applyRestoreReservation(const RestoreReservation& r)
{
  if (credit_.contains(r.id))
  {
    return false;
  }
  if (ledger_ == nullptr)
  {
    // The snapshot describes a venue that held money; this engine does not.
    // Accepting it would leave the reservation table empty while stateHash
    // folds it in, and the generation would later be discarded as "corrupt"
    // -- a wrong diagnosis of a sound file. Refuse here, where the reason
    // is knowable.
    if (r.reservedRaw > 0)
    {
      std::fprintf(stderr,
                   "venue: snapshot carries reservations but no ledger is bound "
                   "(order %llu) -- restore refused\n",
                   static_cast<unsigned long long>(r.id));
      return false;
    }
    return true;  // nothing reserved: nothing to honour
  }
  if (!exactBalanceRestore_ && r.reservedRaw > 0 &&
      !ledger_->reserve(r.account, r.asset, r.reservedRaw))
  {
    return false;
  }
  credit_.put(r.id, Reservation{r.account, r.asset, r.reservedRaw, r.limitPriceRaw, r.side});
  return true;
}

template <class Book>
bool MatchingEngine<Book>::applyRestorePosition(const RestorePosition& r)
{
  return clearing_.restorePosition(r, exactBalanceRestore_);
}

template <class Book>
bool MatchingEngine<Book>::applySnapshotEnd(const SnapshotEnd& e)
{
  tradeSeq_ = e.tradeSeq;
  lastLook_.setSeq(e.heldSeq);
  timeCounter_ = e.timeCounter;
  now_ = SeqNanos::fromRaw(e.nowNs);  // snapshot wire -> sequencer domain
  lastPrice_ = Price::fromRaw(e.lastPriceRaw);
  hasLast_ = e.hasLast;
  markPrice_ = Price::fromRaw(e.markPriceRaw);
  hasMark_ = e.hasMark;
  session_.restoreHaltUntil(SeqNanos::fromRaw(e.haltUntilNs));
  // The restored flags are the state the feed must start from: re-sync the
  // transition memo so the next real transition is measured against the
  // recovered state, not against the one the config records replayed.
  session_.memoRestored(tradingStatus());
  // Full verification: the reconstructed state must hash to what the writer
  // measured. A mismatch (torn/corrupted/semantically-drifted snapshot)
  // rejects the generation.
  return stateHash() == e.stateHash;
}

}  // namespace flox::venue
