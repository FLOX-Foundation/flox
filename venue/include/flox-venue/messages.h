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
#include "flox/util/base/time.h"

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

// How a price level is allocated between the makers resting on it. It lives
// here, next to the instrument's other wire-level choices, because it is
// instrument CONFIGURATION: SymbolConfig carries it, and the config is the
// only route a deployment has into a shard, a journal or a gateway. The
// Matcher that acts on it is one reader of the answer, not its owner.
enum class MatchPolicy : uint8_t
{
  PriceTimeFifo = 0,
  ProRata = 1,  // thick-level proportional distribution (crossProRata)
};

// Which values of an order's enum-typed fields actually exist.
//
// Every one of them crosses the wire as a single byte, and a byte carries 256
// values where these enums name at most eight. A decoder that casts the byte
// straight into the enum hands the engine a value no branch there was written
// for, and the engine has no way to tell it apart from one it chose itself: an
// order type wider than the admission bitmap that indexes it, or a self-trade
// mode the auction uncross cannot act on. Every decoder asks here before it
// builds a command, so the answer to "does this value exist" is given once.
//
// Written as a switch rather than a range test on the last enumerator: a value
// appended to one of these enums is refused until it is listed here, and that
// is the safe direction to fail in -- a client gets a reject it can read
// instead of reaching a branch nobody wrote for it.
constexpr bool inRange(Side s) noexcept
{
  switch (s)
  {
    case Side::BUY:
    case Side::SELL:
      return true;
  }
  return false;
}

constexpr bool inRange(OrderType t) noexcept
{
  switch (t)
  {
    case OrderType::LIMIT:
    case OrderType::MARKET:
    case OrderType::STOP_MARKET:
    case OrderType::STOP_LIMIT:
    case OrderType::TAKE_PROFIT_MARKET:
    case OrderType::TAKE_PROFIT_LIMIT:
    case OrderType::TRAILING_STOP:
    case OrderType::ICEBERG:
      return true;
  }
  return false;
}

constexpr bool inRange(TimeInForce f) noexcept
{
  switch (f)
  {
    case TimeInForce::GTC:
    case TimeInForce::IOC:
    case TimeInForce::FOK:
    case TimeInForce::GTD:
    case TimeInForce::POST_ONLY:
      return true;
  }
  return false;
}

constexpr bool inRange(STPMode m) noexcept
{
  switch (m)
  {
    case STPMode::None:
    case STPMode::CancelNewest:
    case STPMode::CancelOldest:
    case STPMode::CancelBoth:
    case STPMode::Decrement:
      return true;
  }
  return false;
}

constexpr bool inRange(PegRef p) noexcept
{
  switch (p)
  {
    case PegRef::None:
    case PegRef::Bid:
    case PegRef::Ask:
    case PegRef::Mid:
      return true;
  }
  return false;
}

constexpr bool inRange(TriggerRef r) noexcept
{
  switch (r)
  {
    case TriggerRef::Last:
    case TriggerRef::Mark:
      return true;
  }
  return false;
}

struct NewOrder
{
  OrderId id{};
  SymbolId symbol{};
  Side side{};
  OrderType type{OrderType::LIMIT};  // LIMIT | MARKET and the conditional (stop / take-profit / trailing) types
  // Explicit alignment padding: the compiler inserted 7 bytes here
  // regardless, uninitialised; naming it means every construction path (NSDMI
  // on this field) writes zero instead of leaving whatever was on the stack.
  // See journal.h for the static_assert pair that keeps this honest.
  uint8_t pad0_[7]{};
  Price price{};  // ignored for MARKET
  Quantity quantity{};
  TimeInForce tif{TimeInForce::GTC};
  bool postOnly{false};
  STPMode stp{STPMode::None};
  uint8_t pad1_[5]{};  // explicit alignment padding
  uint64_t accountId{0};
  uint64_t clientOrderId{0};
  Quantity visibleQuantity{};  // iceberg display size; 0 or >= quantity = fully visible
  Price triggerPrice{};        // stop / take-profit activation price
  Price trailingOffset{};      // trailing-stop offset (price distance from extreme)
  bool lastLook{false};        // maker holds a fill for a last-look window before confirming
  bool reduceOnly{false};      // derivatives: may only reduce/close a position, never increase
  uint8_t pad2_[6]{};          // explicit alignment padding
  SeqNanos expiryNs{};         // GTD: sequencer time at/after which a resting order auto-cancels (0 = none)
  uint64_t ocoGroup{0};        // OCO: orders sharing a group cancel each other on the first fill (0 = none)
  PegRef peg{PegRef::None};    // peg: re-price to track Bid/Ask/Mid each submit boundary
  uint8_t pad3_[7]{};          // explicit alignment padding
  int64_t pegOffsetRaw{0};     // signed price offset from the peg reference (raw ticks)
};

struct CancelOrder
{
  OrderId id{};
  SymbolId symbol{};
  uint8_t pad0_[4]{};  // explicit alignment padding
  uint64_t accountId{0};
};

