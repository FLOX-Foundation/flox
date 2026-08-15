/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox-venue/reject_reason.h"

namespace flox::venue
{

const char* toString(RejectReason r) noexcept
{
  switch (r)
  {
    case RejectReason::None:
      return "None";
    case RejectReason::UnknownSymbol:
      return "UnknownSymbol";
    case RejectReason::Halted:
      return "Halted";
    case RejectReason::InvalidQuantity:
      return "InvalidQuantity";
    case RejectReason::InvalidPrice:
      return "InvalidPrice";
    case RejectReason::TickSizeViolation:
      return "TickSizeViolation";
    case RejectReason::LotSizeViolation:
      return "LotSizeViolation";
    case RejectReason::DuplicateOrderId:
      return "DuplicateOrderId";
    case RejectReason::UnknownOrder:
      return "UnknownOrder";
    case RejectReason::PostOnlyWouldCross:
      return "PostOnlyWouldCross";
    case RejectReason::FillOrKillUnfulfillable:
      return "FillOrKillUnfulfillable";
    case RejectReason::OrderTooLarge:
      return "OrderTooLarge";
    case RejectReason::InsufficientFunds:
      return "InsufficientFunds";
    case RejectReason::LuldBreach:
      return "LuldBreach";
    case RejectReason::PositionLimitExceeded:
      return "PositionLimitExceeded";
    case RejectReason::TooManyOpenOrders:
      return "TooManyOpenOrders";
    case RejectReason::NotOrderOwner:
      return "NotOrderOwner";
    case RejectReason::DuplicateClientOrderId:
      return "DuplicateClientOrderId";
    case RejectReason::LastLookUnsupported:
      return "LastLookUnsupported";
    case RejectReason::NoLedgerBound:
      return "NoLedgerBound";
    case RejectReason::RateLimited:
      return "RateLimited";
    case RejectReason::MalformedMessage:
      return "MalformedMessage";
    case RejectReason::Unauthenticated:
      return "Unauthenticated";
    case RejectReason::SessionSeqGap:
      return "SessionSeqGap";
    case RejectReason::MarketClosed:
      return "MarketClosed";
    case RejectReason::OrderTypeNotPermitted:
      return "OrderTypeNotPermitted";
    case RejectReason::TimeInForceNotPermitted:
      return "TimeInForceNotPermitted";
    case RejectReason::RestingNotPermitted:
      return "RestingNotPermitted";
    case RejectReason::AmendNotPermitted:
      return "AmendNotPermitted";
    case RejectReason::CancelNotPermitted:
      return "CancelNotPermitted";
    case RejectReason::QuoteNotPermitted:
      return "QuoteNotPermitted";
    case RejectReason::InstrumentDelisted:
      return "InstrumentDelisted";
  }
  return "?";
}

const char* toString(CancelReason r) noexcept
{
  switch (r)
  {
    case CancelReason::UserRequested:
      return "UserRequested";
    case CancelReason::ImmediateOrCancelResidual:
      return "ImmediateOrCancelResidual";
    case CancelReason::MarketResidual:
      return "MarketResidual";
    case CancelReason::SelfTradePrevention:
      return "SelfTradePrevention";
    case CancelReason::Expired:
      return "Expired";
    case CancelReason::OcoTriggered:
      return "OcoTriggered";
    case CancelReason::VenueHalt:
      return "VenueHalt";
    case CancelReason::Liquidation:
      return "Liquidation";
    case CancelReason::FillOrKillResidual:
      return "FillOrKillResidual";
    case CancelReason::ReduceOnlyNotReducing:
      return "ReduceOnlyNotReducing";
    case CancelReason::PositionLimitExceeded:
      return "PositionLimitExceeded";
  }
  return "?";
}

}  // namespace flox::venue
