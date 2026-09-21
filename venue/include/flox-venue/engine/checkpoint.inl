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
  h = mix(h, auctionMode_ ? 1U : 0U);
  if (delisted_)
  {
    h = mix(h, 0xB00EU);  // only when set: an engine that never delisted hashes as before
  }
  h = mix(h, static_cast<uint64_t>(haltUntil_.raw()));
  // Session and funding state fold in only when they are set, the same
  // "zero == absent" rule the balance traversal follows. An engine that has
  // never been closed and never seen a funding rate or schedule therefore
  // hashes exactly as it did before these fields existed -- which is what
  // lets a snapshot written without them still verify on load.
  if (closed_)
  {
    h = mix(h, 0xB00AU);
    h = mix(h, 1U);
  }
  if (fundingRateRaw_ != 0 || fundingIntervalNs_.count() != 0 || nextFundingNs_.raw() != 0)
  {
    h = mix(h, 0xB00BU);
    h = mix(h, static_cast<uint64_t>(fundingRateRaw_));
    h = mix(h, static_cast<uint64_t>(fundingIntervalNs_.count()));
    h = mix(h, static_cast<uint64_t>(nextFundingNs_.raw()));
  }
  h = mix(h, hasLast_ ? 1U : 0U);
  h = mix(h, static_cast<uint64_t>(lastPrice_.raw()));
  h = mix(h, hasMark_ ? 1U : 0U);
  h = mix(h, static_cast<uint64_t>(markPrice_.raw()));
  h = mix(h, tradeSeq_);
  h = mix(h, heldSeq_);
  h = mix(h, static_cast<uint64_t>(timeCounter_));
  h = mix(h, static_cast<uint64_t>(now_.raw()));

  book_.forEachOrder(
      [&](const RestingOrder& o)
      {
        h = mix(h, 0xB001U);
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
          h = mix(h, 0xB00FU);  // only when set: a book without post-only orders hashes as before
        }
        if (o.clientOrderId != 0)
        {
          h = mix(h, o.clientOrderId);  // same rule: absent means the hash is unchanged
        }
        h = mix(h, static_cast<uint64_t>(expiryOf(o.id).raw()));
        h = mix(h, ocoOf(o.id));
      });

  for (const auto& [o, trig] : sortedStops())
  {
    h = mix(h, 0xB002U);
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
    h = mix(h, o.ocoGroup);
    if (o.clientOrderId != 0)
    {
      h = mix(h, o.clientOrderId);
    }
    h = mix(h, static_cast<uint64_t>(trig.raw()));
  }

  for (uint64_t acct : sortedKeys(admission_))
  {
    const AdmissionProfile& p = admission_.at(acct);
    h = mix(h, 0xB00DU);
    h = mix(h, acct);
    h = mix(h, p.allowedTypes);
    h = mix(h, p.allowedTif);
    h = mix(h, static_cast<uint64_t>(p.deny));
  }
  for (OrderId id : sortedKeys(orderStp_))
  {
    h = mix(h, 0xB00CU);
    h = mix(h, static_cast<uint64_t>(id));
    h = mix(h, static_cast<uint64_t>(orderStp_.at(id)));
  }
  for (OrderId id : sortedKeys(pegged_))
  {
    const Peg& p = pegged_.at(id);
    h = mix(h, 0xB003U);
    h = mix(h, id);
    h = mix(h, static_cast<uint64_t>(p.side));
    h = mix(h, static_cast<uint64_t>(p.ref));
    h = mix(h, static_cast<uint64_t>(p.offsetRaw));
  }

  for (uint64_t hid : sortedKeys(held_))
  {
    const Held& x = held_.at(hid);
    h = mix(h, 0xB004U);
    h = mix(h, x.id);
    h = mix(h, x.taker);
    h = mix(h, x.takerAccount);
    h = mix(h, static_cast<uint64_t>(x.takerSide));
    h = mix(h, x.maker);
    h = mix(h, x.makerAccount);
    h = mix(h, static_cast<uint64_t>(x.price.raw()));
    h = mix(h, static_cast<uint64_t>(x.qty.raw()));
    h = mix(h, static_cast<uint64_t>(x.deadline.raw()));
    h = mix(h, static_cast<uint64_t>(x.takerTif));
    h = mix(h, static_cast<uint64_t>(x.takerType));
    h = mix(h, static_cast<uint64_t>(x.takerPrice.raw()));
    h = mix(h, static_cast<uint64_t>(x.takerExpiryNs.raw()));
    h = mix(h, x.makerReduceOnly ? 1U : 0U);
    h = mix(h, x.takerReduceOnly ? 1U : 0U);
    if (x.makerClientOrderId != 0)
    {
      h = mix(h, x.makerClientOrderId);
    }
    if (x.takerClientOrderId != 0)
    {
      h = mix(h, x.takerClientOrderId);
    }
    // Live-tracking truth of the maker (see RestoreHeld::makerTracked).
    h = mix(h, orderAccount_.count(x.maker) != 0 ? 1U : 0U);
  }

  for (OrderId id : sortedKeys(reserve_))
  {
    const Reservation& r = reserve_.at(id);
    h = mix(h, 0xB009U);
    h = mix(h, id);
    h = mix(h, r.account);
    h = mix(h, r.asset);
    h = mix(h, static_cast<uint64_t>(r.side));
    h = mix(h, static_cast<uint64_t>(r.limitPriceRaw));
    h = mixAmount(h, r.reservedRaw);
  }

  for (uint64_t acct : sortedKeys(positions_))
  {
    const Position& p = positions_.at(acct);
    h = mix(h, 0xB005U);
    h = mix(h, acct);
    h = mix(h, static_cast<uint64_t>(p.qtyRaw));
    h = mix(h, static_cast<uint64_t>(p.entryRaw));
    h = mixAmount(h, p.margin);
  }

  for (uint64_t acct : sortedKeys(mmpCfg_))
  {
    const MmpCfg& c = mmpCfg_.at(acct);
    h = mix(h, 0xB006U);
    h = mix(h, acct);
    h = mix(h, static_cast<uint64_t>(c.qtyLimit.raw()));
    h = mix(h, static_cast<uint64_t>(c.windowNs.count()));
  }

  // MMP sliding-window fills, deque (time) order. An EMPTY window contributes
  // nothing, so it is indistinguishable from absence -- which also keeps
  // pre-window-serialization snapshots (that restored windows empty) hashing
  // identically when the windows really were empty.
  for (uint64_t acct : sortedKeys(mmpFills_))
  {
    const MmpWindow& w = mmpFills_.at(acct);
    if (w.fills.empty())
    {
      continue;
    }
    h = mix(h, 0xB00AU);
    h = mix(h, acct);
    for (const auto& [ts, q] : w.fills)
    {
      h = mix(h, static_cast<uint64_t>(ts.raw()));
      h = mix(h, static_cast<uint64_t>(q.raw()));
    }
  }

  // Firm-group STP table (feeds matching decisions, journaled as SetStpGroup).
  if (const auto& groups = matcher_.stpGroups(); !groups.empty())
  {
    for (uint64_t acct : sortedKeys(groups))
    {
      h = mix(h, 0xB00BU);
      h = mix(h, acct);
      h = mix(h, groups.at(acct));
    }
  }

  for (uint64_t acct : sortedKeys(clientOrderIds_))
  {
    h = mix(h, 0xB007U);
    h = mix(h, acct);
    const auto& seen = clientOrderIds_.at(acct);
    // Both halves, in order and each sorted. No separator between them: for
    // every state the engine can actually reach, the concatenation and the
    // rotation moment below already tell two different splits apart, and a
    // marker that no reachable state needs is untested weight.
    for (uint32_t g = 0; g < 2; ++g)
    {
      const auto& gen = g == 0 ? seen.cur : seen.prev;
      std::vector<uint64_t> ids(gen.begin(), gen.end());
      std::sort(ids.begin(), ids.end());
      for (uint64_t id : ids)
      {
        h = mix(h, id);
      }
    }
    h = mix(h, static_cast<uint64_t>(seen.rotatedAtNs));
  }

  if (ledger_ != nullptr)
  {
    std::vector<std::tuple<uint64_t, AssetId, Amount, Amount>> bals;
    ledger_->forEachBalanceSplit(
        [&](uint64_t acct, AssetId asset, Amount avail, Amount rsvd)
        {
          if (avail != 0 || rsvd != 0)  // a zeroed entry is indistinguishable from absence
          {
            bals.emplace_back(acct, asset, avail, rsvd);
          }
        });
    std::sort(bals.begin(), bals.end(),
              [](const auto& a, const auto& b)
              {
                return std::get<0>(a) != std::get<0>(b) ? std::get<0>(a) < std::get<0>(b)
                                                        : std::get<1>(a) < std::get<1>(b);
              });
    for (const auto& [acct, asset, avail, rsvd] : bals)
    {
      h = mix(h, 0xB008U);
      h = mix(h, acct);
      h = mix(h, asset);
      h = mixAmount(h, avail);
      h = mixAmount(h, rsvd);
    }
  }
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
  out.append(InboundCommand{SnapshotBegin{kSnapshotFormatVersion, ts, h, configHash()}}, ts);

  out.append(InboundCommand{ListInstrument{cfg_.id, cfg_.tickSize, cfg_.lotSize, cfg_.minPrice,
                                           cfg_.maxPrice}},
             ts);
  out.append(InboundCommand{SetBands{cfg_.id, cfg_.minPrice, cfg_.maxPrice}}, ts);
  // Risk limits ride the config section for the same reason the bands do:
  // they decide what is admitted, so a recovered engine that lost them would
  // admit orders the live one refused.
  out.append(InboundCommand{riskLimits()}, ts);
  out.append(InboundCommand{SetTriggerRef{cfg_.id, cfg_.triggerRef}}, ts);
  // STP groups are engine state journaled as SetStpGroup commands; the
  // snapshot re-emits the live table as the same records (config section,
  // applied through the ordinary submit path on load).
  if (const auto& groups = matcher_.stpGroups(); !groups.empty())
  {
    for (uint64_t acct : sortedKeys(groups))
    {
      out.append(InboundCommand{SetStpGroup{cfg_.id, acct, groups.at(acct)}}, ts);
    }
  }
  // Admission profiles are engine state of the same kind: they decide what
  // is accepted, so they are re-emitted as the command that set them and
  // applied through the ordinary submit path on load.
  for (uint64_t acct : sortedKeys(admission_))
  {
    out.append(InboundCommand{SetAdmissionProfile{cfg_.id, acct, admission_.at(acct)}}, ts);
  }
  out.append(InboundCommand{AdminCmd{cfg_.id, cfg_.halted ? AdminAction::Halt
                                                          : AdminAction::Resume}},
             ts);
  if (auctionMode_)
  {
    out.append(InboundCommand{AdminCmd{cfg_.id, AdminAction::BeginPreOpen}}, ts);
  }
  // The session state rides the same existing AdminCmd path the halt and the
  // auction phase do. Written last of the three so it restores as the
  // outermost state, exactly as tradingStatus() ranks it.
  if (closed_)
  {
    out.append(InboundCommand{AdminCmd{cfg_.id, AdminAction::CloseSession}}, ts);
  }
  // Delisting is outermost of all, so it is written after the session state.
  // Only when set, so an engine that never delisted writes the file it always
  // did and its state hash is unchanged.
  if (delisted_)
  {
    out.append(InboundCommand{AdminCmd{cfg_.id, AdminAction::Delist}}, ts);
  }
  // Funding state: written only when there is any, so an engine with none
  // produces the same file it did before the record existed (and that file
  // still loads -- see RestoreFunding).
  if (fundingRateRaw_ != 0 || fundingIntervalNs_.count() != 0 || nextFundingNs_.raw() != 0)
  {
    out.append(InboundCommand{RestoreFunding{fundingRateRaw_, nextFundingNs_, fundingIntervalNs_}},
               ts);
  }

  if (ledger_ != nullptr)
  {
    // Exact signed split per (account, asset): every live moment is
    // representable, including a negative wallet mid-liquidation and
    // non-positive totals (states the v1 Deposit-total encoding could not
    // express, forcing recovery a generation back). A fully zero entry is
    // skipped -- indistinguishable from absence, matching stateHash.
    std::vector<std::tuple<uint64_t, AssetId, Amount, Amount>> bals;
    ledger_->forEachBalanceSplit(
        [&](uint64_t acct, AssetId asset, Amount avail, Amount rsvd)
        {
          if (avail != 0 || rsvd != 0)
          {
            bals.emplace_back(acct, asset, avail, rsvd);
          }
        });
    std::sort(bals.begin(), bals.end(),
              [](const auto& a, const auto& b)
              {
                return std::get<0>(a) != std::get<0>(b) ? std::get<0>(a) < std::get<0>(b)
                                                        : std::get<1>(a) < std::get<1>(b);
              });
    for (const auto& [acct, asset, avail, rsvd] : bals)
    {
      out.append(InboundCommand{RestoreBalance{acct, asset, avail, rsvd}}, ts);
    }
  }

  for (uint64_t acct : sortedKeys(mmpCfg_))
  {
    const MmpCfg& c = mmpCfg_.at(acct);
    out.append(InboundCommand{RestoreMmpCfg{acct, c.qtyLimit, c.windowNs}}, ts);
  }

  // MMP sliding-window fills, exact, in deque (time) order -- a maker one
  // fill from its limit stays one fill from it across recovery.
  for (uint64_t acct : sortedKeys(mmpFills_))
  {
    const MmpWindow& w = mmpFills_.at(acct);
    if (w.fills.empty())
    {
      continue;
    }
    RestoreMmpFills batch{};
    batch.account = acct;
    for (const auto& [fts, q] : w.fills)
    {
      batch.tsNs[batch.count] = fts.raw();  // wire batch: raw ticks
      batch.qtyRaw[batch.count] = q.raw();
      if (++batch.count == kMmpFillBatch)
      {
        out.append(InboundCommand{batch}, ts);
        batch = RestoreMmpFills{};
        batch.account = acct;
      }
    }
    if (batch.count > 0)
    {
      out.append(InboundCommand{batch}, ts);
    }
  }

  for (uint64_t acct : sortedKeys(clientOrderIds_))
  {
    const auto& seen = clientOrderIds_.at(acct);
    for (uint32_t g = 0; g < 2; ++g)
    {
      const auto& gen = g == 0 ? seen.cur : seen.prev;
      std::vector<uint64_t> ids(gen.begin(), gen.end());
      std::sort(ids.begin(), ids.end());
      RestoreClOrdIds batch{};
      batch.account = acct;
      batch.generation = g;
      for (uint64_t id : ids)
      {
        batch.ids[batch.count++] = id;
        if (batch.count == kClOrdIdBatch)
        {
          out.append(InboundCommand{batch}, ts);
          batch = RestoreClOrdIds{};
          batch.account = acct;
          batch.generation = g;
        }
      }
      if (batch.count > 0)
      {
        out.append(InboundCommand{batch}, ts);
      }
    }
  }

  book_.forEachOrder(
      [&](const RestingOrder& o)
      {
        RestoreOrder r{o.id, o.accountId, o.price, o.leaves,
                       o.side, o.hidden, o.peak, o.lastLook,
                       o.reduceOnly, o.postOnly, o.clientOrderId};
        r.expiryNs = expiryOf(o.id);
        r.ocoGroup = ocoOf(o.id);
        out.append(InboundCommand{r}, ts);
      });

  for (const auto& [o, trig] : sortedStops())
  {
    out.append(InboundCommand{RestoreStop{o, trig}}, ts);
  }

  for (OrderId id : sortedKeys(pegged_))
  {
    const Peg& p = pegged_.at(id);
    out.append(InboundCommand{RestorePeg{id, p.side, p.ref, p.offsetRaw}}, ts);
  }

  for (OrderId id : sortedKeys(orderStp_))
  {
    out.append(InboundCommand{RestoreOrderStp{id, static_cast<uint8_t>(orderStp_.at(id))}}, ts);
  }

  for (uint64_t acct : sortedKeys(positions_))
  {
    const Position& p = positions_.at(acct);
    out.append(InboundCommand{RestorePosition{acct, p.qtyRaw, p.entryRaw, p.margin}}, ts);
  }

  for (uint64_t hid : sortedKeys(held_))
  {
    const Held& x = held_.at(hid);
    RestoreHeld r{x.id, x.taker, x.takerAccount, x.takerSide, x.maker,
                  x.makerAccount, x.price, x.qty, x.deadline, x.takerTif,
                  x.takerType, x.takerPrice, x.takerExpiryNs, x.makerReduceOnly,
                  x.takerReduceOnly};
    r.makerClientOrderId = x.makerClientOrderId;
    r.takerClientOrderId = x.takerClientOrderId;
    r.makerTracked = orderAccount_.count(x.maker) != 0;
    r.refAtHoldRaw = x.refAtHoldRaw;
    out.append(InboundCommand{r}, ts);
  }

  for (OrderId id : sortedKeys(reserve_))
  {
    const Reservation& r = reserve_.at(id);
    out.append(
        InboundCommand{RestoreReservation{id, r.account, r.asset, r.side, r.limitPriceRaw,
                                          r.reservedRaw}},
        ts);
  }

  SnapshotEnd end{};
  end.stateHash = h;
  end.tradeSeq = tradeSeq_;
  end.heldSeq = heldSeq_;
  end.timeCounter = timeCounter_;
  end.nowNs = now_.raw();  // snapshot wire: raw
  end.mdEpoch = 0;         // the engine carries no MD epoch today
  end.lastPriceRaw = lastPrice_.raw();
  end.hasLast = hasLast_;
  end.markPriceRaw = markPrice_.raw();
  end.hasMark = hasMark_;
  end.haltUntilNs = haltUntil_.raw();
  out.append(InboundCommand{end}, ts);
}