struct ModifyOrder  // cancel/replace
{
  OrderId id{};
  SymbolId symbol{};
  uint8_t pad0_[4]{};  // explicit alignment padding
  Price newPrice{};    // raw 0 = keep current price
  // New leaves target. On an iceberg this is the TOTAL remaining -- displayed
  // peak plus hidden reserve -- which is the same number an execution report
  // gives as that order's leavesQty. The peak itself is preserved across the
  // amend; there is no way to change it without a fresh order.
  Quantity newQty{};
  uint64_t accountId{0};
};

struct MassCancel  // cancel every resting order of an account (MM cancel-all)
{
  uint64_t accountId{};
  SymbolId symbol{};
  uint8_t pad0_[4]{};  // explicit alignment padding, tail
};

struct Quote  // two-sided market-maker quote (replace prior quote on this symbol)
{
  OrderId bidId{};
  OrderId askId{};
  SymbolId symbol{};
  uint8_t pad0_[4]{};  // explicit alignment padding
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
  // And the rest of the controls a single order has. post-only is the one a
  // two-sided quote needs most: a maker repricing into a market that has
  // already moved crosses the book with the near leg and pays to take the
  // liquidity it meant to provide. The others follow the NewOrder semantics
  // exactly and apply to both legs.
  bool postOnly{false};
  bool reduceOnly{false};
  TimeInForce tif{TimeInForce::GTC};
  uint8_t pad1_[3]{};          // explicit alignment padding
  Quantity visibleQuantity{};  // iceberg peak per leg (0 = show the visible leg)
  SeqNanos expiryNs{};         // GTD expiry for both legs (0 = none)
  // Appended field: a quote is one submission that becomes two resting
  // orders (bidId, askId), so it is the one InboundCommand that already
  // splits into children by design. Both legs are stamped with this same
  // value (see MatchingEngine::onQuote), so every report on either leg
  // carries the name the submitter gave the QUOTE, not a per-leg id it
  // never chose -- otherwise a submitter reconciling two reports that carry
  // two different venue order ids and no shared name of its own has nothing
  // to join them on but symbol and timing.
  uint64_t clientOrderId{0};
};

// How many price points a ladder carries per side. 8 rather than 16: the
// mirror feeders above this engine publish 5 levels a side today, so 8 leaves
// room to grow without paying for slots nobody fills -- and an unfilled slot
// is not free, because the wire body is as long as the ladder has levels (see
// quoteLadderBodySize below). A maker that needs more sends a second ladder
// on its own id block.
inline constexpr uint8_t kQuoteLadderLevels = 8;

// One price point of a ladder: the bid and the ask this level asks for.
//
// No ids of its own. The ladder names one id for each side (bidIdBase /
// askIdBase) and level i is slot i of that block, so a ladder owns the ids
// [bidIdBase, bidIdBase + kQuoteLadderLevels) and the matching ask range. Two
// ids per level on the wire would be 16 bytes a level for numbers the
// submitter derives anyway -- and, more importantly, would leave the engine
// unable to name the levels a SHORTER ladder drops, which is the whole point
// of replacing a set rather than updating it one Quote at a time.
struct QuoteLadderLevel
{
  Price bidPrice{};
  Quantity bidQty{};  // 0 = no bid at this level (the prior leg is still taken down)
  Price askPrice{};
  Quantity askQty{};  // 0 = no ask at this level
};

// A market maker's whole set of levels on one symbol, replaced atomically.
//
// Exactly equivalent to kQuoteLadderLevels Quote commands submitted back to
// back -- `levels` of them carrying this ladder's rungs and the rest carrying
// zero quantities on both sides, which is how a shorter ladder takes its
// surplus levels down. Same leg order, same ids, same events, byte for byte
// (see engine::QuoteLadderLegs and MatchingEngine::onQuoteLadder). What it is
// NOT is a second matching path: it is one journal record and one sequencer
// slot instead of K of each.
//
// The clientOrderId is the name the submitter gave the LADDER and every leg
// carries it, the same rule a Quote already applies to its two legs; it is
// deduplicated once, for the ladder as a whole.
//
// `levels` above kQuoteLadderLevels is clamped, not refused, and clamped
// identically by the engine and by the journal writer -- so a replay of the
// record applies exactly what the live run applied.
struct QuoteLadder
{
  uint64_t accountId{};
  uint64_t clientOrderId{0};
  SeqNanos expiryNs{};         // GTD expiry for every leg (0 = none)
  Quantity visibleQuantity{};  // iceberg peak per leg (0 = show the visible leg)
  OrderId bidIdBase{};         // level i is bidIdBase + i
  OrderId askIdBase{};         // level i is askIdBase + i
  SymbolId symbol{};
  uint8_t levels{0};  // live rungs; the rest of the block is taken down
  STPMode stp{STPMode::None};
  bool lastLook{false};
  bool postOnly{false};
  bool reduceOnly{false};
  TimeInForce tif{TimeInForce::GTC};
  uint8_t pad0_[6]{};  // explicit alignment padding
  QuoteLadderLevel level[kQuoteLadderLevels]{};
};

