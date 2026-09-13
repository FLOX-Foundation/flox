/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/book/nlevel_order_book.h"
#include "flox/common.h"

#include <cmath>
#include <optional>

namespace flox
{

struct SymbolContext
{
  static constexpr size_t kDefaultBookLevels = 512;

  NLevelOrderBook<kDefaultBookLevels> book;
  Quantity position{};
  // Empty when the attached position manager reports no cost basis: the
  // position is flat, or the manager keeps none. This used to be a plain
  // Price that nothing in the engine ever wrote, so it read back as 0 and
  // unrealized PnL came out as the entire notional.
  std::optional<Price> avgEntryPrice{};
  Price lastTradePrice{};
  int64_t lastUpdateNs{0};
  SymbolId symbolId{0};

  SymbolContext() : book(Price::fromDouble(0.01)) {}
  SymbolContext(Price tickSize) : book(tickSize) {}

  [[nodiscard]] std::optional<Price> mid() const noexcept
  {
    auto bid = book.bestBid();
    auto ask = book.bestAsk();
    if (!bid || !ask)
    {
      return std::nullopt;
    }
    return Price::fromRaw((bid->raw() + ask->raw()) / 2);
  }

  [[nodiscard]] std::optional<Price> bookSpread() const noexcept
  {
    auto bid = book.bestBid();
    auto ask = book.bestAsk();
    if (!bid || !ask)
    {
      return std::nullopt;
    }
    return Price::fromRaw(ask->raw() - bid->raw());
  }

  // Empty when there is no entry price to measure against. A quietly wrong
  // number is worse than an honest "unknown": the old code substituted zero
  // for the entry and returned position times mark.
  [[nodiscard]] std::optional<double> unrealizedPnl(Price markPrice) const noexcept
  {
    if (position.isZero())
    {
      return 0.0;
    }
    if (!avgEntryPrice)
    {
      return std::nullopt;
    }
    double posQty = position.toDouble();
    double entryPx = avgEntryPrice->toDouble();
    double markPx = markPrice.toDouble();
    return posQty * (markPx - entryPx);
  }

  [[nodiscard]] std::optional<double> unrealizedPnl() const noexcept
  {
    auto midOpt = mid();
    if (!midOpt)
    {
      return std::nullopt;
    }
    return unrealizedPnl(*midOpt);
  }

  bool isLong() const noexcept { return position.raw() > 0; }

  bool isShort() const noexcept { return position.raw() < 0; }

  bool isFlat() const noexcept { return position.isZero(); }

  void reset() noexcept
  {
    book.clear();
    position = Quantity{};
    avgEntryPrice.reset();
    lastTradePrice = Price{};
    lastUpdateNs = 0;
  }
};

[[nodiscard]] inline std::optional<Price> spread(const SymbolContext& a,
                                                 const SymbolContext& b) noexcept
{
  auto midA = a.mid();
  auto midB = b.mid();
  if (!midA || !midB)
  {
    return std::nullopt;
  }
  return Price::fromRaw(midA->raw() - midB->raw());
}

[[nodiscard]] inline std::optional<double> ratio(const SymbolContext& a,
                                                 const SymbolContext& b) noexcept
{
  auto midA = a.mid();
  auto midB = b.mid();
  if (!midA || !midB || midB->isZero())
  {
    return std::nullopt;
  }
  return midA->toDouble() / midB->toDouble();
}

}  // namespace flox
