/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include "flox/backtest/amm_pricing.h"
#include "flox/connector/abstract_exchange_connector.h"

#include <memory_resource>
#include <string>

namespace flox
{

// Reference connector that presents a constant-product AMM pool as an
// IExchangeConnector. It maps pool state into the same BookUpdateEvent and
// TradeEvent the rest of the engine consumes, so the DEX pieces (per-symbol
// scale, on-chain lifecycle, AMM pricing, nonlinear valuation, venue-type
// routing) compose into a working venue without the core knowing it is a
// pool rather than an order book.
//
// This is the flox-side skeleton only. Sourcing reserve updates from a chain,
// signing swaps, mempool watching, gas policy, and MEV protection live in the
// downstream connector that wraps this; here the pool state is fed in
// directly so the mapping can be tested against fixtures with no network.
class AmmDexConnector : public IExchangeConnector
{
 public:
  // levels: synthetic book depth per side. levelSize: base quantity per
  // level, also the depth step used to price each level off the curve.
  AmmDexConnector(std::string name, SymbolId symbol, AmmPool pool, int levels,
                  Quantity levelSize)
      : _name(std::move(name)),
        _symbol(symbol),
        _pool(pool),
        _levels(levels),
        _levelSize(levelSize)
  {
  }

  std::string exchangeId() const override { return _name; }

  // Update the pool's reserves and publish the resulting synthetic book.
  void onPoolState(Quantity reserveBase, Quantity reserveQuote)
  {
    _pool = AmmPool(reserveBase, reserveQuote, _pool.feeBps());
    publishBook();
  }

  // Execute a swap against the pool, publish the trade, and republish the
  // book since the reserves moved. baseForQuote=true sells base into the pool.
  void onSwap(Quantity amountIn, bool baseForQuote, int64_t tsNs)
  {
    const Quantity out = _pool.applySwap(amountIn, baseForQuote);
    const double base = baseForQuote ? amountIn.toDouble() : out.toDouble();
    const double quote = baseForQuote ? out.toDouble() : amountIn.toDouble();

    TradeEvent ev;
    ev.trade.symbol = _symbol;
    ev.trade.price = base > 0.0 ? Price::fromDouble(quote / base) : Price{};
    ev.trade.quantity = Quantity::fromDouble(base);
    ev.trade.isBuy = !baseForQuote;  // buying base = quote->base
    ev.trade.exchangeTsNs = static_cast<UnixNanos>(tsNs);
    emitTrade(ev);

    publishBook();
  }

 private:
  // Synthesize a snapshot book from the curve: bids below spot and asks above,
  // each level priced by the price impact of trading that depth. The ask side
  // mirrors the bid-side impact, a reference approximation; a production
  // connector would price each side from its own swap direction.
  void publishBook()
  {
    const double spot = _pool.spotPrice().toDouble();
    if (spot <= 0.0 || _levels <= 0)
    {
      return;
    }

    BookUpdateEvent ev(std::pmr::new_delete_resource());
    ev.update.symbol = _symbol;
    ev.update.type = BookUpdateType::SNAPSHOT;
    ev.update.bids.reserve(static_cast<size_t>(_levels));
    ev.update.asks.reserve(static_cast<size_t>(_levels));

    for (int i = 1; i <= _levels; ++i)
    {
      const Quantity depth = Quantity::fromDouble(_levelSize.toDouble() * i);
      const double impact = _pool.priceImpact(depth, true);
      ev.update.bids.emplace_back(Price::fromDouble(spot * (1.0 - impact)), _levelSize);
      ev.update.asks.emplace_back(Price::fromDouble(spot * (1.0 + impact)), _levelSize);
    }

    emitBookUpdate(ev);
  }

  std::string _name;
  SymbolId _symbol;
  AmmPool _pool;
  int _levels;
  Quantity _levelSize;
};

}  // namespace flox