// Rungs this ladder actually names. The clamp is the one place both the
// engine and the journal ask, so a `levels` byte past the end of the block
// cannot mean one thing live and another on replay.
inline constexpr uint8_t quoteLadderLiveLevels(const QuoteLadder& l) noexcept
{
  return l.levels > kQuoteLadderLevels ? kQuoteLadderLevels : l.levels;
}

// Bytes a ladder occupies on the wire: the fixed head plus the rungs it
// names. This is the one journaled body whose length is a property of the
// record rather than of its type -- a 5-level ladder costs 5 levels, not 8,
// which is what makes one record cheaper than the Quotes it replaces instead
// of merely tidier. sizeof(QuoteLadder) is still the maximum, and still the
// size the format fingerprint folds in.
inline constexpr size_t kQuoteLadderHeadSize = offsetof(QuoteLadder, level);

inline constexpr size_t quoteLadderBodySize(const QuoteLadder& l) noexcept
{
  return kQuoteLadderHeadSize +
         static_cast<size_t>(quoteLadderLiveLevels(l)) * sizeof(QuoteLadderLevel);
}

struct LastLookDecision  // maker accepts or rejects a held last-look fill
{
  uint64_t heldId{};
  SymbolId symbol{};
  bool accept{};
  uint8_t pad0_[3]{};  // explicit alignment padding
  uint64_t accountId{};
};

// Oracle/admin inputs that mutate state (drive liquidations / funding). They are
// sequenced and journaled like orders so deterministic replay reproduces every
// mark-driven liquidation and funding payment -- without them, derivatives
// recovery would diverge from the live state.
struct SetMark  // update the mark price (triggers maintenance-margin liquidations)
{
  SymbolId symbol{};
  uint8_t pad0_[4]{};  // explicit alignment padding
  Price mark{};
};

struct ApplyFunding  // settle a funding payment across open positions
{
  SymbolId symbol{};
  uint8_t pad0_[4]{};  // explicit alignment padding
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
  uint8_t pad0_[3]{};  // explicit alignment padding, tail
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
  uint8_t pad0_[6]{};   // explicit alignment padding
  int64_t amountRaw{};  // kMoneyScale units; <= 0 is ignored
  SymbolId symbol{};    // routing key only: the shard owning this account's ledger
  uint8_t pad1_[4]{};   // explicit alignment padding, tail
};

struct Withdraw  // debit available funds; a no-op unless available >= amount
{
  uint64_t accountId{};
  AssetId asset{};
  uint8_t pad0_[6]{};   // explicit alignment padding
  int64_t amountRaw{};  // kMoneyScale units; <= 0 is ignored
  SymbolId symbol{};    // routing key only
  uint8_t pad1_[4]{};   // explicit alignment padding, tail
};

// Instrument configuration mutations, sequenced so a restart replays them.
// ListInstrument carries the control-plane listing surface (see ControlApi);
// structural knobs beyond it (assets, scales, margin, fees) are startup
// configuration supplied when the shard is constructed.
struct ListInstrument
{
  SymbolId symbol{};
  uint8_t pad0_[4]{};  // explicit alignment padding
  Price tickSize{};
  Quantity lotSize{};
  Price minPrice{};
  Price maxPrice{};
};

struct SetBands  // adjust the static price band (collar) of a listed instrument
{
  SymbolId symbol{};
  uint8_t pad0_[4]{};  // explicit alignment padding
  Price minPrice{};
  Price maxPrice{};
};

struct SetTriggerRef  // switch the conditional-order reference (last trade vs mark)
{
  SymbolId symbol{};
  TriggerRef ref{TriggerRef::Last};
  uint8_t pad0_[3]{};  // explicit alignment padding, tail
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
  uint8_t pad0_[3]{};        // explicit alignment padding, tail
};

// Rights withheld from a profile. A bitmask rather than four bools so the
// record stays blittable and appending a right does not change its size.
enum AdmissionDeny : uint8_t
{
  DenyResting = 1u << 0,  // a residual may not join the book
  DenyAmend = 1u << 1,    // ModifyOrder refused
  DenyCancel = 1u << 2,   // CancelOrder refused
  DenyQuote = 1u << 3,    // Quote / QuoteLadder refused
  // A quotes-only counterparty (a FIX MassQuote/QuoteCancel session)
  // is the mirror image of DenyQuote: it may replace its ladder but must
  // never place or work a plain order, so the two flags together
  // (DenyNewOrder | DenyCancel, DenyQuote left unset) are what that profile
  // actually sets. Refused on admission, before NewOrder reaches any other
  // gate -- the same posture as every other Deny* flag.
  DenyNewOrder = 1u << 4,  // NewOrder refused
};

