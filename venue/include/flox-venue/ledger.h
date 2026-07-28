/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Account ledger / settlement (real money). A double-entry, multi-asset balance
 * store: every account holds, per asset, an `available` and a `reserved`
 * balance (raw fixed-point, 1e-8 units, 128-bit to avoid notional overflow).
 *
 * Order entry reserves buying power (quote for a bid, base for an ask); a fill
 * settles it -- base and quote change hands, fees are debited; a cancel releases
 * the remainder. Value is conserved: sum of all balances only changes by
 * deposits/withdrawals and fee capture. This is what makes it a venue that
 * moves real money rather than a matching toy.
 */
#pragma once

#include "flox/common.h"

#include <cstdint>
#include <unordered_map>

namespace flox::venue
{

using AssetId = uint16_t;
using Amount = __int128;  // raw 1e-8 units

inline Amount amountOf(Volume v) { return static_cast<Amount>(v.raw()); }
inline Amount amountOf(Quantity q) { return static_cast<Amount>(q.raw()); }

class Ledger
{
 public:
  void deposit(uint64_t account, AssetId asset, Amount raw) { bal(account, asset).avail += raw; }

  Amount available(uint64_t account, AssetId asset) const
  {
    const Bal* b = find(account, asset);
    return b ? b->avail : 0;
  }
  Amount reserved(uint64_t account, AssetId asset) const
  {
    const Bal* b = find(account, asset);
    return b ? b->rsvd : 0;
  }
  Amount total(uint64_t account, AssetId asset) const
  {
    const Bal* b = find(account, asset);
    return b ? b->avail + b->rsvd : 0;
  }

  // available -> reserved; fails (no change) if insufficient available.
  bool reserve(uint64_t account, AssetId asset, Amount raw)
  {
    Bal& b = bal(account, asset);
    if (b.avail < raw)
    {
      return false;
    }
    b.avail -= raw;
    b.rsvd += raw;
    return true;
  }

  // reserved -> available (e.g. on cancel of the unfilled remainder).
  void release(uint64_t account, AssetId asset, Amount raw)
  {
    Bal& b = bal(account, asset);
    const Amount m = raw < b.rsvd ? raw : b.rsvd;
    b.rsvd -= m;
    b.avail += m;
  }

  // Remove `raw` from reserved (it was paid out on a fill).
  void spendReserved(uint64_t account, AssetId asset, Amount raw)
  {
    Bal& b = bal(account, asset);
    b.rsvd -= raw;
  }

  void credit(uint64_t account, AssetId asset, Amount raw) { bal(account, asset).avail += raw; }

  // Enumerate every (account, asset, total) balance -- for reconciliation and
  // segregation reporting (compliance). Not on the hot path.
  template <class Fn>
  void forEachBalance(Fn&& fn) const
  {
    for (const auto& [k, b] : bal_)
    {
      fn(k >> 16, static_cast<AssetId>(k & 0xFFFF), b.avail + b.rsvd);
    }
  }

  // Debit available directly (market taker leg / fee with no prior reservation).
  bool debit(uint64_t account, AssetId asset, Amount raw)
  {
    Bal& b = bal(account, asset);
    if (b.avail < raw)
    {
      return false;
    }
    b.avail -= raw;
    return true;
  }

 private:
  struct Bal
  {
    Amount avail{0};
    Amount rsvd{0};
  };
  static uint64_t key(uint64_t account, AssetId asset)
  {
    return (account << 16) | asset;  // account uses low 48 bits in practice
  }
  Bal& bal(uint64_t account, AssetId asset) { return bal_[key(account, asset)]; }
  const Bal* find(uint64_t account, AssetId asset) const
  {
    auto it = bal_.find(key(account, asset));
    return it == bal_.end() ? nullptr : &it->second;
  }

  std::unordered_map<uint64_t, Bal> bal_;
};

}  // namespace flox::venue
