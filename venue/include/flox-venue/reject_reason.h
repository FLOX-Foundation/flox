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
};

const char* toString(RejectReason r) noexcept;
const char* toString(CancelReason r) noexcept;

}  // namespace flox::venue
