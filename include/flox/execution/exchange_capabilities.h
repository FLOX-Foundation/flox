/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/common.h"

namespace flox
{

struct ExchangeCapabilities
{
  // Order types
  bool supportsStopMarket{true};
  bool supportsStopLimit{true};
  bool supportsTakeProfitMarket{true};
  bool supportsTakeProfitLimit{true};
  bool supportsTrailingStop{false};
  bool supportsIceberg{false};

  // OCO
  bool supportsOCO{false};

  // Time-in-force
  bool supportsGTC{true};
  bool supportsIOC{true};
  bool supportsFOK{true};
  bool supportsGTD{false};
  bool supportsPostOnly{true};

  // Execution flags
  bool supportsReduceOnly{true};
  bool supportsClosePosition{false};

  bool supports(OrderType type) const
  {
    switch (type)
    {
      case OrderType::LIMIT:
      case OrderType::MARKET:
        return true;
      case OrderType::STOP_MARKET:
        return supportsStopMarket;
      case OrderType::STOP_LIMIT:
        return supportsStopLimit;
      case OrderType::TAKE_PROFIT_MARKET:
        return supportsTakeProfitMarket;
      case OrderType::TAKE_PROFIT_LIMIT:
        return supportsTakeProfitLimit;
      case OrderType::TRAILING_STOP:
        return supportsTrailingStop;
      case OrderType::ICEBERG:
        return supportsIceberg;
      default:
        return false;
    }
  }

  bool supports(TimeInForce tif) const
  {
    switch (tif)
    {
      case TimeInForce::GTC:
        return supportsGTC;
      case TimeInForce::IOC:
        return supportsIOC;
      case TimeInForce::FOK:
        return supportsFOK;
      case TimeInForce::GTD:
        return supportsGTD;
      case TimeInForce::POST_ONLY:
        return supportsPostOnly;
      default:
        return false;
    }
  }

  static ExchangeCapabilities all()
  {
    ExchangeCapabilities caps;
    caps.supportsTrailingStop = true;
    caps.supportsIceberg = true;
    caps.supportsOCO = true;
    caps.supportsGTD = true;
    caps.supportsClosePosition = true;
    return caps;
  }

  static ExchangeCapabilities simulated() { return all(); }
};

}  // namespace flox