struct SetAdmissionProfile
{
  SymbolId symbol{};   // routing key
  uint8_t pad0_[4]{};  // explicit alignment padding
  uint64_t account{};
  AdmissionProfile profile{};
  uint8_t pad1_[4]{};  // explicit alignment padding, tail
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
  uint16_t fields{};   // bitmask of RiskLimitField; 0 = no-op
  uint8_t pad0_[2]{};  // explicit alignment padding
  int32_t luldBps{};
  uint8_t pad1_[4]{};       // explicit alignment padding
  DurationNs luldHaltNs{};  // pause LENGTH -- an interval, not a moment (haltUntil = now + this)
  Quantity maxOrderQty{};
  Volume maxOrderNotional{};
  uint32_t maxOpenOrders{};
  uint8_t pad2_[4]{};  // explicit alignment padding
  Quantity maxPositionQty{};
  int32_t initialMarginBps{};
  int32_t maintenanceMarginBps{};
};

// Which limits a SetAccountRiskLimits record carries.
enum AccountRiskLimitField : uint16_t
{
  AccountRiskFatFinger = 1u << 0,  // maxOrderQty + maxOrderNotional
  AccountRiskMaxOpenOrders = 1u << 1,
  AccountRiskMaxPosition = 1u << 2,
};

// Risk limits on one ACCOUNT, sequenced.
//
// SetRiskLimits is the symbol's: every account on the instrument is bound by
// it. An owner of risk above the venue -- a prime broker's limit desk, a
// margin engine -- tightens ONE account, and it has to do so through the
// journal: the journal is written before the decision, a replay decides
// again, and a limit that lived outside the journal is a limit the replay
// never saw, so an order the live engine refused is accepted on replay.
// That is the defect this record closes. Applied in stream order like any
// command; written into the snapshot's config section so a recovered engine
// refuses what the live one refused; a field mask says which limits the
// record carries, so tightening one cannot zero another by omission. Zero in
// a carried field means "unchecked" for that account, the way it does on the
// symbol. Where both the symbol's and the account's limit are set, the
// tighter one binds.
struct SetAccountRiskLimits
{
  SymbolId symbol{};   // routing key
  uint8_t pad0_[2]{};  // explicit alignment padding
  uint16_t fields{};   // bitmask of AccountRiskLimitField; 0 = no-op
  uint64_t account{};
  Quantity maxOrderQty{};
  Volume maxOrderNotional{};
  uint32_t maxOpenOrders{};
  uint8_t pad1_[4]{};  // explicit alignment padding
  Quantity maxPositionQty{};
};

