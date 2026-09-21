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

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

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

  // Enumerate accepted collateral assets (for liquidation-time conversion), in
  // AssetId order.
  //
  // Sorted, not merely enumerated. The assets live in an unordered_map, so
  // their traversal order is a bucket-layout artifact. The caller that matters
  // is CrossMarginEngine::convertCollateral, which walks this enumeration
  // GREEDILY: it sells each asset only up to the quote deficit still
  // outstanding and stops once the deficit is covered. An unsorted enumeration
  // therefore decides WHICH coin a liquidating account's basket is sold from
  // and how much of each -- the per-asset balances left on the ledger
  // afterwards, i.e. the resulting STATE, not merely the order of a report.
  // Two venues built against different standard libraries would settle the
  // same bankruptcy into different wallets.
  template <class Fn>
  void forEachAsset(Fn&& fn) const
  {
    std::vector<AssetId> assets;
    assets.reserve(cfg_.size());
    // order: collected and sorted here, before anything is handed to fn
    for (const auto& [asset, c] : cfg_)
    {
      (void)c;
      assets.push_back(asset);
    }
    std::sort(assets.begin(), assets.end());
    for (AssetId a : assets)
    {
      fn(a);
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
    // The haircut discounts what an account owns, never what it owes. A
    // negative balance is a debt -- funding can drive a wallet there, which the
    // segregation notes call out as reachable -- and discounting a debt writes
    // down the liability: a coin owed at a 20% haircut counted as four fifths
    // of itself, and the missing fifth read as equity the account could
    // withdraw or trade against.
    if (gross <= 0)
    {
      return static_cast<Amount>(gross);
    }
    return static_cast<Amount>(gross * (10000 - it->second.haircutBps) / 10000);
  }

  // Total haircut-adjusted collateral value of an account's whole basket.
  // What the account OWNS, reserved balance included: collateral locked
  // against an obligation is still owned. This is the basket's worth, not its
  // spendable part -- see freeValue for that.
  Amount portfolioValue(const Ledger& led, uint64_t account) const
  {
    Amount t = 0;
    // order: not observable -- Amount is an integer and the loop is a sum;
    // addition is associative, so the basket's value is layout-independent
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
    // order: not observable -- integer sum, same as portfolioValue above
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
