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

#include <functional>
#include <vector>

namespace flox
{

struct ExchangePosition
{
  SymbolId symbol{};
  Quantity quantity{};  // signed: positive=long, negative=short
  Price avgEntryPrice{};
};

struct PositionMismatch
{
  SymbolId symbol{};
  Quantity localQuantity{};
  Quantity exchangeQuantity{};
  Price localAvgEntry{};
  Price exchangeAvgEntry{};

  Quantity quantityDiff() const
  {
    return Quantity::fromRaw(exchangeQuantity.raw() - localQuantity.raw());
  }
};

enum class ReconcileAction : uint8_t
{
  ACCEPT_EXCHANGE = 0,  // Trust exchange position, adjust local
  ACCEPT_LOCAL = 1,     // Trust local position, ignore exchange
  FLAG_ONLY = 2,        // Log mismatch, do not adjust
};

class PositionReconciler
{
 public:
  using MismatchCallback = std::function<ReconcileAction(const PositionMismatch&)>;

  void setMismatchHandler(MismatchCallback cb)
  {
    _onMismatch = std::move(cb);
  }

  // localSymbols: if provided, also detects positions that exist locally but not at exchange.
  template <typename LocalPosFn>
  std::vector<PositionMismatch> reconcile(
      const std::vector<ExchangePosition>& exchangePositions,
      LocalPosFn&& localPositionFn,
      const std::vector<SymbolId>& localSymbols = {}) const
  {
    std::vector<PositionMismatch> mismatches;

    for (const auto& bp : exchangePositions)
    {
      auto [localQty, localAvg] = localPositionFn(bp.symbol);

      if (localQty.raw() != bp.quantity.raw())
      {
        PositionMismatch m{};
        m.symbol = bp.symbol;
        m.localQuantity = localQty;
        m.exchangeQuantity = bp.quantity;
        m.localAvgEntry = localAvg;
        m.exchangeAvgEntry = bp.avgEntryPrice;
        mismatches.push_back(m);
      }
    }

    if (!localSymbols.empty())
    {
      for (SymbolId sym : localSymbols)
      {
        bool foundAtExchange = false;
        for (const auto& bp : exchangePositions)
        {
          if (bp.symbol == sym)
          {
            foundAtExchange = true;
            break;
          }
        }
        if (foundAtExchange)
        {
          continue;
        }

        auto [localQty, localAvg] = localPositionFn(sym);
        if (localQty.raw() != 0)
        {
          PositionMismatch m{};
          m.symbol = sym;
          m.localQuantity = localQty;
          m.exchangeQuantity = Quantity{};
          m.localAvgEntry = localAvg;
          m.exchangeAvgEntry = Price{};
          mismatches.push_back(m);
        }
      }
    }

    return mismatches;
  }

  template <typename LocalPosFn, typename AdjustFn>
  std::vector<PositionMismatch> reconcileAndApply(
      const std::vector<ExchangePosition>& exchangePositions,
      LocalPosFn&& localPositionFn,
      AdjustFn&& adjustFn,
      const std::vector<SymbolId>& localSymbols = {}) const
  {
    auto mismatches = reconcile(exchangePositions, std::forward<LocalPosFn>(localPositionFn),
                                localSymbols);

    if (_onMismatch)
    {
      for (const auto& m : mismatches)
      {
        ReconcileAction action = _onMismatch(m);
        if (action == ReconcileAction::ACCEPT_EXCHANGE)
        {
          adjustFn(m.symbol, m.exchangeQuantity, m.exchangeAvgEntry);
        }
      }
    }

    return mismatches;
  }

 private:
  MismatchCallback _onMismatch;
};

}  // namespace flox
