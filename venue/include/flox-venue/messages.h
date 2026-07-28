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

#include "flox-venue/reject_reason.h"

#include <cstdint>
#include <variant>

namespace flox::venue
{

// Saturating double -> fixed-point conversion for UNTRUSTED input (REST/FIX
// numeric fields). Decimal::fromDouble multiplies by Scale and casts to int64;
// a hostile value like 5e52 would overflow that cast (undefined behavior). Clamp
// to a safe range and neutralize NaN before converting.
template <class D>
inline D safeDecimal(double v)
{
  const double kMax = 9.0e18 / static_cast<double>(D::Scale);  // < INT64_MAX after *Scale
  if (!(v == v))
  {
    v = 0.0;  // NaN
  }
  if (v > kMax)
  {
    v = kMax;
  }
  else if (v < -kMax)
  {
    v = -kMax;
  }
  return D::fromDouble(v);
}

// ---- Inbound commands -----------------------------------------------------

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
};

struct AdminCmd
{
  SymbolId symbol{};
  AdminAction action{};
};

using InboundCommand = std::variant<NewOrder, CancelOrder, ModifyOrder, MassCancel, Quote,
                                    LastLookDecision, SetMark, ApplyFunding, AdminCmd>;

// ---- Outbound events ------------------------------------------------------

struct OrderAccepted  // order accepted / working
{
  OrderId id{};
  SymbolId symbol{};
  Side side{};
  Price price{};
  Quantity leavesQty{};      // full working quantity (for the owner's ack)
  bool restingOnBook{true};  // false = working but not on the visible book (pending stop)
  Quantity displayQty{};     // publicly visible size (== leavesQty unless iceberg; 0 = use leavesQty)
};

struct OrderRejected
{
  OrderId id{};
  SymbolId symbol{};
  RejectReason reason{};
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
};

struct OrderCanceled
{
  OrderId id{};
  SymbolId symbol{};
  CancelReason reason{};
};

struct OrderModified
{
  OrderId id{};
  SymbolId symbol{};
  Price price{};
  Quantity leavesQty{};
  bool priorityKept{false};  // false = re-entered at the tail (lost time priority)
};

struct OrderTriggered  // a stop / take-profit activated and was injected into matching
{
  OrderId id{};
  SymbolId symbol{};
  Price refPrice{};  // reference (last-trade) price that crossed the trigger
};

struct FillHeld  // last-look: a fill is held pending the maker's decision
{
  uint64_t heldId{};
  SymbolId symbol{};
  OrderId makerId{};
  OrderId takerId{};
  Price price{};
  Quantity qty{};
};

struct FillRejected  // last-look: the held fill was rejected (or timed out)
{
  uint64_t heldId{};
  SymbolId symbol{};
  OrderId takerId{};
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

using OutboundEvent =
    std::variant<OrderAccepted, OrderRejected, Trade, OrderExecuted, OrderCanceled, OrderModified,
                 OrderTriggered, FillHeld, FillRejected, MmpTriggered, FeeCharged, Liquidation>;

}  // namespace flox::venue