struct SetStpGroup
{
  SymbolId symbol{};   // routing key
  uint8_t pad0_[4]{};  // explicit alignment padding
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
  uint8_t pad0_[4]{};        // explicit alignment padding
  DurationNs intervalNs{};   // funding interval (<= 0 clears the schedule)
  SeqNanos nextFundingNs{};  // next settlement boundary, sequencer time (<= 0 clears)
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
// What a maker did with the fills it was offered under last look, per maker.
//
// The split by direction is the point. A maker that refuses only when the
// price moved ITS way is taking a free option: it keeps the good fills and
// returns the bad ones. One that refuses in both directions at a similar rate
// is answering a latency problem, not picking. Totals alone cannot tell those
// apart, which is why `rejectedAdverse` and `rejectedFavourable` are counted
// separately rather than derived.
//
// Lives here rather than inside MatchingEngine because a metrics exporter has
// to name the type, and a type nested in a class template is a different type
// for every book the engine is instantiated with.
struct LastLookStats
{
  uint64_t held{0};
  uint64_t accepted{0};
  uint64_t rejected{0};
  uint64_t adverse{0};          // holds where the move went against the maker
  uint64_t rejectedAdverse{0};  // ... of which it refused
  uint64_t favourable{0};       // holds where the move went its way
  uint64_t rejectedFavourable{0};
};

// 3: order and held-fill records carry the submitter's own identifier. A
// version-2 snapshot would restore orders whose reports name nobody, so it is
// refused outright rather than read as though the field had always been zero.
inline constexpr uint32_t kSnapshotFormatVersion = 4;

struct SnapshotBegin
{
  uint32_t formatVersion{kSnapshotFormatVersion};
  uint8_t pad0_[4]{};        // explicit alignment padding
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
  uint8_t pad0_[4]{};  // explicit alignment padding
  Quantity hidden{};   // iceberg reserve (0 = none)
  Quantity peak{};     // iceberg display size (0 = non-iceberg)
  bool lastLook{false};
  bool reduceOnly{false};
  bool postOnly{false};  // may never take, including after an amend
  uint8_t pad1_[5]{};    // explicit alignment padding
  // The identifier the submitter gave the order. The resting record carries
  // it now (see RestingOrder), so this is the real value and not a placeholder
  // -- a restored order reports under the same name its submitter chose.
  uint64_t clientOrderId{0};
  SeqNanos expiryNs{};   // GTD expiry, sequencer time (0 = none)
  uint64_t ocoGroup{0};  // OCO group (0 = none)
  // Mirrors RestingOrder::cumQty -- total filled over the order's
  // life so far, so a cancel reported after a restart still knows its real
  // CumQty instead of resetting to 0 across a recovery. Appended at the
  // end (the explicit alignment-padding convention); 8-byte Quantity after
  // the 8-byte ocoGroup introduces no new padding.
  Quantity cumQty{};
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
  uint8_t pad0_[3]{};  // explicit alignment padding
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
  uint8_t mode{};      // STPMode
  uint8_t pad0_[7]{};  // explicit alignment padding, tail
};

struct RestoreHeld  // one open last-look hold (mirrors MatchingEngine::Held)
{
  uint64_t heldId{};
  OrderId taker{};
  uint64_t takerAccount{};
  Side takerSide{};
  uint8_t pad0_[4]{};  // explicit alignment padding
  OrderId maker{};
  uint64_t makerAccount{};
  Price price{};
  Quantity qty{};
  SeqNanos deadline{};
  TimeInForce takerTif{TimeInForce::GTC};
  OrderType takerType{OrderType::LIMIT};
  uint8_t pad1_[6]{};  // explicit alignment padding
  Price takerPrice{};
  SeqNanos takerExpiryNs{};
  bool makerReduceOnly{false};
  bool takerReduceOnly{false};
  // Whether the maker is in the engine's live-order tracking maps. Normally
  // true (a held maker stays tracked even fully off the book), but an
  // STP-cancel can legally remove a maker while its hold stays open -- the
  // write side records the live truth instead of re-deriving it.
  bool makerTracked{true};
  uint8_t pad2_[5]{};  // explicit alignment padding
  // Reference when the hold was taken; the move that last look is about is
  // measured from here. Appended -- the record grows, and this module has not
  // shipped.
  int64_t refAtHoldRaw{0};
  // The identifiers the submitters gave the two legs, so a hold that resolves
  // after a restart reports under the names they chose.
  uint64_t makerClientOrderId{0};
  uint64_t takerClientOrderId{0};
  // Each leg's CONFIRMED cumulative fill as of the moment this hold
  // was taken (mirrors Held::makerCumQtyAtHold/takerCumQtyAtHold). Needed so
  // a hold that resolves after a restart reports the same FIX 14 (CumQty) it
  // would have without the restart, on both the reject-restore path
  // (rebuilding a maker/taker that left the book entirely) and FillRejected
  // itself. Appended -- both new fields are 8-byte Quantity after two 8-byte
  // uint64_t, so no new padding.
  Quantity makerCumQtyAtHold{};
  Quantity takerCumQtyAtHold{};
};

struct RestorePosition  // one perp position (qty, average entry, posted margin)
{
  uint64_t account{};
  int64_t qtyRaw{0};    // signed contracts (Quantity raw)
  int64_t entryRaw{0};  // average entry price (Price raw)
  uint8_t pad0_[8]{};   // explicit alignment padding
  Amount marginRaw{0};  // posted position margin (quote raw); re-reserved on apply
};

struct RestoreMmpCfg  // market-maker-protection config for one account.
{
  // The mmp FILL WINDOWS restore EMPTY by design: the sliding window is
  // shorter than any realistic checkpoint interval, so the lost tail of the
  // window cannot span a restart (see docs/venue/runtime.md).
  uint64_t account{};
  Quantity qtyLimit{};
  DurationNs windowNs{};
};

inline constexpr uint32_t kClOrdIdBatch = 32;

struct RestoreClOrdIds  // fixed-size batch of an account's clientOrderId dedup set
{
  // The set can be large; it serializes as repeated fixed-size batches so the
  // journal's strictly-sized blittable body model is preserved.
  uint64_t account{};
  uint32_t count{0};  // ids[0..count) valid, count <= kClOrdIdBatch
  // Which half of the rotating window these ids belong to (0 = current,
  // 1 = previous). Written into padding that was already there, so the record
  // keeps its size and the journal layout fingerprint does not move -- only
  // what the bytes MEAN changed, which is what the snapshot version is for.
  uint32_t generation{0};
  uint64_t ids[kClOrdIdBatch]{};
  // When this account's window last rotated, in sequencer time. State, not
  // bookkeeping: it decides WHEN the next half is dropped, and the state hash
  // folds it -- so a snapshot without it restores every id and then rotates
  // on a schedule of its own, which SnapshotEnd refuses. Repeated on every
  // batch of the same account (they all carry the one value); an account
  // whose halves are both empty cannot occur, so no account is left without a
  // record to carry it (see ClOrdIdWindow::duplicate).
  int64_t rotatedAtNs{0};
};

// One buying-power reservation entry (engine::Credit::Reservation), serialized
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
  uint8_t pad0_[2]{};  // explicit alignment padding
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
  uint8_t pad0_[6]{};      // explicit alignment padding
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
  uint32_t count{0};   // entries [0..count) valid, count <= kMmpFillBatch
  uint8_t pad0_[4]{};  // explicit alignment padding
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
  int64_t fundingRateRaw{0};       // last applied rate, kFundingRateScale
  SeqNanos nextFundingNs{};        // next settlement boundary (0 = no schedule set)
  DurationNs fundingIntervalNs{};  // schedule interval (0 = no schedule set)
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
  uint8_t pad0_[7]{};  // explicit alignment padding
  int64_t markPriceRaw{0};
  bool hasMark{false};
  uint8_t pad1_[7]{};      // explicit alignment padding
  int64_t haltUntilNs{0};  // pending timed (LULD) halt deadline (0 = none)
};

