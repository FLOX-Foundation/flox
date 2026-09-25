/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Compile-time audit of the connector configs: a config field that nothing
 * reads is a lie in the public API, and for privateKey/funderWallet it is also
 * a plaintext credential that an operator is invited to write into a config
 * file which no code path ever consumes -- the executor takes both through its
 * constructor instead.
 *
 * This pins the *removal* side of the choice, which is what the fields'
 * own acceptance criterion asked for ("Unread config fields removed, or
 * actually consumed by the connector/executor") and what was reported done
 * while the fields stayed. If the fields are instead wired up for real, this
 * expectation is the thing to rewrite -- into one that observes the use.
 */

#include "flox-connectors/hyperliquid/hyperliquid_exchange_connector.h"
#include "flox-connectors/polymarket/polymarket_config.h"

#include <gtest/gtest.h>

#include <type_traits>

using namespace flox;

namespace
{

template <typename C>
concept HasPrivateKey = requires(C& c) { c.privateKey; };

template <typename C>
concept HasFunderWallet = requires(C& c) { c.funderWallet; };

template <typename C>
concept HasRestEndpoint = requires(C& c) { c.restEndpoint; };

}  // namespace

TEST(PolymarketConfigFields, DeadCredentialFieldsAreGone)
{
  EXPECT_FALSE(HasPrivateKey<PolymarketConfig>)
      << "PolymarketConfig::privateKey has no reader; the executor takes the key via its ctor";
  EXPECT_FALSE(HasFunderWallet<PolymarketConfig>)
      << "PolymarketConfig::funderWallet has no reader; the executor takes it via its ctor";
}

TEST(PolymarketConfigFields, DeadRestEndpointIsGone)
{
  EXPECT_FALSE(HasRestEndpoint<PolymarketConfig>)
      << "PolymarketConfig::restEndpoint has no reader; the connector is WS-only";
}

// Same class of dead field, same audit, and the header comment says so out
// loud ("The connector is WS-only; it never reads restEndpoint").
TEST(HyperliquidConfigFields, DeadRestEndpointIsGone)
{
  EXPECT_FALSE(HasRestEndpoint<HyperliquidConfig>)
      << "HyperliquidConfig::restEndpoint has no reader; the connector is WS-only";
}

// Control (green today): the fields that are genuinely read stay, and isValid
// gates on the one that matters.
TEST(PolymarketConfigFields, LiveFieldsStayAndGateValidity)
{
  static_assert(requires(PolymarketConfig& c) { c.wsEndpoint; });
  static_assert(requires(PolymarketConfig& c) { c.tokenIds; });
  static_assert(requires(PolymarketConfig& c) { c.reconnectDelayMs; });

  PolymarketConfig cfg;
  EXPECT_TRUE(cfg.isValid());
  cfg.wsEndpoint.clear();
  EXPECT_FALSE(cfg.isValid());
}
