/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include "flox/common.h"
#include "flox/engine/abstract_subsystem.h"
#include "flox/strategy/symbol_state_map.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace flox
{

template <size_t MaxExchanges = 8>
class AggregatedPositionTracker : public ISubsystem
{
 public:
  struct PositionSnapshot
  {
    Quantity quantity{};
    Price avgEntryPrice{};
    Volume costBasis{};
  };

  PositionSnapshot position(ExchangeId exchange, SymbolId symbol) const
  {
    if (exchange >= MaxExchanges)
    {
      return {};
    }

    const auto* state = _positions[exchange].tryGet(symbol);
    if (!state)
    {
      return {};
    }

    int64_t qtyRaw = state->quantityRaw.load(std::memory_order_acquire);
    int64_t costRaw = state->costBasisRaw.load(std::memory_order_acquire);

    PositionSnapshot snap;
    snap.quantity = Quantity::fromRaw(qtyRaw);
    snap.costBasis = Volume::fromRaw(costRaw);
    if (qtyRaw != 0)
    {
      snap.avgEntryPrice = Price::fromRaw(costRaw / qtyRaw * Price::Scale);
    }
    return snap;
  }

  PositionSnapshot totalPosition(SymbolId symbol) const
  {
    PositionSnapshot total{};
    int64_t totalQtyRaw = 0;
    int64_t totalCostRaw = 0;

    for (size_t ex = 0; ex < MaxExchanges; ++ex)
    {
      const auto* state = _positions[ex].tryGet(symbol);
      if (!state)
      {
        continue;
      }

      totalQtyRaw += state->quantityRaw.load(std::memory_order_acquire);
      totalCostRaw += state->costBasisRaw.load(std::memory_order_acquire);
    }

    total.quantity = Quantity::fromRaw(totalQtyRaw);
    total.costBasis = Volume::fromRaw(totalCostRaw);
    if (totalQtyRaw != 0)
    {
      total.avgEntryPrice = Price::fromRaw(totalCostRaw / totalQtyRaw * Price::Scale);
    }
    return total;
  }

  Volume unrealizedPnl(SymbolId symbol, Price currentPrice) const
  {
    auto pos = totalPosition(symbol);
    if (pos.quantity.raw() == 0)
    {
      return Volume{};
    }
    // PnL = qty * (current - avg)
    Price diff = currentPrice - pos.avgEntryPrice;
    return Volume::fromRaw((pos.quantity.raw() * diff.raw()) / Price::Scale);
  }

  void onFill(ExchangeId exchangeId, SymbolId symbol, Quantity filledQty, Price fillPrice)
  {
    if (exchangeId >= MaxExchanges) [[unlikely]]
    {
      return;
    }

    auto& pos = _positions[exchangeId][symbol];

    int64_t qtyRaw = pos.quantityRaw.load(std::memory_order_relaxed);
    int64_t costRaw = pos.costBasisRaw.load(std::memory_order_relaxed);

    int64_t fillQtyRaw = filledQty.raw();
    int64_t fillPriceRaw = fillPrice.raw();

    if (fillQtyRaw > 0)
    {
      // Buy: cost += qty * price (stored as volume, already scaled correctly)
      costRaw += (fillQtyRaw * fillPriceRaw) / Price::Scale;
      qtyRaw += fillQtyRaw;
    }
    else if (fillQtyRaw < 0)
    {
      // Sell: reduce at avg entry
      int64_t sellQty = -fillQtyRaw;
      int64_t avgEntryRaw = (qtyRaw != 0) ? (costRaw / qtyRaw * Price::Scale) : 0;
      costRaw -= (sellQty * avgEntryRaw) / Price::Scale;
      qtyRaw -= sellQty;
    }

    if (qtyRaw == 0)
    {
      costRaw = 0;
    }

    pos.costBasisRaw.store(costRaw, std::memory_order_release);
    pos.quantityRaw.store(qtyRaw, std::memory_order_release);
  }

  void reset(SymbolId symbol)
  {
    for (size_t ex = 0; ex < MaxExchanges; ++ex)
    {
      auto* state = _positions[ex].tryGet(symbol);
      if (state)
      {
        state->quantityRaw.store(0, std::memory_order_release);
        state->costBasisRaw.store(0, std::memory_order_release);
      }
    }
  }

  void resetAll()
  {
    for (size_t ex = 0; ex < MaxExchanges; ++ex)
    {
      _positions[ex].clear();
    }
  }

 private:
  struct alignas(64) AtomicPositionState
  {
    std::atomic<int64_t> quantityRaw{0};
    std::atomic<int64_t> costBasisRaw{0};
  };

  std::array<SymbolStateMap<AtomicPositionState>, MaxExchanges> _positions;
};

}  // namespace flox