// The order below is the variant's own and carries no promise: journal tags
// are explicit (see kWireTag), so alternatives may be reordered freely. The
// history of WHY the order looks as it does is still worth keeping, because it
// explains why live and snapshot-only tags interleave: SetTriggerRef postdates
// TimeTick; the snapshot-only block postdates SetTriggerRef;
// RestoreBalance/RestoreMmpFills and the live SetStpGroup postdate that block;
// SetFundingSchedule (live) and RestoreFunding (snapshot-only) came as a pair.
//
// What is still append-only is the TAG SPACE: a tag that has been on disk may
// never be reused for a different command.
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
  uint8_t pad0_[4]{};  // explicit alignment padding
  int64_t qtyRaw{};    // 0 = the whole position
};

// Why a position was corrected by hand. The venue's books and an external
// record of the same positions drift for ordinary reasons -- a counterparty
// reports a fill the venue never saw, a settlement lands differently, a
// migration brings history in -- and the correction has to say which, or the
// next person to look at it cannot tell a reconciliation from a mistake.
enum class AdjustReason : uint8_t
{
  Reconciliation = 0,    // periodic comparison against an external record
  CounterpartyReport,    // a counterparty reported what the venue did not see
  SettlementCorrection,  // settlement differed from what was booked
  Migration,             // history brought in from elsewhere
  Manual,                // operator judgement; the note carries the rest
};
inline constexpr size_t kAdjustReasons = 5;

// Note length is fixed because the record is memcpy'd into the journal like
// every other body; 32 bytes is a sentence, not an essay, and an essay belongs
// in the operator's own record rather than in the matching path.
inline constexpr size_t kAdjustNoteLen = 32;

// Correct a position by hand, journaled like any other command so the
// correction replays and a later reader can see it happened and why.
//
// Deliberately NOT a trade: no PnL is realized, no fee is charged, the ledger
// is not touched and posted margin is left alone. The discrepancy being
// corrected is by definition not backed by a fill, so inventing the cash flow
// that a fill would have produced would make the books agree by adding a
// second error. Margin that no longer fits the corrected size is the
// operator's next decision, not this command's business -- and the event says
// what the position became so that decision can be made.
struct AdjustPosition
{
  uint64_t accountId{};
  SymbolId symbol{};
  uint8_t pad0_[4]{};      // explicit alignment padding
  int64_t qtyDeltaRaw{0};  // signed; added to the existing position
  int64_t entryRaw{0};     // 0 = keep the current average entry; else set it
  AdjustReason reason{AdjustReason::Reconciliation};
  char note[kAdjustNoteLen]{};
  uint8_t pad1_[7]{};  // explicit alignment padding, tail
};

using InboundCommand =
    std::variant<NewOrder, CancelOrder, ModifyOrder, MassCancel, Quote, LastLookDecision, SetMark,
                 ApplyFunding, AdminCmd, Deposit, Withdraw, ListInstrument, SetBands, TimeTick,
                 SetTriggerRef, SnapshotBegin, RestoreOrder, RestoreStop, RestorePeg, RestoreHeld,
                 RestorePosition, RestoreMmpCfg, RestoreClOrdIds, SnapshotEnd, RestoreReservation,
                 RestoreBalance, RestoreMmpFills, SetStpGroup, SetFundingSchedule, RestoreFunding,
                 ForceClosePosition, RestoreOrderStp, SetAdmissionProfile, SetRiskLimits,
                 AdjustPosition, QuoteLadder, SetAccountRiskLimits>;

// The wire tag of each alternative, by its position in the variant.
//
// It used to BE the position: `tag = c.index()`. That made the variant's
// declaration order a format promise, and the promise could only be kept by a
// rule written in a comment -- append only, never reorder. Break it and an old
// journal is not rejected, it is re-read as DIFFERENT commands, which is the
// worst failure a format can have.
//
// The numbers below are the ones already on disk, so nothing moves today. What
// changes is that they are now written down instead of inferred, and the
// variant can be reordered freely: the tag travels with the alternative rather
// than with its position.
inline constexpr uint8_t kWireTag[] = {
    0,   // NewOrder
    1,   // CancelOrder
    2,   // ModifyOrder
    3,   // MassCancel
    4,   // Quote
    5,   // LastLookDecision
    6,   // SetMark
    7,   // ApplyFunding
    8,   // AdminCmd
    9,   // Deposit
    10,  // Withdraw
    11,  // ListInstrument
    12,  // SetBands
    13,  // TimeTick
    14,  // SetTriggerRef
    15,  // SnapshotBegin
    16,  // RestoreOrder
    17,  // RestoreStop
    18,  // RestorePeg
    19,  // RestoreHeld
    20,  // RestorePosition
    21,  // RestoreMmpCfg
    22,  // RestoreClOrdIds
    23,  // SnapshotEnd
    24,  // RestoreReservation
    25,  // RestoreBalance
    26,  // RestoreMmpFills
    27,  // SetStpGroup
    28,  // SetFundingSchedule
    29,  // RestoreFunding
    30,  // ForceClosePosition
    31,  // RestoreOrderStp
    32,  // SetAdmissionProfile
    33,  // SetRiskLimits
    34,  // AdjustPosition
    35,  // QuoteLadder
    36,  // SetAccountRiskLimits
};
static_assert(std::size(kWireTag) == std::variant_size_v<InboundCommand>,
              "every alternative needs a wire tag, and only alternatives have one");

