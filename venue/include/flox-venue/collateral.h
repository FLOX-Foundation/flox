/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/ledger.h"

#include <cstdint>
#include <unordered_map>

namespace flox::venue
{

class CollateralSchedule
{
 public:
  // `priceRaw` = quote value of one unit of `asset` (Price-scaled); `haircutBps`
  // discounts that value (2000 = 20% haircut -> 80% credit).
  void configure(AssetId asset, int64_t priceRaw, int32_t haircutBps)
  {
    cfg_[asset] = {priceRaw, haircutBps};
  }

  void setPrice(AssetId asset, int64_t priceRaw)
  {
    auto it = cfg_.find(asset);
    if (it != cfg_.end())
    {
      it->second.priceRaw = priceRaw;
    }
  }

  bool accepts(AssetId asset) const { return cfg_.count(asset) != 0; }

  // Enumerate accepted collateral assets (for liquidation-time conversion).
  template <class Fn>
  void forEachAsset(Fn&& fn) const
  {
    for (const auto& [asset, c] : cfg_)
    {
      (void)c;
      fn(asset);
    }
  }

  // Haircut-adjusted quote value of `balanceRaw` units of `asset` (0 if the
  // asset is not accepted as collateral).
  Amount value(AssetId asset, Amount balanceRaw) const
  {
    auto it = cfg_.find(asset);
    if (it == cfg_.end())
    {
      return 0;
    }
    const __int128 gross =
        static_cast<__int128>(balanceRaw) * it->second.priceRaw / static_cast<__int128>(Price::Scale);
    return static_cast<Amount>(gross * (10000 - it->second.haircutBps) / 10000);
  }

  // Total haircut-adjusted collateral value of an account's whole basket.
  // What the account OWNS, reserved balance included: collateral locked
  // against an obligation is still owned. This is the basket's worth, not its
  // spendable part -- see freeValue for that.
  Amount portfolioValue(const Ledger& led, uint64_t account) const
  {
    Amount t = 0;
    for (const auto& [asset, c] : cfg_)
    {
      (void)c;
      t += value(asset, led.total(account, asset));
    }
    return t;
  }

  // What the account can still commit: reserved balance excluded. Margin
  // decisions need this one -- counting collateral already posted against
  // another obligation as spendable is how an account gets to use the same
  // money twice.
  Amount freeValue(const Ledger& led, uint64_t account) const
  {
    Amount t = 0;
    for (const auto& [asset, c] : cfg_)
    {
      (void)c;
      t += value(asset, led.available(account, asset));
    }
    return t;
  }

 private:
  struct Cfg
  {
    int64_t priceRaw;
    int32_t haircutBps;
  };
  std::unordered_map<AssetId, Cfg> cfg_;
};

}  // namespace flox::venue
