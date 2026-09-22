/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Inventory of MatchingEngine's public surface, checked at compile time.
 *
 * matching_engine.h is about to be taken apart. The golden replay says the
 * BEHAVIOUR did not change; this says the SHAPE did not -- every method the
 * tests and the shard call still exists, on MatchingEngine, with the same
 * return type, the same parameter types and the same const qualification.
 *
 * Why a separate check when the tests already call these methods: a rename
 * that moves a method to a helper and leaves a forwarding member of a
 * different shape (Price instead of int64_t, non-const instead of const, a
 * reference instead of a value) compiles in most callers and changes what a
 * caller outside this repo gets. The list is the contract, written down.
 *
 * Deliberately NOT checked: noexcept. Declaring a signature without it still
 * accepts a noexcept member (the pointer converts), so adding or removing
 * noexcept passes -- it is not a rename and not a shape change.
 *
 * Adding a method needs no change here. REMOVING or RESHAPING one does, and
 * that is the point: the diff has to say so out loud.
 */
#pragma once

#include "flox-venue/journal.h"
#include "flox-venue/ledger.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/messages.h"

#include "flox/clearing/fee_schedule.h"

#include <cstdint>
#include <unordered_map>

namespace flox::venue::surface
{

using Engine = MatchingEngine<MatchingBook>;

// Explicit target type, so an overload set resolves against it and nothing
// but an exact member of that shape is accepted.
template <class Sig>
constexpr bool has(Sig) noexcept
{
  return true;
}

// The name comes first so the signature -- which may hold a comma inside a
// template argument list -- can be variadic and arrive intact.
#define FLOX_VENUE_HAS(NAME, ...)                \
  static_assert(has<__VA_ARGS__>(&Engine::NAME), \
                "MatchingEngine::" #NAME " changed shape or vanished")

// ---- ingestion and time -----------------------------------------------
FLOX_VENUE_HAS(submit, void (Engine::*)(const InboundCommand&));
FLOX_VENUE_HAS(submit, void (Engine::*)(const InboundCommand&, int64_t));
FLOX_VENUE_HAS(submit, void (Engine::*)(const InboundCommand&, SeqNanos));
FLOX_VENUE_HAS(tick, void (Engine::*)(int64_t));
FLOX_VENUE_HAS(engineTime, SeqNanos (Engine::*)() const);
FLOX_VENUE_HAS(engineTimeNs, int64_t (Engine::*)() const);

// ---- book and config --------------------------------------------------
FLOX_VENUE_HAS(book, const MatchingBook& (Engine::*)() const);
FLOX_VENUE_HAS(config, const SymbolConfig& (Engine::*)() const);
FLOX_VENUE_HAS(restingOrderCount, uint64_t (Engine::*)() const);
FLOX_VENUE_HAS(tradesGenerated, uint64_t (Engine::*)() const);

// ---- determinism digests ----------------------------------------------
FLOX_VENUE_HAS(stateHash, uint64_t (Engine::*)() const);
FLOX_VENUE_HAS(configHash, uint64_t (Engine::*)() const);

// ---- checkpoint / recovery --------------------------------------------
FLOX_VENUE_HAS(writeSnapshot, void (Engine::*)(Journal&) const);
FLOX_VENUE_HAS(applySnapshotRecord, bool (Engine::*)(const InboundCommand&, int64_t));
FLOX_VENUE_HAS(cloneForSnapshot, Engine::SnapshotClone (Engine::*)(MatchingBook) const);
FLOX_VENUE_HAS(droppedSnapshotRecords, uint64_t (Engine::*)() const);

// ---- clearing -----------------------------------------------------------
FLOX_VENUE_HAS(setLedger, void (Engine::*)(Ledger*, uint64_t));
FLOX_VENUE_HAS(ledger, Ledger* (Engine::*)() const);
FLOX_VENUE_HAS(venueAccount, uint64_t (Engine::*)() const);
FLOX_VENUE_HAS(setFeeSchedule, void (Engine::*)(flox::FeeSchedule));
FLOX_VENUE_HAS(setCreditCheck, void (Engine::*)(Engine::CreditCheck));
FLOX_VENUE_HAS(unsettledTrades, uint64_t (Engine::*)() const);

// ---- admission, MMP, STP ------------------------------------------------
FLOX_VENUE_HAS(setMmp, void (Engine::*)(uint64_t, Quantity, DurationNs));
FLOX_VENUE_HAS(setStpGroup, void (Engine::*)(uint64_t, uint64_t));
FLOX_VENUE_HAS(setAdmissionProfile, void (Engine::*)(uint64_t, const AdmissionProfile&));
FLOX_VENUE_HAS(admissionProfiles, const std::unordered_map<uint64_t, AdmissionProfile>& (Engine::*)() const);
FLOX_VENUE_HAS(admissionRejects, uint64_t (Engine::*)() const);

// ---- last look ----------------------------------------------------------
FLOX_VENUE_HAS(openHolds, uint64_t (Engine::*)() const);
FLOX_VENUE_HAS(hasHold, bool (Engine::*)(uint64_t) const);
FLOX_VENUE_HAS(lastLookStats, const std::unordered_map<uint64_t, LastLookStats>& (Engine::*)() const);
FLOX_VENUE_HAS(toleranceRejectedHolds, uint64_t (Engine::*)() const);
FLOX_VENUE_HAS(riskRejectedHolds, uint64_t (Engine::*)() const);
FLOX_VENUE_HAS(skippedLastLookProRata, uint64_t (Engine::*)() const);

// ---- derivatives --------------------------------------------------------
FLOX_VENUE_HAS(positionQty, int64_t (Engine::*)(uint64_t) const);
FLOX_VENUE_HAS(positionEntry, Price (Engine::*)(uint64_t) const);
FLOX_VENUE_HAS(totalPositionMargin, Amount (Engine::*)() const);
FLOX_VENUE_HAS(unrealizedPnlRaw, Amount (Engine::*)(uint64_t, Price) const);
FLOX_VENUE_HAS(openInterest, Quantity (Engine::*)() const);
FLOX_VENUE_HAS(setMarkPrice, void (Engine::*)(Price));
FLOX_VENUE_HAS(applyFunding, void (Engine::*)(double, Price));
FLOX_VENUE_HAS(setFundingSchedule, void (Engine::*)(DurationNs, SeqNanos));
FLOX_VENUE_HAS(nextFundingNs, SeqNanos (Engine::*)() const);
FLOX_VENUE_HAS(fundingIntervalNs, int64_t (Engine::*)() const);
FLOX_VENUE_HAS(fundingRateRaw, int64_t (Engine::*)() const);

// ---- session, auction, halt ---------------------------------------------
FLOX_VENUE_HAS(setHalted, void (Engine::*)(bool));
FLOX_VENUE_HAS(haltAndCancelAll, void (Engine::*)());
FLOX_VENUE_HAS(cancelEntireBook, void (Engine::*)(CancelReason));
FLOX_VENUE_HAS(beginPreOpen, void (Engine::*)());
FLOX_VENUE_HAS(openContinuous, void (Engine::*)());
FLOX_VENUE_HAS(resumeWithAuction, void (Engine::*)());
FLOX_VENUE_HAS(onAdmin, void (Engine::*)(AdminAction));
FLOX_VENUE_HAS(closeSession, void (Engine::*)());
FLOX_VENUE_HAS(openSession, void (Engine::*)());
FLOX_VENUE_HAS(delist, void (Engine::*)());
FLOX_VENUE_HAS(relist, void (Engine::*)());
FLOX_VENUE_HAS(delisted, bool (Engine::*)() const);
FLOX_VENUE_HAS(sessionClosed, bool (Engine::*)() const);
FLOX_VENUE_HAS(tradingStatus, TradingStatus (Engine::*)() const);

// ---- risk knobs ----------------------------------------------------------
FLOX_VENUE_HAS(riskLimits, SetRiskLimits (Engine::*)() const);
FLOX_VENUE_HAS(applyRiskLimits, void (Engine::*)(const SetRiskLimits&));
FLOX_VENUE_HAS(setPriceBand, void (Engine::*)(Price, Price));
FLOX_VENUE_HAS(setFatFinger, void (Engine::*)(Quantity, Volume));
FLOX_VENUE_HAS(setPositionLimit, void (Engine::*)(Quantity));
FLOX_VENUE_HAS(setMaxOpenOrders, void (Engine::*)(uint32_t));
FLOX_VENUE_HAS(setMarginBps, void (Engine::*)(int32_t, int32_t));
FLOX_VENUE_HAS(setLuldBps, void (Engine::*)(int32_t));
FLOX_VENUE_HAS(setTriggerRef, void (Engine::*)(TriggerRef));

// ---- account query -------------------------------------------------------
FLOX_VENUE_HAS(snapshotAccount, Engine::AccountSnapshot (Engine::*)(uint64_t) const);

#undef FLOX_VENUE_HAS

// The member templates and the constructor cannot be named by a pointer, so
// they are checked by use instead.
static_assert(requires(const Engine& e) { e.forEachHold([](const auto&) {}); }, "MatchingEngine::forEachHold(Fn) changed shape or vanished");
static_assert(requires(SymbolConfig c, EventSink s) {
  Engine{c, s};
  Engine{c, s, MatchingBook{}};
  Engine{c, s, MatchingBook{}, MatchPolicy::PriceTimeFifo}; }, "MatchingEngine's constructor changed shape");

// The two members the snapshot clone is read through.
static_assert(requires(Engine::SnapshotClone c) {
  { c.engine.get() } -> std::same_as<Engine*>;
  { c.ledger.get() } -> std::same_as<Ledger*>; }, "MatchingEngine::SnapshotClone changed shape");

}  // namespace flox::venue::surface