// Two alternatives sharing a tag would make one unreadable and the other
// ambiguous, and nothing at runtime could tell which had been written.
consteval bool wireTagsAreUnique()
{
  for (size_t a = 0; a < std::size(kWireTag); ++a)
  {
    for (size_t b = a + 1; b < std::size(kWireTag); ++b)
    {
      if (kWireTag[a] == kWireTag[b])
      {
        return false;
      }
    }
  }
  return true;
}
static_assert(wireTagsAreUnique(), "two InboundCommand alternatives share a wire tag");

inline uint8_t wireTagOf(const InboundCommand& c) noexcept
{
  return kWireTag[c.index()];
}

// The ladder's own tag, named rather than spelled as a number in the two
// places that have to treat it apart from the rest (its body length is a
// property of the record, not of its type -- see quoteLadderBodySize). Read
// off the variant, so it follows the alternative wherever the alternative
// goes.
inline constexpr uint8_t kQuoteLadderWireTag = kWireTag[InboundCommand{QuoteLadder{}}.index()];

// Snapshot-only records, by TAG rather than by a range of variant positions.
// The range was the same conflation in another place: reordering the variant
// silently changed which records a client was allowed to send.
inline constexpr uint8_t kSnapshotOnlyTags[] = {15, 16, 17, 18, 19, 20, 21,
                                                22, 23, 24, 25, 26, 29};

inline bool isSnapshotRecord(const InboundCommand& c) noexcept
{
  // A snapshot-only record this predicate does not recognise is treated as
  // live traffic: accepted from a client, journaled into the live stream, and
  // replayed as a command.
  static_assert(std::variant_size_v<InboundCommand> == 37,
                "new InboundCommand alternative: if it is snapshot-only, add its tag to "
                "kSnapshotOnlyTags -- otherwise it is treated as live traffic a client may send");
  const uint8_t tag = wireTagOf(c);
  for (const uint8_t t : kSnapshotOnlyTags)
  {
    if (t == tag)
    {
      return true;
    }
  }
  return false;
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
  // The identifier the submitter gave the order (0 = none). A submitter
  // reconciles reports against the identifier it chose, not the one the venue
  // assigned, so every report about an order carries it.
  uint64_t clientOrderId{0};
  // FIX 14 (CumQty) -- appended field, wire codecs place it last. Most
  // accepts are 0 (a fresh order has filled nothing yet); nonzero when a
  // triggered stop or a crossing new order fills part of itself BEFORE the
  // residual rests (MatchOutcome::filled at the moment this accept fires).
  Quantity cumQty{};
};

struct OrderRejected
{
  OrderId id{};
  SymbolId symbol{};
  RejectReason reason{};
  uint64_t account{0};  // owner (appended: delivery routing)
  // The identifier the submitter gave the order (0 = none). A submitter
  // reconciles reports against the identifier it chose, not the one the venue
  // assigned, so every report about an order carries it.
  uint64_t clientOrderId{0};
  // FIX 14 (CumQty) on this report -- appended field, wire codecs
  // place it last. Currently always 0 with this engine's own call sites:
  // MatchOutcome::reject (matcher.h cross()/crossProRata()) is set only
  // BEFORE the fill loop runs, on both policies, so every existing
  // OrderRejected fires pre-trade -- a fill-time risk re-check or an STP
  // block that stops a partial residual reports through OrderCanceled
  // instead (see MatchOutcome::residualCanceled), never through a reject.
  // The field exists for FIX-spec completeness (14 is required on every
  // ExecutionReport) and so a future reject path that DOES follow a partial
  // fill does not silently misreport; it is not dead weight today, it is
  // unexercised today. No leavesQty field: a rejected order is never left
  // resting, so it is always 0 by construction, not merely by observation.
  Quantity cumQty{};
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
  // The identifier the submitter gave the order (0 = none). A submitter
  // reconciles reports against the identifier it chose, not the one the venue
  // assigned, so every report about an order carries it.
  uint64_t clientOrderId{0};
  // FIX 14 (CumQty) -- appended field, wire codecs place it last. The
  // total filled over this leg's whole life, AS OF this fill (inclusive): a
  // maker's running RestingOrder::cumQty after this fill, or a taker's
  // running total across its own crossing sweep (which may include prior
  // fills from before a modify re-entered matching).
  Quantity cumQty{};
};

