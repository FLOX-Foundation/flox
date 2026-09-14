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
#include "flox/position/position_valuator.h"
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
      snap.avgEntryPrice = snap.costBasis / snap.quantity;
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
      total.avgEntryPrice = total.costBasis / total.quantity;
    }
    return total;
  }

  // Plug a custom valuator for nonlinear positions (AMM LP, options). When
  // null (the default), valuation is linear: qty * (current - avg).
  void setValuator(const IPositionValuator* valuator) { _valuator = valuator; }

  Volume unrealizedPnl(SymbolId symbol, Price currentPrice) const
  {
    auto pos = totalPosition(symbol);
    // A custom valuator is consulted whenever it is set, even at a zero linear
    // position: a nonlinear valuator (AMM LP, option) derives value from its
    // own state, not from a tracked quantity.
    if (_valuator != nullptr)
    {
      return _valuator->unrealizedPnl(symbol, pos.quantity, pos.avgEntryPrice, currentPrice);
    }
    if (pos.quantity.raw() == 0)
    {
      return Volume{};
    }
    // PnL = qty * (current - avg)
    Price diff = currentPrice - pos.avgEntryPrice;
    return pos.quantity * diff;
  }

  void onFill(ExchangeId exchangeId, SymbolId symbol, Quantity filledQty, Price fillPrice)
  {
    if (exchangeId >= MaxExchanges) [[unlikely]]
    {
      return;
    }

    auto& pos = _positions[exchangeId][symbol];

    Quantity qty = Quantity::fromRaw(pos.quantityRaw.load(std::memory_order_relaxed));
    Volume cost = Volume::fromRaw(pos.costBasisRaw.load(std::memory_order_relaxed));

    // Cost basis is signed and follows the quantity: a long carries a positive
    // cost, a short a negative one, so `cost / qty` reads back as the entry
    // price on either side. Opening or adding books at the fill price,
    // reducing unwinds at the average already booked, and a fill that crosses
    // through flat does both in that order.
    //
    // The earlier form keyed only off the sign of the fill and treated every
    // sell as a reduction. Opening a short from flat then divided by a zero
    // quantity, booked entry at price 0 and left unrealized PnL equal to the
    // whole notional: a 10 BTC short at 50,000 reported -500,000 against a
    // true 0, and the documented cross-venue hedge carried a fixed error of
    // about a third of its own notional at every price.
    const int64_t fillRaw = filledQty.raw();
    if (fillRaw != 0)
    {
      const bool sameDirection = (qty.raw() == 0) || ((qty.raw() > 0) == (fillRaw > 0));
      if (sameDirection)
      {
        cost = cost + (filledQty * fillPrice);
        qty = qty + filledQty;
      }
      else
      {
        const int64_t absFill = fillRaw > 0 ? fillRaw : -fillRaw;
        const int64_t absPos = qty.raw() > 0 ? qty.raw() : -qty.raw();
        const int64_t reduceRaw = absFill < absPos ? absFill : absPos;

        const Price avgEntry = cost / qty;
        const Quantity reduceSigned =
            Quantity::fromRaw(qty.raw() > 0 ? reduceRaw : -reduceRaw);
        cost = cost - (reduceSigned * avgEntry);
        qty = qty - reduceSigned;

        const int64_t flipRaw = absFill - reduceRaw;
        if (flipRaw > 0)
        {
          const Quantity flipSigned =
              Quantity::fromRaw(fillRaw > 0 ? flipRaw : -flipRaw);
          cost = cost + (flipSigned * fillPrice);
          qty = qty + flipSigned;
        }
      }
    }

    if (qty.raw() == 0)
    {
      cost = Volume{};
    }

    pos.costBasisRaw.store(cost.raw(), std::memory_order_release);
    pos.quantityRaw.store(qty.raw(), std::memory_order_release);
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
  const IPositionValuator* _valuator{nullptr};
};

}  // namespace flox
