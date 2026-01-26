/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <string>
#include <vector>

namespace flox
{

struct PolymarketConfig
{
  std::string wsEndpoint{"wss://ws-subscriptions-clob.polymarket.com/ws/market"};
  std::string restEndpoint{"https://clob.polymarket.com"};

  /// Private key for signing orders (hex, with or without 0x prefix)
  std::string privateKey;

  /// Funder/proxy wallet address (0x...)
  std::string funderWallet;

  /// Token IDs to subscribe to for market data
  std::vector<std::string> tokenIds;

  int reconnectDelayMs{1000};
  int pingIntervalSec{5};

  bool isValid() const { return !wsEndpoint.empty(); }

  bool hasCredentials() const { return !privateKey.empty() && !funderWallet.empty(); }
};

}  // namespace flox
