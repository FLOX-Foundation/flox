/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox/common.h"  // Price, Quantity, Side, OrderId, OrderType, TimeInForce, STPMode, SymbolId

#include "flox-venue/ledger.h"  // AssetId, kMoneyScale (Deposit/Withdraw amounts)
#include "flox-venue/reject_reason.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <variant>

namespace flox::venue
{

// ---- Inbound commands -----------------------------------------------------

// Reference price a conditional order (stop / take-profit / trailing) triggers
// against. Spot: last trade price. Derivatives: usually the mark price.
enum class TriggerRef : uint8_t
{
  Last = 0,
  Mark = 1,
};

// Peg reference: a resting order tracks the book, re-priced at each submit
// boundary. Bid/Ask peg to the near or far touch; Mid to the midpoint.
enum class PegRef : uint8_t
{
  None = 0,
  Bid,
  Ask,
  Mid,
};

struct NewOrder
{
  OrderId id{};
  SymbolId symbol{};
  Side side{};
  OrderType type{OrderType::LIMIT};  // LIMIT | MARKET and the conditional (stop / take-profit / trailing) types
  Price price{};                     // ignored for MARKET
  Quantity quantity{};
  TimeInForce tif{TimeInForce::GTC};
  bool postOnly{false};
  STPMode stp{STPMode::None};
  uint64_t accountId{0};
  uint64_t clientOrderId{0};
  Quantity visibleQuantity{};  // iceberg display size; 0 or >= quantity = fully visible
  Price triggerPrice{};        // stop / take-profit activation price
  Price trailingOffset{};      // trailing-stop offset (price distance from extreme)
  bool lastLook{false};        // maker holds a fill for a last-look window before confirming
  bool reduceOnly{false};      // derivatives: may only reduce/close a position, never increase
  int64_t expiryNs{0};         // GTD: sequencer-ts at/after which a resting order auto-cancels (0 = none)
  uint64_t ocoGroup{0};        // OCO: orders sharing a group cancel each other on the first fill (0 = none)
  PegRef peg{PegRef::None};    // peg: re-price to track Bid/Ask/Mid each submit boundary
  int64_t pegOffsetRaw{0};     // signed price offset from the peg reference (raw ticks)
};

struct CancelOrder
{
  OrderId id{};
  SymbolId symbol{};
  uint64_t accountId{0};
};

struct ModifyOrder  // cancel/replace
{
  OrderId id{};
  SymbolId symbol{};
  Price newPrice{};   // raw 0 = keep current price
  Quantity newQty{};  // new leaves target
  uint64_t accountId{0};
};

struct MassCancel  // cancel every resting order of an account (MM cancel-all)
{
  uint64_t accountId{};
  SymbolId symbol{};
};

struct Quote  // two-sided market-maker quote (replace prior quote on this symbol)
{
  OrderId bidId{};
  OrderId askId{};
  SymbolId symbol{};
  Price bidPrice{};
  Quantity bidQty{};  // 0 = no bid side
  Price askPrice{};
  Quantity askQty{};  // 0 = no ask side
  uint64_t accountId{};
  // A quote is two orders, and every other order-bearing command carries its
  // own self-trade-prevention mode. Without it here, the one participant that
  // most needs the control -- a maker quoting both sides continuously -- was
  // the only one unable to ask for it.
  STPMode stp{STPMode::None};
  // Same reasoning for last look. A maker holds fills to protect a tight quote,
  // and quoting continuously is exactly when a quote is tight; a mass quote
  // that could not be marked non-firm left that maker choosing between the
  // primitive built for it and the control it needs.
  bool lastLook{false};
};

struct LastLookDecision  // maker accepts or rejects a held last-look fill
{
  uint64_t heldId{};
  SymbolId symbol{};
  bool accept{};
  uint64_t accountId{};
};

// Oracle/admin inputs that mutate state (drive liquidations / funding). They are
// sequenced and journaled like orders so deterministic replay reproduces every
// mark-driven liquidation and funding payment -- without them, derivatives
// recovery would diverge from the live state.
struct SetMark  // update the mark price (triggers maintenance-margin liquidations)
{
  SymbolId symbol{};
  Price mark{};
};

struct ApplyFunding  // settle a funding payment across open positions
{
  SymbolId symbol{};
  double rate{};
  Price mark{};
};

// Operator/session actions that mutate MATCHABLE state (auction uncross fills,
// emergency cancel-all, halt). Sequenced + journaled like orders so replay
// reproduces them at the exact point in the stream -- otherwise a crash after an
// opening uncross or an emergency halt-cancel would recover a divergent book /
// ledger. (Persistent risk-limit/config knobs are recovered from the config
// store, not the WAL, so they are NOT AdminCmds.)
enum class AdminAction : uint8_t
{
  BeginPreOpen,      // enter pre-open call-auction accumulation (no matching)
  OpenContinuous,    // uncross at the single clearing price, resume continuous
  ResumeAuction,     // clear halt + enter pre-open (re-opening auction)
  HaltAndCancelAll,  // emergency: halt + cancel the entire resting book
  Halt,              // reject new orders
  Resume,            // clear halt
  // Session boundary. Appended values (the enum is append-only: it rides an
  // existing journal tag, so a reordering would reinterpret every AdminCmd
  // record already on disk). Carried by AdminCmd rather than a command of its
  // own on purpose: a session transition is the same KIND of state as a halt or
  // an auction phase -- an operator action that mutates matchable state and must
  // replay at its exact point in the stream -- so it belongs in the record that
  // already carries those, and costs no new journal tag, no expectedBodySize
  // entry and no snapshot-format change.
  CloseSession,  // close the session: new orders rejected (MarketClosed), the book stands
  OpenSession,   // reopen the session; the halt/auction state underneath is untouched
  // Withdrawal from trading, as distinct from a halt or a closed session. A
  // halt promises the instrument comes back and a closed session promises the
  // next one; delisting promises neither, so it pulls the resting book on the
  // way out rather than leaving orders waiting for an open that is not coming.
  // Reversible by Relist on purpose: an irreversible operator action is one
  // mistake away from needing a restart to undo.
  Delist,
  Relist,
};

struct AdminCmd
{
  SymbolId symbol{};
  AdminAction action{};
};

// Genesis flows through the SAME sequenced, journaled stream as orders, so a
// replay from an EMPTY ledger/registry reproduces balances and instrument
// state without out-of-band seeding. Amounts travel as int64 raw at
// kMoneyScale (1e-8 units) -- the scale the ledger settles in -- matching the
// raw fixed-point convention of every other wire number (SetMark, prices).
struct Deposit  // credit external funds into an account
{
  uint64_t accountId{};
  AssetId asset{};
  int64_t amountRaw{};  // kMoneyScale units; <= 0 is ignored
  SymbolId symbol{};    // routing key only: the shard owning this account's ledger
};

struct Withdraw  // debit available funds; a no-op unless available >= amount
{
  uint64_t accountId{};
  AssetId asset{};
  int64_t amountRaw{};  // kMoneyScale units; <= 0 is ignored
  SymbolId symbol{};    // routing key only
};

// Instrument configuration mutations, sequenced so a restart replays them.
// ListInstrument carries the control-plane listing surface (see ControlApi);
// structural knobs beyond it (assets, scales, margin, fees) are startup
// configuration supplied when the shard is constructed.
struct ListInstrument
{
  SymbolId symbol{};
  Price tickSize{};
  Quantity lotSize{};
  Price minPrice{};
  Price maxPrice{};
};

struct SetBands  // adjust the static price band (collar) of a listed instrument
{
  SymbolId symbol{};
  Price minPrice{};
  Price maxPrice{};
};

struct SetTriggerRef  // switch the conditional-order reference (last trade vs mark)
{
  SymbolId symbol{};
  TriggerRef ref{TriggerRef::Last};
};

// Firm-group STP membership: map an account to a firm/group id so self-trade
// prevention fires across all of a firm's accounts. Sequenced and journaled
// like every other engine-state mutation (the group table feeds matching
// decisions), so replay and recovery reproduce the same STP outcomes; a
// checkpoint re-emits the live table as SetStpGroup records in its config
// section. group 0 removes the membership (back to account-level STP).
// What a counterparty is entitled to send.
//
// Not every session plays the same role. One routes flow it manages itself and
// must never leave an order resting here -- an order the framework holds and
// the counterparty does not know it owns is a position nobody reconciles.
// Another exists to post and pull quotes, and cancel/replace is its hot path.
// Those are opposite rights, so they are stated per account and refused on
// admission rather than assumed.
//
// A zero-valued profile permits everything, which is the behaviour of an
// engine that was never given one.
struct AdmissionProfile
{
  uint32_t allowedTypes{0};  // bit i = OrderType(i) allowed; 0 = all
  uint32_t allowedTif{0};    // bit i = TimeInForce(i) allowed; 0 = all
  uint8_t deny{0};           // bitmask of AdmissionDeny
};

// Rights withheld from a profile. A bitmask rather than four bools so the
// record stays blittable and appending a right does not change its size.
enum AdmissionDeny : uint8_t
{
  DenyResting = 1u << 0,  // a residual may not join the book
  DenyAmend = 1u << 1,    // ModifyOrder refused
  DenyCancel = 1u << 2,   // CancelOrder refused
  DenyQuote = 1u << 3,    // Quote refused
};

struct SetAdmissionProfile
{
  SymbolId symbol{};  // routing key
  uint64_t account{};
  AdmissionProfile profile{};
};

// Which risk limits a SetRiskLimits record carries.
//
// A mask rather than replace-all semantics: an operator raising a position cap
// must not silently zero the fat-finger cap by omitting it, and a record that
// says exactly what it changes replays the same way whatever else moved in
// between. Related knobs travel together because they are only meaningful as
// a pair.
enum RiskLimitField : uint16_t
{
  RiskLuld = 1u << 0,       // luldBps + luldHaltNs
  RiskFatFinger = 1u << 1,  // maxOrderQty + maxOrderNotional
  RiskMaxOpenOrders = 1u << 2,
  RiskMaxPosition = 1u << 3,
  RiskMargin = 1u << 4,  // initialMarginBps + maintenanceMarginBps
};

// Live risk limits.
//
// These used to be reachable only through direct setters on the engine, which
// applied immediately and rode nothing: a restart reverted them and a replica
// replaying the journal never saw the change. As a sequenced command they
// behave like the rest of engine state -- journaled, re-emitted by a
// checkpoint, replayed.
struct SetRiskLimits
{
  SymbolId symbol{};
  uint16_t fields{};  // bitmask of RiskLimitField; 0 = no-op
  int32_t luldBps{};
  int64_t luldHaltNs{};
  Quantity maxOrderQty{};
  Volume maxOrderNotional{};
  uint32_t maxOpenOrders{};
  Quantity maxPositionQty{};
  int32_t initialMarginBps{};
  int32_t maintenanceMarginBps{};
};

struct SetStpGroup
{
  SymbolId symbol{};  // routing key
  uint64_t account{};
  uint64_t group{};
};

// Idle time sweep: carries no order flow, only advances engine time so
// last-look holds (and GTD expiries) resolve on a quiet symbol instead of
// waiting for the next order. Sequenced and journaled like any other command,
// so a replay reproduces every timeout at the same point in the stream.
struct TimeTick
{
  SymbolId symbol{};  // routing key
};

// Perp funding calendar as STATE rather than as a formula. Without it the next
// funding boundary is derived from (now, SymbolConfig::fundingIntervalNs) -- a
// computation over startup config, not a fact, which silently disagrees with
// reality the moment an operator changes the interval or shifts a settlement.
// This command makes the calendar an engine fact: journaled, so it replays;
// hashed and checkpointed (RestoreFunding), so it survives a restart; and
// published on the derivatives feed as the actual next boundary. ApplyFunding
// then advances nextFundingNs by whole intervals.
//
// The engine still settles funding only when told to (ApplyFunding). This is
// the calendar it publishes and advances, NOT a timer that fires payments.
struct SetFundingSchedule
{
  SymbolId symbol{};
  int64_t intervalNs{0};     // funding interval (<= 0 clears the schedule)
  int64_t nextFundingNs{0};  // next settlement boundary, sequencer time (<= 0 clears)
};

// ---- Snapshot-only records ------------------------------------------------
// A checkpoint is NOT a parallel binary format: it is a journal-format file
// (same [ts][tag][len][body][crc] framing, same CRC/torn-tail protection)
// whose records rebuild engine state through the same apply machinery live
// traffic uses. These records are SNAPSHOT-ONLY: MatchingEngine::submit drops
// them from live traffic (a client must never be able to "restore" itself an
// order or a balance); they apply exclusively through
// MatchingEngine::applySnapshotRecord during shard recovery.
//
// Balances travel as snapshot-only RestoreBalance records carrying the EXACT
// signed (available, reserved) split -- so any live moment is representable,
// including a negative wallet mid-liquidation. RestoreReservation /
// RestorePosition then only rebuild the engine-side reservation/position
// tables; they do not move ledger money when the balances were restored
// exactly. Snapshots written by format version 1 carried Deposit records
// (TOTAL per account x asset) instead; those still apply through the same
// journaled-deposit path and reconstitute `reserved` by re-reservation
// (backward read compatibility). Instrument configuration travels as existing
// ListInstrument / SetBands / SetTriggerRef / SetStpGroup / AdminCmd records.

// v2: SnapshotBegin gained configHash; balances moved from Deposit totals to
// exact RestoreBalance splits; MMP fill windows serialize (RestoreMmpFills).
inline constexpr uint32_t kSnapshotFormatVersion = 2;

struct SnapshotBegin
{
  uint32_t formatVersion{kSnapshotFormatVersion};
  int64_t lastAppliedTs{0};  // sequencer-ts of the last command folded into this snapshot
  uint64_t stateHash{0};     // MatchingEngine::stateHash() at write time (repeated in SnapshotEnd)
  // Hash of the engine's CONSTRUCTOR configuration (scales, assets, tick/lot,
  // last-look window, perp mode, match policy -- MatchingEngine::configHash).
  // Recovery compares it against the loading engine and rejects the snapshot
  // on mismatch: restoring raw fixed-point state into an engine with, say,
  // different scales would silently reinterpret every price and quantity.
  // 0 = unknown (crafted/legacy file): the check is skipped.
  uint64_t configHash{0};
};

struct RestoreOrder  // one resting book order, applied straight to the TAIL of its level
{
  OrderId id{};
  uint64_t accountId{};
  Price price{};
  Quantity leaves{};  // displayed peak
  Side side{};
  Quantity hidden{};  // iceberg reserve (0 = none)
  Quantity peak{};    // iceberg display size (0 = non-iceberg)
  bool lastLook{false};
  bool reduceOnly{false};
  // The engine keeps only the per-account dedup SET (restored via
  // RestoreClOrdIds), not an order -> clientOrderId mapping, so writeSnapshot
  // emits 0 here; the field exists so the record stays self-contained if a
  // future engine retains the mapping.
  uint64_t clientOrderId{0};
  int64_t expiryNs{0};   // GTD expiry sequencer-ts (0 = none)
  uint64_t ocoGroup{0};  // OCO group (0 = none)
};

struct RestoreStop  // one pending conditional order (stop book)
{
  NewOrder order{};
  Price trigger{};  // current trigger; trailing: the ratcheted value (raw 0 = unarmed)
};

struct RestorePeg  // peg spec of a resting order (pegged_ entry)
{
  OrderId id{};
  Side side{};
  PegRef ref{PegRef::None};
  int64_t offsetRaw{0};
};

// Self-trade-prevention mode of one resting order.
//
// In continuous trading only the aggressor's mode counts, so the book never
// needed to carry it. An auction has no aggressor: both legs of an uncross
// print are resting, and the mode each of them asked for is the only thing
// that says whether they may trade with each other. Kept sparse -- orders
// with STPMode::None have no entry.
struct RestoreOrderStp
{
  OrderId id{};
  uint8_t mode{};  // STPMode
};

struct RestoreHeld  // one open last-look hold (mirrors MatchingEngine::Held)
{
  uint64_t heldId{};
  OrderId taker{};
  uint64_t takerAccount{};
  Side takerSide{};
  OrderId maker{};
  uint64_t makerAccount{};
  Price price{};
  Quantity qty{};
  int64_t deadline{};
  TimeInForce takerTif{TimeInForce::GTC};
  OrderType takerType{OrderType::LIMIT};
  Price takerPrice{};
  int64_t takerExpiryNs{0};
  bool makerReduceOnly{false};
  bool takerReduceOnly{false};
  // Whether the maker is in the engine's live-order tracking maps. Normally
  // true (a held maker stays tracked even fully off the book), but an
  // STP-cancel can legally remove a maker while its hold stays open -- the
  // write side records the live truth instead of re-deriving it.
  bool makerTracked{true};
};

struct RestorePosition  // one perp position (qty, average entry, posted margin)
{
  uint64_t account{};
  int64_t qtyRaw{0};    // signed contracts (Quantity raw)
  int64_t entryRaw{0};  // average entry price (Price raw)
  Amount marginRaw{0};  // posted position margin (quote raw); re-reserved on apply
};

struct RestoreMmpCfg  // market-maker-protection config for one account.
{
  // The mmp FILL WINDOWS restore EMPTY by design: the sliding window is
  // shorter than any realistic checkpoint interval, so the lost tail of the
  // window cannot span a restart (see docs/venue/runtime.md).
  uint64_t account{};
  Quantity qtyLimit{};
  int64_t windowNs{0};
};

inline constexpr uint32_t kClOrdIdBatch = 32;

struct RestoreClOrdIds  // fixed-size batch of an account's clientOrderId dedup set
{
  // The set can be large; it serializes as repeated fixed-size batches so the
  // journal's strictly-sized blittable body model is preserved.
  uint64_t account{};
  uint32_t count{0};  // ids[0..count) valid, count <= kClOrdIdBatch
  uint64_t ids[kClOrdIdBatch]{};
};

// One buying-power reservation entry (MatchingEngine::reserve_), serialized
// EXACTLY as held live. Reservations are deliberately not re-derived from
// order formulas on load: partial fills, held slices and STP interactions
// make the live amount history-dependent, so the record carries it. Applying
// still moves the amount available -> reserved through Ledger::reserve, so
// the deposited totals split back into the exact live available/reserved.
struct RestoreReservation
{
  OrderId id{};
  uint64_t account{};
  AssetId asset{};
  Side side{};
  int64_t limitPriceRaw{0};
  Amount reservedRaw{0};
};

// One (account, asset) ledger balance, EXACT signed available/reserved split.
// Snapshot-only: a client that could submit this would set its own balance.
// Applied via Ledger::restore (recovery-only direct write); RestoreReservation
// and RestorePosition records that follow rebuild the engine-side tables
// WITHOUT re-reserving, so the split is preserved bit-for-bit -- including
// states Deposit records could not represent (negative available mid-
// liquidation, non-positive totals).
struct RestoreBalance
{
  uint64_t account{};
  AssetId asset{};
  Amount availableRaw{0};  // signed, kMoneyScale units
  Amount reservedRaw{0};
};

inline constexpr uint32_t kMmpFillBatch = 16;

// Fixed-size batch of one account's MMP sliding-window fills, in deque (time)
// order; consecutive batches for the same account concatenate. Restores the
// window EXACTLY, so a market maker sitting one fill from its qtyLimit is
// still one fill from it after recovery -- the window no longer restores
// empty.
struct RestoreMmpFills
{
  uint64_t account{};
  uint32_t count{0};  // entries [0..count) valid, count <= kMmpFillBatch
  int64_t tsNs[kMmpFillBatch]{};
  int64_t qtyRaw[kMmpFillBatch]{};
};

// Derivatives funding state: the last applied rate and the live funding
// calendar. A snapshot-only record and not three more fields on SnapshotEnd,
// because SnapshotEnd is a strictly-sized journal body -- widening it would
// change its expectedBodySize and make every existing snapshot unreadable
// (the loader stops at the first wrong-sized record). The same additive path
// balances (RestoreBalance), MMP windows (RestoreMmpFills) and STP groups took.
//
// A snapshot written by an engine with no funding state at all does not carry
// this record; a file without it restores rate 0 and no schedule, exactly as
// before the record existed (read compatibility, pinned by a test).
struct RestoreFunding
{
  int64_t fundingRateRaw{0};     // last applied rate, kFundingRateScale
  int64_t nextFundingNs{0};      // next settlement boundary (0 = no schedule set)
  int64_t fundingIntervalNs{0};  // schedule interval (0 = no schedule set)
};

struct SnapshotEnd
{
  uint64_t stateHash{0};  // must equal the loader's recomputed hash, or the snapshot is corrupt
  uint64_t tradeSeq{0};
  uint64_t heldSeq{0};
  int64_t timeCounter{0};
  int64_t nowNs{0};
  uint64_t mdEpoch{0};  // 0 = none (the engine carries no MD epoch today)
  int64_t lastPriceRaw{0};
  bool hasLast{false};
  int64_t markPriceRaw{0};
  bool hasMark{false};
  int64_t haltUntilNs{0};  // pending timed (LULD) halt deadline (0 = none)
};

// Journal tags are the variant indices: append-only, never reorder.
// (SetTriggerRef postdates TimeTick, hence its position at the tail; the
// snapshot-only records postdate SetTriggerRef; RestoreBalance/RestoreMmpFills
// and the live SetStpGroup command postdate the original snapshot block, so
// live and snapshot-only tags interleave past tag 24. SetFundingSchedule (live)
// and RestoreFunding (snapshot-only) are the newest pair, tags 28 and 29.)
// Force-close a perp position from OUTSIDE the engine. Isolated margin lets the
// engine decide for itself (it sees one symbol and the collateral behind it),
// but a portfolio-margin model decides on the whole basket and lives above the
// per-symbol engines -- it can already stop an account trading (MassCancel) and
// until now had no way to close what it holds. Journaled like any command, so
// replay reproduces an externally-driven liquidation exactly.
struct ForceClosePosition
{
  uint64_t accountId{};
  SymbolId symbol{};
  int64_t qtyRaw{};  // 0 = the whole position
};

using InboundCommand =
    std::variant<NewOrder, CancelOrder, ModifyOrder, MassCancel, Quote, LastLookDecision, SetMark,
                 ApplyFunding, AdminCmd, Deposit, Withdraw, ListInstrument, SetBands, TimeTick,
                 SetTriggerRef, SnapshotBegin, RestoreOrder, RestoreStop, RestorePeg, RestoreHeld,
                 RestorePosition, RestoreMmpCfg, RestoreClOrdIds, SnapshotEnd, RestoreReservation,
                 RestoreBalance, RestoreMmpFills, SetStpGroup, SetFundingSchedule, RestoreFunding,
                 ForceClosePosition, RestoreOrderStp, SetAdmissionProfile, SetRiskLimits>;

inline constexpr size_t kFirstSnapshotTag = 15;
inline constexpr size_t kLastContiguousSnapshotTag = 24;
static_assert(std::is_same_v<std::variant_alternative_t<kFirstSnapshotTag, InboundCommand>,
                             SnapshotBegin>,
              "kFirstSnapshotTag must index SnapshotBegin");
static_assert(std::is_same_v<std::variant_alternative_t<kLastContiguousSnapshotTag, InboundCommand>,
                             RestoreReservation>,
              "kLastContiguousSnapshotTag must index RestoreReservation");

// Snapshot-only records are forbidden in live traffic; the engine's submit
// dispatcher drops them and recovery applies them through a dedicated path.
// Tags [kFirstSnapshotTag, kLastContiguousSnapshotTag] are the original
// contiguous snapshot block; appended alternatives past it interleave live
// commands with snapshot-only records, so membership is explicit from there.
inline bool isSnapshotRecord(const InboundCommand& c) noexcept
{
  const size_t i = c.index();
  return (i >= kFirstSnapshotTag && i <= kLastContiguousSnapshotTag) ||
         std::holds_alternative<RestoreBalance>(c) || std::holds_alternative<RestoreMmpFills>(c) ||
         std::holds_alternative<RestoreFunding>(c);
}

// ---- Outbound events ------------------------------------------------------

// Owner account on per-order events (appended fields; wire codecs place them
// last, and the order-entry wire does not carry them -- the session already IS
// the account). They exist so the delivery layer (SessionRegistry) can route an
// asynchronous exec report to the session that owns it: a maker fill from a
// foreign aggressor, a stop trigger, a GTD expiry or a liquidation cancel has
// no request/response context to answer on. account == 0 = unrouteable
// (unbound/trusted-transport sessions).
struct OrderAccepted  // order accepted / working
{
  OrderId id{};
  SymbolId symbol{};
  Side side{};
  Price price{};
  Quantity leavesQty{};      // full working quantity (for the owner's ack)
  bool restingOnBook{true};  // false = working but not on the visible book (pending stop)
  Quantity displayQty{};     // publicly visible size (== leavesQty unless iceberg; 0 = use leavesQty)
  uint64_t account{0};       // owner (appended: delivery routing)
};

struct OrderRejected
{
  OrderId id{};
  SymbolId symbol{};
  RejectReason reason{};
  uint64_t account{0};  // owner (appended: delivery routing)
};

struct Trade
{
  uint64_t tradeId{};
  SymbolId symbol{};
  Price price{};  // maker price
  Quantity quantity{};
  OrderId makerId{};
  OrderId takerId{};
  Side takerSide{};
  uint64_t makerAccount{};
  uint64_t takerAccount{};
};

struct OrderExecuted  // per-order execution report on a fill
{
  OrderId id{};
  SymbolId symbol{};
  Quantity lastQty{};
  Quantity leavesQty{};
  bool aggressor{false};  // true = taker leg
  bool complete{false};   // fully filled
  Price lastPx{};         // price of this fill (for the exec report / client reconciliation)
  // Displayed remaining after this fill, for the PUBLIC market-data feed. For a
  // non-iceberg this equals leavesQty; for an iceberg it is only the visible peak
  // (never the hidden reserve), so the public book cannot be probed for hidden
  // size. leavesQty stays the whole remaining for the owner's exec report.
  Quantity displayLeaves{};
  uint64_t account{0};  // owner of this order leg (appended: delivery routing)
};

struct OrderCanceled
{
  OrderId id{};
  SymbolId symbol{};
  CancelReason reason{};
  uint64_t account{0};  // owner (appended: delivery routing)
};

struct OrderModified
{
  OrderId id{};
  SymbolId symbol{};
  Price price{};
  Quantity leavesQty{};
  bool priorityKept{false};  // false = re-entered at the tail (lost time priority)
  uint64_t account{0};       // owner (appended: delivery routing)
};

struct OrderTriggered  // a stop / take-profit activated and was injected into matching
{
  OrderId id{};
  SymbolId symbol{};
  Price refPrice{};     // reference (last-trade) price that crossed the trigger
  uint64_t account{0};  // owner (appended: delivery routing)
};

struct FillHeld  // last-look: a fill is held pending the maker's decision
{
  uint64_t heldId{};
  SymbolId symbol{};
  OrderId makerId{};
  OrderId takerId{};
  Price price{};
  Quantity qty{};
  // Maker's DISPLAYED remaining after the held qty is reserved out of the book
  // (iceberg: the refilled peak). Lets the public feed keep level == book while
  // the qty is in limbo. Appended field -- wire codecs place it last.
  Quantity makerDisplayAfter{};
  uint64_t makerAccount{0};  // appended: delivery routing (both parties get the report)
  uint64_t takerAccount{0};
};

struct FillRejected  // last-look: the held fill was rejected (or timed out)
{
  uint64_t heldId{};
  SymbolId symbol{};
  OrderId takerId{};
  // Appended fields (wire codecs place them last): what was rejected, so the
  // taker can show a usable report -- counterparty order, price and size of the
  // fill that did not happen.
  OrderId makerId{};
  Price price{};
  Quantity qty{};
  uint64_t takerAccount{0};  // appended: delivery routing (both parties get the report)
  uint64_t makerAccount{0};
};

struct MmpTriggered  // market-maker protection fired: the account was mass-canceled
{
  uint64_t accountId{};
  SymbolId symbol{};
};

struct FeeCharged  // per-leg maker/taker fee (negative = rebate)
{
  OrderId id{};
  SymbolId symbol{};
  Volume fee{};
  bool maker{};
  uint64_t account{0};  // charged account (appended: delivery routing)
};

struct Liquidation  // a perp position was force-closed below maintenance margin
{
  uint64_t account{};
  SymbolId symbol{};
  Quantity qty{};   // absolute size closed
  Price price{};    // mark price at liquidation
  bool bankrupt{};  // equity went negative -> insurance fund covered the deficit
  bool adl{};       // auto-deleveraged: closed to absorb a bankrupt counterparty
};

enum class BalanceReason : uint8_t
{
  Deposit = 0,
  Withdraw = 1,
  WithdrawRejected = 2,  // insufficient available: nothing moved
};

// Balance change report on a sequenced Deposit/Withdraw. Carries the POST-event
// available/reserved of the touched (account, asset) so the owner needs no
// arithmetic of its own; a rejected withdraw reports the unchanged balances
// with reason WithdrawRejected -- the client is told, never left guessing.
// Emitted from the engine's journaled money path only: snapshot restore
// (Deposit records inside a checkpoint) replays through the same code but the
// shard's recovery suppression keeps it off the wire, like every replayed
// event.
struct BalanceUpdate
{
  uint64_t account{};
  AssetId asset{};
  int64_t availableRaw{};  // kMoneyScale units after this event
  int64_t reservedRaw{};
  BalanceReason reason{};
};

// ---- Instrument-wide outbound events --------------------------------------
// These carry no account: they describe the instrument, not a client's order,
// so the delivery layer routes them to nobody and the order-entry codecs have
// no exec-report form for them (like MmpTriggered / FeeCharged / Liquidation).
// Their consumer is the public market-data feed (market_data.h).

// Trading state of the instrument. Exactly the states the engine HAS:
// continuous matching, an operator halt, the timed limit-up/limit-down
// volatility pause, pre-open accumulation, the uncross that ends it, and a
// closed session.
//
// Closed and Halted are deliberately DIFFERENT states, not one state with two
// names. A halt is an exception -- something went wrong and the operator
// stopped the instrument; a closed session is the instrument's normal
// out-of-hours condition. A subscriber that cannot tell them apart cannot tell
// a broken market from a sleeping one, and the two reject an order with
// different reasons (Halted vs MarketClosed). What the engine deliberately does
// NOT own is the CALENDAR: it holds the state and the transitions, while the
// schedule that fires them belongs to the operator / control plane (see
// docs/venue/runtime.md).
enum class TradingStatus : uint8_t
{
  Trading = 0,         // continuous matching
  Halted = 1,          // operator halt: new orders rejected (no deadline)
  LuldPause = 2,       // timed volatility pause after a band breach (untilNs = deadline)
  AuctionPreOpen = 3,  // pre-open accumulation, no matching (a crossed book is legal)
  AuctionUncross = 4,  // the uncross itself; the next transition ends the auction
  // Appended value -- wire enums are append-only, so a decoder of the previous
  // schema keeps reading every field of the message and sees only an unknown
  // status code (which it must treat as "not tradeable", never as Trading).
  Closed = 5,  // session closed: new orders rejected, the book stands
  // Appended: withdrawn from trading with no scheduled return. Ranked outside
  // every other status -- a delisted instrument is not halted, not closed and
  // not in an auction, and nothing underneath it can make it tradeable.
  Delisted = 6,
};

enum class TradingStatusReason : uint8_t
{
  None = 0,
  Administrative = 1,    // operator action (AdminCmd Halt / Resume / HaltAndCancelAll)
  LuldBreach = 2,        // a limit-up/limit-down band breach tripped the pause
  LuldPauseElapsed = 3,  // the timed pause deadline passed and trading resumed
  Auction = 4,           // an auction phase transition (pre-open, uncross, re-open)
  Session = 5,           // a session boundary (AdminCmd CloseSession / OpenSession)
};

// Emitted on every trading-state TRANSITION of the symbol -- from the engine's
// own state changes, never inferred downstream and never repeated periodically.
// untilNs is the sequencer-ts the timed pause expires at (0 = no deadline), so
// it replays identically.
struct TradingStatusChanged
{
  SymbolId symbol{};
  TradingStatus status{};
  TradingStatusReason reason{};
  int64_t untilNs{0};
};

// Funding rate fixed-point scale: the same power of ten prices and quantities
// use, so a rate travels as a raw int64 like every other wire number and never
// as a double. 0.0001 (1bp per interval) = 10'000 raw.
inline constexpr int64_t kFundingRateScale = Price::Scale;

// Derivatives state of the instrument, emitted when the engine LEARNS it: on a
// sequenced SetMark and on a sequenced ApplyFunding. Both are journaled, so the
// values reproduce on replay.
//
// openInterest is the long side of the open positions the engine tracks for
// this symbol (equal to the short side, since every contract has both legs) --
// a real sum over positions_, not an estimate. It is published with the mark
// rather than on every fill: a per-trade open-interest message would multiply
// the feed's message rate for a number consumers read at mark cadence.
struct DerivativesUpdated
{
  SymbolId symbol{};
  Price mark{};               // last mark price the engine was given (0 before the first SetMark)
  int64_t fundingRateRaw{0};  // last applied rate, kFundingRateScale, per funding interval
  int64_t nextFundingNs{0};   // next funding boundary (0 = no funding interval configured)
  Quantity openInterest{};    // long side of open positions on this symbol
};

// Appended alternatives go at the END (wire codecs and the event hash key off
// the alternative order).
// A cancel or a cancel/replace that was refused.
//
// Distinct from OrderRejected because it answers a different request and, on
// the wire, a different message: FIX 4.4 answers a refused 35=F/35=G with
// OrderCancelReject (35=9), not with an execution report. Carrying which
// request it was ON THE EVENT is what makes that encodable -- a resend
// re-encodes from the event log long after the session forgot the context, and
// a replayed message that changes type is a protocol violation exactly when
// the counterparty is recovering.
struct CancelRejected
{
  OrderId id{};
  SymbolId symbol{};
  RejectReason reason{};
  uint64_t account{0};
  bool wasReplace{false};  // false = cancel request, true = cancel/replace
};

using OutboundEvent =
    std::variant<OrderAccepted, OrderRejected, Trade, OrderExecuted, OrderCanceled, OrderModified,
                 OrderTriggered, FillHeld, FillRejected, MmpTriggered, FeeCharged, Liquidation,
                 BalanceUpdate, TradingStatusChanged, DerivativesUpdated, CancelRejected>;

}  // namespace flox::venue
