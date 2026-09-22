/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * The one command stream the process-death scenario is built on.
 *
 * It lives here rather than in the test because two programs have to agree on
 * it exactly: the test, and the helper binary that writes the journal and then
 * dies. Two copies of "the same" stream would drift, and the drift would look
 * like a recovery bug.
 */
#pragma once

#include "flox-venue/ledger.h"
#include "flox-venue/messages.h"

#include <cstdint>
#include <vector>

namespace flox::venue::test
{

constexpr SymbolId kScenarioSymbol = 1;
constexpr AssetId kScenarioBase = 0;
constexpr AssetId kScenarioQuote = 1;
constexpr uint64_t kScenarioVenueAccount = 900;

inline venue::SymbolConfig scenarioConfig()
{
  venue::SymbolConfig c;
  c.id = kScenarioSymbol;
  c.tickSize = Price::fromDouble(0.01);
  c.minPrice = Price::fromDouble(50.0);
  c.maxPrice = Price::fromDouble(150.0);
  c.baseAsset = kScenarioBase;
  c.quoteAsset = kScenarioQuote;
  return c;
}

inline NewOrder scenarioLimit(OrderId id, Side s, double p, double q, uint64_t acct)
{
  NewOrder o;
  o.id = id;
  o.symbol = kScenarioSymbol;
  o.side = s;
  o.type = OrderType::LIMIT;
  o.price = Price::fromDouble(p);
  o.quantity = Quantity::fromDouble(q);
  o.accountId = acct;
  return o;
}

// Journaled genesis deposits, then crossing flow that leaves resting orders
// and reservations behind -- so a recovery has something to get wrong.
inline std::vector<InboundCommand> scenarioCommands()
{
  const auto baseRaw = [](double v)
  { return static_cast<int64_t>(amountOf(Quantity::fromDouble(v))); };
  const auto quoteRaw = [](double v)
  { return static_cast<int64_t>(amountOf(Volume::fromDouble(v))); };

  std::vector<InboundCommand> v;
  v.emplace_back(Deposit{1, kScenarioBase, {}, baseRaw(1000), kScenarioSymbol});
  v.emplace_back(Deposit{2, kScenarioQuote, {}, quoteRaw(100000), kScenarioSymbol});
  for (uint64_t i = 0; i < 20; ++i)
  {
    v.emplace_back(scenarioLimit(100 + 2 * i, Side::SELL,
                                 100.0 + static_cast<double>(i % 5) * 0.01, 1.0, 1));
    v.emplace_back(scenarioLimit(101 + 2 * i, Side::BUY,
                                 100.0 + static_cast<double>(i % 5) * 0.01, 0.5, 2));
  }
  v.emplace_back(CancelOrder{100, kScenarioSymbol, {}, 1});
  v.emplace_back(Withdraw{2, kScenarioQuote, {}, quoteRaw(10), kScenarioSymbol});
  return v;
}

}  // namespace flox::venue::test
