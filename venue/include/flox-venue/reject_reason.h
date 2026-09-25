/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include <cstdint>

namespace flox::venue
{

enum class RejectReason : uint8_t
{
  None = 0,
  UnknownSymbol,
  Halted,
  InvalidQuantity,
  InvalidPrice,
  TickSizeViolation,
  LotSizeViolation,
  DuplicateOrderId,
  UnknownOrder,
  PostOnlyWouldCross,
  FillOrKillUnfulfillable,
  OrderTooLarge,           // fat-finger: exceeds max order qty / notional
  InsufficientFunds,       // pre-trade credit / buying-power check failed
  LuldBreach,              // price outside the limit-up/limit-down volatility band
  PositionLimitExceeded,   // would grow the account's position past the symbol cap
  TooManyOpenOrders,       // account already at its max live-order count (ingress DoS/risk gate)
  NotOrderOwner,           // last-look decision from an account that does not own the held maker order
  DuplicateClientOrderId,  // clientOrderId already used by this account this session (resend dedup)
  LastLookUnsupported,     // lastLook order on a pro-rata instrument (allocation would not honour the hold)
  NoLedgerBound,           // a money command reached an engine that holds no ledger (funds live elsewhere)
  // Session-level admission rejects, surfaced to the client as an exec-report
  // reject instead of silence (appended values -- wire enum is append-only).
  RateLimited,       // per-session rate limit tripped; the command was not admitted
  MalformedMessage,  // frame did not decode to a valid command
  Unauthenticated,   // command before a successful logon
  SessionSeqGap,     // FIX: inbound MsgSeqNum gap -- session must resync
  // The instrument's SESSION is closed -- distinct from Halted, which is an
  // operator exception. A client retries after the next session opens; a halt
  // has no such promise. Appended value (the wire enum is append-only).
  MarketClosed,
  // Admission profile: what a counterparty is entitled to send. A broker
  // routing flow it manages itself must not be able to leave an order resting
  // in a book it does not know it owns, so the entitlement is refused on
  // admission rather than assumed by agreement (appended -- wire enum is
  // append-only).
  OrderTypeNotPermitted,    // the profile does not allow this order type
  TimeInForceNotPermitted,  // the profile does not allow this time in force
  RestingNotPermitted,      // the profile forbids leaving a residual on the book
  AmendNotPermitted,        // the profile forbids modify
  CancelNotPermitted,       // the profile forbids cancel
  QuoteNotPermitted,        // the profile forbids mass quoting
  // Withdrawn from trading with no scheduled return, unlike Halted (comes back)
  // or MarketClosed (next session). Appended -- wire enum is append-only.
  InstrumentDelisted,
  // Refused by the risk owner outside the engine (setCreditCheck), for its own
  // reasons rather than for anything the engine can see. Appended -- the wire
  // enum is append-only.
  //
  // These exist because the alternative was borrowing: an external owner that
  // refused because an account was suspended had to answer InsufficientFunds,
  // and one whose limit source had gone quiet had to answer MarketClosed. Both
  // are false, and a client reading the text acts on the wrong thing -- tops
  // up an account that is not short of money, or waits for a session that
  // never closed.
  CreditRefused,            // the risk owner said no; the reason is its own
  CreditSourceUnavailable,  // the risk owner could not decide: limits unreachable
  // An operator correction (AdjustPosition) that the engine will not make.
  // Appended -- the wire enum is append-only.
  //
  // A correction that neither moves the size nor sets an entry says nothing,
  // and a correction that creates a position out of nothing has no average
  // entry to give it: a zero entry would make every later PnL wrong, quietly,
  // which is worse than refusing to do it.
  AdjustmentEmpty,       // neither a size delta nor an entry was given
  AdjustmentNeedsEntry,  // no position to adjust, and no entry to open one at
  // A quotes-only counterparty (AdmissionDeny::DenyNewOrder) sent a
  // plain order. Appended -- the wire enum is append-only.
  NewOrderNotPermitted,  // the profile forbids NewOrder
  // An order type outside the values OrderType names. Distinct from
  // OrderTypeNotPermitted, which is a profile's answer about a type that
  // exists: this one is the venue's, and no profile has to be configured for
  // it. Appended -- the wire enum is append-only.
  //
  // It has its own value because the two ask the counterparty for different
  // things. "Not permitted" means talk to whoever set your entitlements;
  // "unknown" means your encoder and this venue disagree about the schema,
  // and no entitlement change will help.
  UnknownOrderType,
  // the resting book had no room for the order. A bounded book
  // (LadderBook's node pool) is a venue-side limit and nothing the client did
  // wrong, so it is not InsufficientFunds, not OrderTooLarge and not
  // TooManyOpenOrders -- a client told any of those changes the order and
  // resends it, when the one useful action is to wait or to cancel something.
  // The out-of-band price case keeps InvalidPrice: that one IS the client's
  // price. Appended -- the wire enum is append-only.
  BookCapacityExceeded,
  // A pegged order on an instrument that declares no tick. The peg clamp
  // exists to keep a tracking order strictly inside the price it tracks, and
  // the only distance it has to step is the tick; with tickSize 0 the clamp
  // lands on the opposite touch itself. Its own reason rather than
  // TickSizeViolation, which says "your price is off the grid" and sends the
  // client back with a rounded price: there is no price this client could
  // send that would help, because the instrument -- not the order -- is what
  // is missing something. Appended -- the wire enum is append-only.
  PegRequiresTick,
};

enum class CancelReason : uint8_t
{
  UserRequested = 0,
  ImmediateOrCancelResidual,
  MarketResidual,
  SelfTradePrevention,
  Expired,
  OcoTriggered,        // canceled because a linked OCO order filled first
  VenueHalt,           // canceled by an operator emergency halt-and-cancel
  Liquidation,         // canceled because the account was liquidated (free its collateral)
  FillOrKillResidual,  // a FOK that did not fully fill -- killed, never rests
  // Fill-time perp risk re-check (the position an order was sized against can
  // move while it rests). Appended values -- the wire enum is append-only.
  ReduceOnlyNotReducing,  // the order would no longer reduce: it would open or flip the position
  PositionLimitExceeded,  // the fill would carry the account past maxPositionQty
  // the book would not take the order back. Reached by an order
  // whose residual has already traded (a reject after a print would be a lie)
  // and by the paths that lift an order off the book and put it back -- a peg
  // reprice, an amend, a last-look restore -- when the price it returns at has
  // no level or the pool filled in between. Appended -- the wire enum is
  // append-only.
  BookRefused,
};

const char* toString(RejectReason r) noexcept;
const char* toString(CancelReason r) noexcept;

}  // namespace flox::venue