struct OrderCanceled
{
  OrderId id{};
  SymbolId symbol{};
  CancelReason reason{};
  uint64_t account{0};  // owner (appended: delivery routing)
  // The identifier the submitter gave the order (0 = none). A submitter
  // reconciles reports against the identifier it chose, not the one the venue
  // assigned, so every report about an order carries it.
  uint64_t clientOrderId{0};
  // FIX 151/14 on this report -- both appended fields (wire codecs
  // place them last). leavesQty is what was actually killed (the residual
  // that never traded and never rests again); cumQty is the total this
  // order filled over its whole life, before this cancel. A counterparty
  // that reads LeavesQty off terminal reports (routine for an IOC/FOK
  // residual) previously had no way to tell "the order filled completely"
  // from "the remainder was silently canceled" -- this is the fix.
  Quantity leavesQty{};
  Quantity cumQty{};
};

struct OrderModified
{
  OrderId id{};
  SymbolId symbol{};
  Price price{};
  Quantity leavesQty{};
  bool priorityKept{false};  // false = re-entered at the tail (lost time priority)
  uint64_t account{0};       // owner (appended: delivery routing)
  // The identifier the submitter gave the order (0 = none). A submitter
  // reconciles reports against the identifier it chose, not the one the venue
  // assigned, so every report about an order carries it.
  uint64_t clientOrderId{0};
  // FIX 14 (CumQty) -- appended field, wire codecs place it last. The
  // order's running total filled over its whole life, unaffected by this
  // modify (a reprice/resize never trades); carried across a re-enter
  // (price/qty change) so an order that filled before being amended does
  // not report cumQty resetting to 0.
  Quantity cumQty{};
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
  // Side of the aggressor that caused the hold. Appended -- wire codecs place
  // it after the fields above.
  //
  // The engine has it (it is the taker's own side) and used to keep it to
  // itself, so anything acting on a hold had to rebuild it: remember the
  // maker's side from its OrderAccepted, hold a map of sides for orders that
  // may never be hit, and refuse a hold whose maker it never saw. The maker's
  // side is the opposite of this one, so the byte answers all three.
  Side takerSide{};
  // The identifier the TAKER gave its order (0 = none). Appended after
  // takerSide -- wire codecs place it last. A hold's own OrderID (heldId)
  // names the hold itself, not the order that caused it, so a client that
  // split a parent order into venue-level children still had to keep a
  // 37->11 map of its own just to recognise a hold on one of them; the child
  // leg gave the venue its name at submission and the venue had it in the
  // Held record all along.
  uint64_t clientOrderId{0};
  // FIX 14 (CumQty) -- appended field, wire codecs place it last. The
  // taker's CONFIRMED fill total as of the moment this hold opened -- prior
  // real (non-held) fills earlier in the same crossing sweep, if any. Never
  // includes this hold's own qty (still pending, not yet confirmed) or a
  // sibling hold on the same taker that has not resolved yet. Mirrors
  // Held::takerCumQtyAtHold (engine/last_look.h).
  Quantity cumQty{};
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
  // The identifier the TAKER gave its order (0 = none). Appended after
  // makerAccount -- same reasoning as FillHeld.clientOrderId: the hold's
  // reject is the report that never gets a second chance, so this is where a
  // client that named its own order needs the name back most.
  uint64_t clientOrderId{0};
  // FIX 14 (CumQty) -- appended field, wire codecs place it last. Same
  // value FillHeld reported when this hold opened (Held::takerCumQtyAtHold):
  // the taker's confirmed fill total as of hold creation, not touched by
  // this rejection since nothing here traded.
  Quantity cumQty{};
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

// The one conversion from the rate a journaled ApplyFunding carries into the
// raw the venue publishes, hashes and settles on. The body stays a double --
// it is what a rate calculator produces and what an operator types, it is
// blittable, and it round-trips through the journal bit for bit -- so the
// arithmetic has to become integer exactly once, here, at the boundary.
//
// Round to nearest, not truncate. 0.0003 is stored as 0.00029999999999999997,
// so truncating the scaled value publishes a rate of 0.00029999 where 0.03%
// was set, and charges a raw less than is owed on every interval forever.
inline int64_t fundingRateRawOf(double rate)
{
  return roundDoubleToI64(rate * static_cast<double>(kFundingRateScale));
}

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
  SeqNanos nextFundingNs{};   // next funding boundary (0 = no funding interval configured)
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

// A position was corrected by hand. Carries what it became, not only what
// changed, so a reader does not have to replay to know where it ended up.
struct PositionAdjusted
{
  uint64_t account{};
  SymbolId symbol{};
  int64_t qtyDeltaRaw{0};
  int64_t qtyAfterRaw{0};
  int64_t entryAfterRaw{0};
  AdjustReason reason{AdjustReason::Reconciliation};
  char note[kAdjustNoteLen]{};
};

using OutboundEvent =
    std::variant<OrderAccepted, OrderRejected, Trade, OrderExecuted, OrderCanceled, OrderModified,
                 OrderTriggered, FillHeld, FillRejected, MmpTriggered, FeeCharged, Liquidation,
                 BalanceUpdate, TradingStatusChanged, DerivativesUpdated, CancelRejected,
                 PositionAdjusted>;

}  // namespace flox::venue
