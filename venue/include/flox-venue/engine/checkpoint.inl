/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

// MatchingEngine<Book>: snapshot writing and the determinism hash.
//
// Included only from flox-venue/matching_engine.h, which declares every member
// defined here. Including it directly gives a fragment with no class to attach
// to, so the include is refused rather than left to fail on the first method.
#ifndef FLOX_VENUE_MATCHING_ENGINE_INL
#error "flox-venue/engine/checkpoint.inl is a fragment of flox-venue/matching_engine.h; include that instead"
#endif

namespace flox::venue
{

// Deterministic digest of the full engine state over a canonical traversal
// (event_hash-style FNV fold): instrument config and market state, sequence
// counters, book (price levels best-first, FIFO within each level), stops,
// pegs, open holds, positions, MMP config, clientOrderId dedup sets and
// ledger balances (available AND reserved per account x asset). Written into
// SnapshotBegin/SnapshotEnd and re-verified on load: a mismatch marks the
// snapshot corrupt and recovery falls back a generation.
template <class Book>
uint64_t MatchingEngine<Book>::stateHash() const
{
  uint64_t h = 0xcbf29ce484222325ULL;
  h = mix(h, cfg_.id);
  h = mix(h, cfg_.halted ? 1U : 0U);
  h = mix(h, static_cast<uint64_t>(cfg_.minPrice.raw()));
  h = mix(h, static_cast<uint64_t>(cfg_.maxPrice.raw()));
  h = mix(h, static_cast<uint64_t>(cfg_.triggerRef));
  // The session fields fold in their own order, stated once next to them
  // (engine/session.h). They fold only when set, the same "zero == absent"
  // rule the balance traversal and the funding block follow: an engine that
  // has never been closed, never delisted and never seen a funding rate
  // hashes exactly as it did before these fields existed -- which is what
  // lets a snapshot written without them still verify on load.
  h = session_.mixState(h);
  h = clearing_.hashFunding(h);
  h = mix(h, hasLast_ ? 1U : 0U);
  h = mix(h, static_cast<uint64_t>(lastPrice_.raw()));
  h = mix(h, hasMark_ ? 1U : 0U);
  h = mix(h, static_cast<uint64_t>(markPrice_.raw()));
  h = mix(h, tradeSeq_);
  h = mix(h, lastLook_.seq());
  h = mix(h, static_cast<uint64_t>(timeCounter_));
  h = mix(h, static_cast<uint64_t>(now_.raw()));

  // Each component folds its own state, at the point of the traversal the
  // engine folded it at before: the order IS the format, so a section that
  // moved would fail every snapshot already on disk.
  h = hashBookAndStops(h);
  h = credit_.hashAdmission(h);
  h = credit_.hashAccountLimits(h);
  h = stp_.hashInto(h);
  h = pegs_.hashInto(h);
  h = lastLook_.hashInto(h, [this](OrderId id)
                         { return pub_.tracked(id); });
  h = credit_.hashReservations(h);
  h = clearing_.hashPositions(h);
  h = mmp_.hashInto(h);
  h = stp_.hashGroupsInto(matcher_.stpGroups(), h);
  h = clOrdIds_.hashInto(h);
  h = hashBalances(h);
  return h;
}

// Digest of the CONSTRUCTOR configuration -- the structural parameters a
// snapshot cannot restore and recovery cannot verify through stateHash
// alone: fixed-point scales, assets, tick/lot/minQty, last-look window,
// perp mode and the match policy. Written into SnapshotBegin and compared
// on load: restoring raw fixed-point state into an engine constructed with
// different scales (or a different policy) would silently reinterpret every
// number, so a mismatch rejects the snapshot. Mutable knobs (bands, halted,
// triggerRef, LULD, fat-finger, margins, position/order caps) are excluded:
// they are carried by the snapshot itself or owned by the control plane.
template <class Book>
uint64_t MatchingEngine<Book>::configHash() const
{
  uint64_t h = 0xcbf29ce484222325ULL;
  h = mix(h, 0xC0F1U);
  h = mix(h, cfg_.id);
  h = mix(h, static_cast<uint64_t>(cfg_.tickSize.raw()));
  h = mix(h, static_cast<uint64_t>(cfg_.lotSize.raw()));
  h = mix(h, static_cast<uint64_t>(cfg_.minQty.raw()));
  h = mix(h, cfg_.baseAsset);
  h = mix(h, cfg_.quoteAsset);
  h = mix(h, static_cast<uint64_t>(cfg_.lastLookWindowNs.count()));
  h = mix(h, cfg_.lastLookAcceptOnTimeout ? 1U : 0U);
  h = mix(h, cfg_.linearPerp ? 1U : 0U);
  h = mix(h, cfg_.autoDeleverage ? 1U : 0U);
  h = mix(h, static_cast<uint64_t>(cfg_.priceScale));
  h = mix(h, static_cast<uint64_t>(cfg_.qtyScale));
  h = mix(h, static_cast<uint64_t>(matcher_.policy()));
  return h;
}

// Serialize the engine into `out` as a journal-format snapshot: SnapshotBegin
// (with the constructor-config hash), existing config records
// (ListInstrument/SetBands/SetTriggerRef/SetStpGroup/AdminCmd), one
// RestoreBalance per (account, asset) carrying the EXACT signed
// available/reserved split, then the Restore* records in canonical order,
// closed by SnapshotEnd. The canonical traversal (levels by price
// best-first, FIFO within; everything else sorted by key) makes the file
// byte-for-byte deterministic, and tail-appending RestoreOrder application
// reproduces the exact book layout. Because balances restore exactly,
// RestoreReservation / RestorePosition rebuild only the engine-side tables
// on apply (no ledger re-reservation); the records still carry the exact
// live amounts (history-dependent -- partial fills, held slices, STP; a
// formula re-derivation is NOT faithful).
template <class Book>
void MatchingEngine<Book>::writeSnapshot(Journal& out) const
{
  const int64_t ts = now_.raw();  // snapshot records carry raw sequencer ticks
  const uint64_t h = stateHash();
  out.append(InboundCommand{SnapshotBegin{kSnapshotFormatVersion, {}, ts, h, configHash()}}, ts);

  writeConfigSection(out, ts);
  clearing_.writeFunding(out, ts);
  writeBalances(out, ts);
  mmp_.writeSnapshot(out, ts);
  clOrdIds_.writeSnapshot(out, ts);
  writeBookAndStops(out, ts);
  pegs_.writeSnapshot(out, ts);
  stp_.writeSnapshot(out, ts);
  clearing_.writePositions(out, ts);
  lastLook_.writeSnapshot(out, ts, [this](OrderId id)
                          { return pub_.tracked(id); });
  credit_.writeReservations(out, ts);

  SnapshotEnd end{};
  end.stateHash = h;
  end.tradeSeq = tradeSeq_;
  end.heldSeq = lastLook_.seq();
  end.timeCounter = timeCounter_;
  end.nowNs = now_.raw();  // snapshot wire: raw
  end.mdEpoch = 0;         // the engine carries no MD epoch today
  end.lastPriceRaw = lastPrice_.raw();
  end.hasLast = hasLast_;
  end.markPriceRaw = markPrice_.raw();
  end.hasMark = hasMark_;
  end.haltUntilNs = session_.haltUntil().raw();
  out.append(InboundCommand{end}, ts);
}

// ---- the remainder no component owns ------------------------------------
//
// The book, the stop book, the instrument's own config records and the bound
// ledger. A pair of helpers per direction rather than one, because the
// remainder is not contiguous in the file: the config records lead it, the
// book sits in the middle, the balances close it, and the order IS the
// format. Everything else the checkpoint touches belongs to a component in
// engine/*.h and is folded and written by it.

// Resting orders in the book's canonical traversal (price levels best-first,
// FIFO within each level), then the pending conditionals by order id.
template <class Book>
uint64_t MatchingEngine<Book>::hashBookAndStops(uint64_t h) const
{
  book_.forEachOrder(
      [&](const RestingOrder& o)
      {
        h = mix(h, hash_tags::kRestingOrder);
        h = mix(h, o.id);
        h = mix(h, o.accountId);
        h = mix(h, static_cast<uint64_t>(o.price.raw()));
        h = mix(h, static_cast<uint64_t>(o.leaves.raw()));
        h = mix(h, static_cast<uint64_t>(o.hidden.raw()));
        h = mix(h, static_cast<uint64_t>(o.peak.raw()));
        h = mix(h, static_cast<uint64_t>(o.side));
        h = mix(h, o.lastLook ? 1U : 0U);
        h = mix(h, o.reduceOnly ? 1U : 0U);
        if (o.postOnly)
        {
          h = mix(h, hash_tags::kPostOnlyOrder);  // only when set: a book without post-only orders hashes as before
        }
        if (o.clientOrderId != 0)
        {
          h = mix(h, o.clientOrderId);  // same rule: absent means the hash is unchanged
        }
        if (!o.cumQty.isZero())
        {
          // T058: only when a fill has actually landed against this resting
          // order -- an order that has never traded (the overwhelming common
          // case) hashes exactly as it did before cumQty existed.
          h = mix(h, static_cast<uint64_t>(o.cumQty.raw()));
        }
        h = mix(h, static_cast<uint64_t>(expiryOf(o.id).raw()));
        h = mix(h, oco_.groupOf(o.id));
      });

  for (const auto& [o, trig] : sortedStops())
  {
    h = mix(h, hash_tags::kStop);
    h = mix(h, o.id);
    h = mix(h, o.accountId);
    h = mix(h, static_cast<uint64_t>(o.side));
    h = mix(h, static_cast<uint64_t>(o.type));
    h = mix(h, static_cast<uint64_t>(o.price.raw()));
    h = mix(h, static_cast<uint64_t>(o.quantity.raw()));
    h = mix(h, static_cast<uint64_t>(o.tif));
    h = mix(h, static_cast<uint64_t>(o.visibleQuantity.raw()));
    h = mix(h, static_cast<uint64_t>(o.triggerPrice.raw()));
    h = mix(h, static_cast<uint64_t>(o.trailingOffset.raw()));
    h = mix(h, o.lastLook ? 1U : 0U);
    h = mix(h, o.reduceOnly ? 1U : 0U);
    h = mix(h, static_cast<uint64_t>(o.expiryNs.raw()));
    // The deadline the engine actually sweeps, not only the one the record
    // carries: a conditional whose expiry never reached the expiry book never
    // expires, and folding the record's own field alone made that invisible.
    // Only when set, the "zero == absent" rule the rest of the traversal
    // follows -- a book without GTD conditionals hashes as it did before.
    if (const SeqNanos due = expiryOf(o.id); static_cast<bool>(due))
    {
      h = mix(h, static_cast<uint64_t>(due.raw()));
    }
    h = mix(h, o.ocoGroup);
    if (o.clientOrderId != 0)
    {
      h = mix(h, o.clientOrderId);
    }
    h = mix(h, static_cast<uint64_t>(trig.raw()));
  }
  return h;
}

// The bound ledger's live balances, sorted by (account, asset), with a fully
// zero entry dropped -- indistinguishable from absence, which is what lets an
// account that has been drained hash as one that never existed. ONE
// traversal, used by the hash and by the snapshot, so the two cannot drift
// apart. No ledger: no rows.
template <class Book>
auto MatchingEngine<Book>::sortedBalances() const -> std::vector<BalanceRow>
{
  std::vector<BalanceRow> bals;
  if (ledger_ == nullptr)
  {
    return bals;
  }
  ledger_->forEachBalanceSplit(
      [&](uint64_t acct, AssetId asset, Amount avail, Amount rsvd)
      {
        if (avail != 0 || rsvd != 0)
        {
          bals.emplace_back(acct, asset, avail, rsvd);
        }
      });
  // order: sorted below, before the vector is handed out
  std::sort(bals.begin(), bals.end(),
            [](const auto& a, const auto& b)
            {
              return std::get<0>(a) != std::get<0>(b) ? std::get<0>(a) < std::get<0>(b)
                                                      : std::get<1>(a) < std::get<1>(b);
            });
  return bals;
}

// Available AND reserved per (account, asset): the reserved side is engine
// state as much as the available side is, so a snapshot that carried only the
// total could not tell a drained account from a fully committed one.
template <class Book>
uint64_t MatchingEngine<Book>::hashBalances(uint64_t h) const
{
  for (const auto& [acct, asset, avail, rsvd] : sortedBalances())
  {
    h = mix(h, hash_tags::kBalance);
    h = mix(h, acct);
    h = mix(h, asset);
    h = mixAmount(h, avail);
    h = mixAmount(h, rsvd);
  }
  return h;
}

// The config section: records an ordinary journal already carries, which
// restore through the live submit path rather than a Restore* branch. They
// lead the file because they decide what the Restore* records that follow
// are allowed to rebuild.
template <class Book>
void MatchingEngine<Book>::writeConfigSection(Journal& out, int64_t ts) const
{
  out.append(InboundCommand{ListInstrument{cfg_.id, {}, cfg_.tickSize, cfg_.lotSize, cfg_.minPrice, cfg_.maxPrice}},
             ts);
  out.append(InboundCommand{SetBands{cfg_.id, {}, cfg_.minPrice, cfg_.maxPrice}}, ts);
  // Risk limits ride the config section for the same reason the bands do:
  // they decide what is admitted, so a recovered engine that lost them would
  // admit orders the live one refused.
  out.append(InboundCommand{riskLimits()}, ts);
  out.append(InboundCommand{SetTriggerRef{cfg_.id, cfg_.triggerRef}}, ts);
  stp_.writeGroups(matcher_.stpGroups(), cfg_.id, out, ts);
  credit_.writeAdmission(out, cfg_.id, ts);
  // Per-account limits ride the config section for the same reason the
  // symbol's do (W26-T064): a recovered engine without them admits what the
  // live one refused.
  credit_.writeAccountLimits(out, cfg_.id, ts);
  // The halt, the auction phase, the session boundary and delisting all ride
  // the same existing AdminCmd path, in the order engine::Session hands them
  // back -- outermost last, exactly as tradingStatus() ranks them, and only
  // the ones that are set, so an engine that never closed and never delisted
  // writes the file it always did.
  std::array<AdminAction, 4> sessionActions{};
  const size_t nSessionActions = session_.snapshotActions(cfg_.halted, sessionActions);
  for (size_t i = 0; i < nSessionActions; ++i)
  {
    out.append(InboundCommand{AdminCmd{cfg_.id, sessionActions[i]}}, ts);
  }
}

// One RestoreBalance per (account, asset) carrying the EXACT signed split.
// Every live moment is representable, including a negative wallet
// mid-liquidation and non-positive totals -- states the v1 Deposit-total
// encoding could not express, which forced recovery a generation back.
template <class Book>
void MatchingEngine<Book>::writeBalances(Journal& out, int64_t ts) const
{
  for (const auto& [acct, asset, avail, rsvd] : sortedBalances())
  {
    out.append(InboundCommand{RestoreBalance{acct, asset, {}, avail, rsvd}}, ts);
  }
}

// Resting orders in the traversal above, so tail-appending RestoreOrder
// application reproduces the exact book layout, then the pending
// conditionals.
template <class Book>
void MatchingEngine<Book>::writeBookAndStops(Journal& out, int64_t ts) const
{
  book_.forEachOrder(
      [&](const RestingOrder& o)
      {
        RestoreOrder r{o.id, o.accountId, o.price, o.leaves, o.side, {}, o.hidden, o.peak, o.lastLook, o.reduceOnly, o.postOnly, {}, o.clientOrderId};
        r.expiryNs = expiryOf(o.id);
        r.ocoGroup = oco_.groupOf(o.id);
        r.cumQty = o.cumQty;  // T058
        out.append(InboundCommand{r}, ts);
      });

  for (const auto& [o, trig] : sortedStops())
  {
    out.append(InboundCommand{RestoreStop{o, trig}}, ts);
  }
}

// Live-traffic snapshot-tag drops (observability; see the guard in submit).
template <class Book>
uint64_t MatchingEngine<Book>::droppedSnapshotRecords() const noexcept
{
  return integrity_.droppedSnapshotRecords();
}

// `emptyBook` must be a PRISTINE book instance carrying only construction
// config (ladder geometry); the resting orders are re-added canonically
// (levels best-first, FIFO within -- forEachOrder order), which reproduces
// the exact live layout the same way snapshot restore does. A default
// MatchingBook needs no config, so the default argument suffices for it.
template <class Book>
typename MatchingEngine<Book>::SnapshotClone MatchingEngine<Book>::cloneForSnapshot(Book emptyBook) const
{
  SnapshotClone c;
  c.engine = std::make_unique<MatchingEngine>(cfg_, EventSink{[](const OutboundEvent&) {}},
                                              std::move(emptyBook), matcher_.policy());
  MatchingEngine& e = *c.engine;
  book_.forEachOrder([&](const RestingOrder& o)
                     { e.book_.addResting(o.side, o); });
  e.stops_ = stops_;
  e.lastPrice_ = lastPrice_;
  e.hasLast_ = hasLast_;
  e.markPrice_ = markPrice_;
  e.hasMark_ = hasMark_;
  e.tradeSeq_ = tradeSeq_;
  e.now_ = now_;
  e.timeCounter_ = timeCounter_;
  e.pub_.copyStateFrom(pub_);
  e.expiry_ = expiry_;
  e.oco_ = oco_;  // pending is empty at a command boundary; copied for completeness
  e.pegs_ = pegs_;
  e.stp_ = stp_;
  e.credit_.copyStateFrom(credit_);
  e.fees_ = fees_;
  e.mmp_ = mmp_;
  e.lastLook_.copyHoldsFrom(lastLook_);
  e.clOrdIds_ = clOrdIds_;
  e.clearing_.copyStateFrom(clearing_);
  e.session_.copyForSnapshotClone(session_);
  // order: not observable -- a keyed copy into the clone's own map; the
  // resulting (account -> group) mapping is the same set either way
  for (const auto& [acct, grp] : matcher_.stpGroups())
  {
    e.matcher_.setStpGroup(acct, grp);
  }
  if (ledger_ != nullptr)
  {
    c.ledger = std::make_unique<Ledger>(*ledger_);
    e.setLedger(c.ledger.get(), venueAccount_);
  }
  else
  {
    e.setLedger(nullptr, venueAccount_);  // no ledger: carry the venue account across
  }
  return c;
}

template <class Book>
uint64_t MatchingEngine<Book>::mixAmount(uint64_t h, Amount a) noexcept
{
  h = mix(h, static_cast<uint64_t>(static_cast<unsigned __int128>(a)));
  h = mix(h, static_cast<uint64_t>(static_cast<unsigned __int128>(a) >> 64));
  return h;
}

template <class Book>
template <class Map>
std::vector<typename Map::key_type> MatchingEngine<Book>::sortedKeys(const Map& m)
{
  return sortedKeysOf(m);
}

}  // namespace flox::venue