// Live-traffic snapshot-tag drops (observability; see the guard in submit).
template <class Book>
uint64_t MatchingEngine<Book>::droppedSnapshotRecords() const noexcept
{
  return droppedSnapshotRecords_;
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
  e.orderAccount_ = orderAccount_;
  e.byAccount_ = byAccount_;
  e.expiry_ = expiry_;
  e.orderOco_ = orderOco_;
  e.ocoMembers_ = ocoMembers_;
  e.ocoPending_ = ocoPending_;  // empty at a command boundary; copied for completeness
  e.pegged_ = pegged_;
  e.orderStp_ = orderStp_;
  e.admission_ = admission_;
  e.fees_ = fees_;
  e.feesEnabled_ = feesEnabled_;
  e.mmpCfg_ = mmpCfg_;
  e.mmpFills_ = mmpFills_;
  e.mmpBreached_ = mmpBreached_;
  e.held_ = held_;
  e.heldSeq_ = heldSeq_;
  e.heldOpen_.store(e.held_.size(), std::memory_order_relaxed);
  e.clientOrderIds_ = clientOrderIds_;
  e.reserve_ = reserve_;
  e.positions_ = positions_;
  e.auctionMode_ = auctionMode_;
  e.haltUntil_ = haltUntil_;
  e.closed_ = closed_;
  e.lastStatus_ = lastStatus_;
  e.lastStatusUntil_ = lastStatusUntil_;
  e.statusPublished_ = statusPublished_;
  e.fundingRateRaw_ = fundingRateRaw_;
  e.fundingIntervalNs_ = fundingIntervalNs_;
  e.nextFundingNs_ = nextFundingNs_;
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
    e.venueAccount_ = venueAccount_;
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
  std::vector<typename Map::key_type> keys;
  keys.reserve(m.size());
  for (const auto& [k, v] : m)
  {
    (void)v;
    keys.push_back(k);
  }
  std::sort(keys.begin(), keys.end());
  return keys;
}

}  // namespace flox::venue
