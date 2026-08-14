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
  // Session-level admission rejects, surfaced to the client as an exec-report
  // reject instead of silence (appended values -- wire enum is append-only).
  RateLimited,       // per-session rate limit tripped; the command was not admitted
  MalformedMessage,  // frame did not decode to a valid command
  Unauthenticated,   // command before a successful logon
  SessionSeqGap,     // FIX: inbound MsgSeqNum gap -- session must resync
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
};

const char* toString(RejectReason r) noexcept;
const char* toString(CancelReason r) noexcept;

}  // namespace flox::venue
