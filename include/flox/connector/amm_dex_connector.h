/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include "flox/backtest/ntoken_curve.h"
#include "flox/connector/abstract_exchange_connector.h"
#include "flox/util/int/u256.h"

#include <cstddef>
#include <memory_resource>
#include <string>

namespace flox
{

// Reference connector that presents one token pair of an n-token AMM pool as an
// IExchangeConnector. It maps the curve's exact pricing into the same
// BookUpdateEvent and TradeEvent the rest of the engine consumes, so the DEX
// pieces compose into a working venue without the core knowing it is a pool.
//
// This is the single conversion boundary: the curve speaks native-wei u256, and
// the connector converts to the engine's Quantity / Price here, using the two
// tokens' decimals. The curve is borrowed and must outlive the connector.
//
// flox-side skeleton only. Sourcing curve state from a chain, signing swaps,
// mempool, gas, and MEV live in the downstream connector that wraps this.
class AmmDexConnector : public IExchangeConnector
{
 public:
  // baseIdx / quoteIdx: the pair this connector presents (base priced in quote).
  // baseDecimals / quoteDecimals: the tokens' native decimals, for the wei ->
  // Quantity conversion. levels: synthetic book depth per side. levelSizeBaseWei:
  // base amount per level (native wei), also the depth step used to price levels.
  AmmDexConnector(std::string name, SymbolId symbol, INTokenCurve& curve, std::size_t baseIdx,
                  std::size_t quoteIdx, unsigned baseDecimals, unsigned quoteDecimals, int levels,
                  u256 levelSizeBaseWei)
      : _name(std::move(name)),
        _symbol(symbol),
        _curve(&curve),
        _baseIdx(baseIdx),
        _quoteIdx(quoteIdx),
        _baseDec(baseDecimals),
        _quoteDec(quoteDecimals),
        _levels(levels),
        _levelSize(levelSizeBaseWei)
  {
  }

  std::string exchangeId() const override { return _name; }

  // Re-point at a new curve (same pair / decimals), for a driver that advances
  // pool state by swapping in a fresh curve per snapshot. The curve is borrowed
  // and must outlive its use.
  void setCurve(INTokenCurve& curve) { _curve = &curve; }

  // Publish a synthetic book from the curve's current state, stamped at tsNs.
  // Call after the caller mutates or re-points the curve.
  void republish(int64_t tsNs = 0) { publishBook(tsNs); }

  // Execute a swap against the curve, publish the trade, and republish the book.
  // baseForQuote=true sells base into the pool. amountIn is native wei of the
  // in-token.
  void onSwap(const u256& amountIn, bool baseForQuote, int64_t tsNs)
  {
    const std::size_t i = baseForQuote ? _baseIdx : _quoteIdx;
    const std::size_t j = baseForQuote ? _quoteIdx : _baseIdx;
    const u256 out = _curve->applySwap(i, j, amountIn);

    const double base = baseForQuote ? toHuman(amountIn, _baseDec) : toHuman(out, _baseDec);
    const double quote = baseForQuote ? toHuman(out, _quoteDec) : toHuman(amountIn, _quoteDec);

    TradeEvent ev;
    ev.trade.symbol = _symbol;
    ev.trade.price = base > 0.0 ? Price::fromDouble(quote / base) : Price{};
    ev.trade.quantity = Quantity::fromDouble(base);
    ev.trade.isBuy = !baseForQuote;  // buying base = quote->base
    ev.trade.exchangeTsNs = static_cast<UnixNanos>(tsNs);
    emitTrade(ev);

    publishBook(tsNs);
  }

 private:
  static double toHuman(const u256& wei, unsigned decimals)
  {
    return std::stod(wei.toDecimalString(decimals));
  }

  // Marginal quote-per-base from a small base-in probe (a millionth of the base
  // reserve), small enough to be near-marginal, large enough that the integer
  // floor does not dominate.
  double spotPrice() const
  {
    u256 probe = _curve->balances()[_baseIdx] / u256(1000000);
    if (probe.isZero())
    {
      probe = u256(1);
    }
    const double in = toHuman(probe, _baseDec);
    if (in <= 0.0)
    {
      return 0.0;
    }
    const double out = toHuman(_curve->amountOut(_baseIdx, _quoteIdx, probe), _quoteDec);
    return out / in;
  }

  // Synthesize a snapshot book: bids below spot and asks above, each level priced
  // by the realized price impact of selling that depth of base. The ask side
  // mirrors the bid-side impact, a reference approximation.
  void publishBook(int64_t tsNs = 0)
  {
    const double spot = spotPrice();
    if (spot <= 0.0 || _levels <= 0)
    {
      return;
    }
    const Quantity levelQty = Quantity::fromDouble(toHuman(_levelSize, _baseDec));

    BookUpdateEvent ev(std::pmr::new_delete_resource());
    ev.update.symbol = _symbol;
    ev.update.type = BookUpdateType::SNAPSHOT;
    ev.update.exchangeTsNs = static_cast<UnixNanos>(tsNs);
    ev.update.bids.reserve(static_cast<std::size_t>(_levels));
    ev.update.asks.reserve(static_cast<std::size_t>(_levels));

    u256 depth(0);
    for (int k = 1; k <= _levels; ++k)
    {
      depth = depth + _levelSize;
      const double baseIn = toHuman(depth, _baseDec);
      const double quoteOut = toHuman(_curve->amountOut(_baseIdx, _quoteIdx, depth), _quoteDec);
      const double realized = baseIn > 0.0 ? quoteOut / baseIn : spot;
      const double impact = spot > 0.0 ? 1.0 - realized / spot : 0.0;
      ev.update.bids.emplace_back(Price::fromDouble(spot * (1.0 - impact)), levelQty);
      ev.update.asks.emplace_back(Price::fromDouble(spot * (1.0 + impact)), levelQty);
    }

    emitBookUpdate(ev);
  }

  std::string _name;
  SymbolId _symbol;
  INTokenCurve* _curve;
  std::size_t _baseIdx;
  std::size_t _quoteIdx;
  unsigned _baseDec;
  unsigned _quoteDec;
  int _levels;
  u256 _levelSize;
};

}  // namespace flox
