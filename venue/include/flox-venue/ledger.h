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

#include <cassert>
#include <cstdint>
#include <unordered_map>

namespace flox::venue
{

using AssetId = uint16_t;
using Amount = __int128;  // raw 1e-8 units

inline Amount amountOf(Volume v) { return static_cast<Amount>(v.raw()); }
inline Amount amountOf(Quantity q) { return static_cast<Amount>(q.raw()); }

// Money is always at this scale (1e-8 units of the asset), regardless of any
// symbol's price/qty scale. A base-asset balance is the symbol's qty raw, so
// every symbol sharing a base asset must use one qtyScale.
inline constexpr int64_t kMoneyScale = 100'000'000;

inline constexpr Amount kAmountMax = static_cast<Amount>(~static_cast<unsigned __int128>(0) >> 1);
inline constexpr Amount kAmountMin = -kAmountMax - 1;

// kMoneyScale and qtyScale must divide one another exactly (any power-of-ten
// scale qualifies); priceScale only needs to be positive.
inline constexpr bool scalesValid(int64_t priceScale, int64_t qtyScale)
{
  return priceScale > 0 && qtyScale > 0 &&
         (qtyScale <= kMoneyScale ? kMoneyScale % qtyScale == 0 : qtyScale % kMoneyScale == 0);
}

// price x quantity, each raw in the symbol's own scale, -> Amount raw at
// kMoneyScale. Generalizes the fixed-scale `praw * qraw / Price::Scale`
// (bit-identical to it at the default 1e8/1e8, so determinism hashes are
// unchanged). Signed; truncates toward zero exactly like the expression it
// replaces. Multiply-before-divide keeps sub-quote-unit precision when
// qtyScale is coarser than money (a meme-coin at qtyScale 1); the overflow
// guard falls back to divide-first for magnitudes past ~1e22 quote units,
// where the lost remainder is meaningless but signed overflow would be UB.
inline Amount notionalRaw(int64_t priceRaw, int64_t qtyRaw, int64_t priceScale, int64_t qtyScale)
{
  const Amount prod = static_cast<Amount>(priceRaw) * qtyRaw;
  if (qtyScale <= kMoneyScale)
  {
    const Amount mul = kMoneyScale / qtyScale;
    if (prod > kAmountMax / mul || prod < kAmountMin / mul)
    {
      return prod / priceScale * mul;
    }
    return prod * mul / priceScale;
  }
  return prod / (static_cast<Amount>(priceScale) * (qtyScale / kMoneyScale));
}

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

  // RECOVERY-ONLY: overwrite an (account, asset) balance with an exact signed
  // available/reserved split (snapshot RestoreBalance application). This is a
  // direct write, not a flow -- it deliberately bypasses the reserve/release
  // discipline so any live moment restores bit-for-bit, including a negative
  // wallet mid-liquidation. Never call it on the live trading path.
  void restore(uint64_t account, AssetId asset, Amount avail, Amount rsvd)
  {
    Bal& b = bal(account, asset);
    b.avail = avail;
    b.rsvd = rsvd;
  }

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

  // Enumerate every (account, asset, available, reserved) balance -- for the
  // venue checkpoint (balances serialize as totals; reserved reconstitutes by
  // re-reservation) and its state hash. Not on the hot path.
  template <class Fn>
  void forEachBalanceSplit(Fn&& fn) const
  {
    for (const auto& [k, b] : bal_)
    {
      fn(k >> 16, static_cast<AssetId>(k & 0xFFFF), b.avail, b.rsvd);
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
  // Account ids are packed into the top 48 bits. An id that does not fit would
  // wrap and silently share a balance row with another account -- two customers
  // spending one balance -- so it is a programming error, not a runtime case to
  // recover from.
  static constexpr uint64_t kMaxAccountId = (1ULL << 48) - 1;

  static uint64_t key(uint64_t account, AssetId asset)
  {
    assert(account <= kMaxAccountId && "account id must fit 48 bits (ledger key packing)");
    return (account << 16) | asset;
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
