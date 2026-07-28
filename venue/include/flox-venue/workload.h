/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/messages.h"

#include <cstdint>
#include <vector>

namespace flox::venue::workload
{

struct Params
{
  SymbolId symbol{1};
  double mid{100.0};
  double tick{0.01};
  int spreadTicks{50};
  uint64_t seed{0x1234abcdULL};
  size_t count{1000};
};

// Symmetric buy/sell limit flow around `mid`. Symmetric so crossings are
// frequent and resting depth stays bounded.
inline std::vector<InboundCommand> symmetricLimits(const Params& p)
{
  const int64_t midRaw = Price::fromDouble(p.mid).raw();
  const int64_t tickRaw = Price::fromDouble(p.tick).raw();
  const uint64_t span = static_cast<uint64_t>(2 * p.spreadTicks + 1);

  std::vector<InboundCommand> v;
  v.reserve(p.count);
  uint64_t s = p.seed;
  auto next = [&]() noexcept
  {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  };

  for (size_t i = 0; i < p.count; ++i)
  {
    const uint64_t r = next();
    NewOrder o;
    o.id = i + 1;
    o.symbol = p.symbol;
    o.side = (r & 1U) ? Side::BUY : Side::SELL;
    o.type = OrderType::LIMIT;
    const int ticks = static_cast<int>((r >> 1) % span) - p.spreadTicks;
    o.price = Price::fromRaw(midRaw + static_cast<int64_t>(ticks) * tickRaw);
    o.quantity = Quantity::fromDouble(1.0 + static_cast<double>((r >> 10) % 5));
    o.tif = TimeInForce::GTC;
    o.accountId = 1 + ((r >> 20) % 64);
    v.emplace_back(o);
  }
  return v;
}

}  // namespace flox::venue::workload
