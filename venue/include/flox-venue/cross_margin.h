/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/collateral.h"
#include "flox-venue/ledger.h"
#include "flox-venue/messages.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace flox::venue
{

class CrossMarginManager
{
 public:
  struct SymCfg
  {
    int32_t imBps{0};
    int32_t mmBps{0};
    int64_t priceScale{Price::Scale};
    int64_t qtyScale{Quantity::Scale};
  };

  using OnLiquidation = std::function<void(const Liquidation&)>;

  CrossMarginManager(Ledger& led, AssetId collateral, uint64_t venueAccount, OnLiquidation onLiq = [](const Liquidation&) {}, bool autoDeleverage = false) : led_(led), collateral_(collateral), venue_(venueAccount), onLiq_(std::move(onLiq)), adl_(autoDeleverage)
  {
  }

  void configureSymbol(SymbolId s, int32_t imBps, int32_t mmBps,
                       int64_t priceScale = Price::Scale, int64_t qtyScale = Quantity::Scale)
  {
    assert(scalesValid(priceScale, qtyScale));
    cfg_[s] = {imBps, mmBps, priceScale, qtyScale};
  }

  // Scales of a configured symbol; defaults for one traded before configureSymbol.
  std::pair<int64_t, int64_t> scalesOf(SymbolId s) const
  {
    auto c = cfg_.find(s);
    return c == cfg_.end() ? std::pair<int64_t, int64_t>{Price::Scale, Quantity::Scale}
                           : std::pair<int64_t, int64_t>{c->second.priceScale, c->second.qtyScale};
  }

  // Optional multi-asset collateral: when set, equity uses the haircut-adjusted
  // basket value (BTC/ETH/... posted as margin) instead of the single quote
  // balance. Null -> quote-only wallet (default).
  void setCollateralSchedule(const CollateralSchedule* sched) noexcept { collat_ = sched; }

  // Circuit breaker: when the mark/index feed is stale or disorderly, the
  // operator pauses liquidations so a bad price can't mass-liquidate the book.
  // Marks still update (for PnL/equity); only the liquidation sweep is skipped.
  void setLiquidationsPaused(bool paused) noexcept { liqPaused_ = paused; }
  bool liquidationsPaused() const noexcept { return liqPaused_; }

  // Mark update for one symbol; liquidates any account now below maintenance
  // (unless liquidations are paused).
  void setMark(SymbolId s, Price mark)
  {
    marks_[s] = mark.raw();
    if (!liqPaused_)
    {
      checkLiquidations();
    }
  }

  // Funding for the portfolio book: each open position in `s` transfers
  // rate*notional to/from the clearing pool -- longs pay shorts when rate > 0.
  // Zero-sum across a balanced perp book (Σ long OI == Σ short OI); every move
  // is mirrored on the venue pool, so value is conserved regardless.
  void applyFunding(SymbolId s, double rate, Price mark)
  {
    const int64_t markRaw = mark.raw();
    for (auto& [acct, legs] : pos_)
    {
      auto l = legs.find(s);
      if (l == legs.end())
      {
        continue;
      }
      const auto [pS, qS] = scalesOf(s);
      const Amount notionalSigned = notionalRaw(markRaw, l->second.qtyRaw, pS, qS);
      const Amount pay = -static_cast<Amount>(static_cast<double>(notionalSigned) * rate);
      if (pay == 0)
      {
        continue;
      }
      led_.credit(acct, collateral_, pay);  // long pays (rate>0), short receives
      led_.credit(venue_, collateral_, -pay);
    }
    if (!liqPaused_)
    {
      checkLiquidations();  // funding can push a payer below maintenance
    }
  }

  int64_t positionQty(uint64_t acct, SymbolId s) const
  {
    auto a = pos_.find(acct);
    if (a == pos_.end())
    {
      return 0;
    }
    auto l = a->second.find(s);
    return l == a->second.end() ? 0 : l->second.qtyRaw;
  }

  Price positionEntry(uint64_t acct, SymbolId s) const
  {
    auto a = pos_.find(acct);
    if (a == pos_.end())
    {
      return Price{};
    }
    auto l = a->second.find(s);
    return l == a->second.end() ? Price{} : Price::fromRaw(l->second.entryRaw);
  }

  // Portfolio equity = collateral value + unrealized PnL across all symbols.
  // With a collateral schedule, the wallet is the haircut-adjusted basket;
  // otherwise it is the single quote balance.
  Amount equity(uint64_t acct) const
  {
    // Both branches mean the same thing: collateral this account can still
    // commit. portfolioValue (which counts reserved balance) would have made
    // attaching a schedule silently treat margin posted elsewhere -- isolated
    // IM locked by a matching engine on the same ledger -- as free equity.
    const Amount wallet =
        collat_ ? collat_->freeValue(led_, acct) : led_.available(acct, collateral_);
    return wallet + unrealizedPnl(acct);
  }

  Amount initialMargin(uint64_t acct) const { return marginReq(acct, /*maintenance*/ false); }
  Amount maintenanceMargin(uint64_t acct) const { return marginReq(acct, /*maintenance*/ true); }

  // Aggregate open interest across all accounts (position notional at mark) --
  // a venue-wide risk gauge for observability.
  Amount openInterestRaw() const
  {
    __int128 t = 0;
    for (const auto& [acct, legs] : pos_)
    {
      for (const auto& [s, leg] : legs)
      {
        const auto [pS, qS] = scalesOf(s);
        t += notionalRaw(markOf(s, leg.entryRaw), iabs64(leg.qtyRaw), pS, qS);
      }
    }
    return static_cast<Amount>(t);
  }

  // Total number of open position legs across all accounts.
  uint64_t openPositionCount() const
  {
    uint64_t n = 0;
    for (const auto& [acct, legs] : pos_)
    {
      n += legs.size();
    }
    return n;
  }

  // The largest amount that may be withdrawn while leaving the portfolio fully
  // initial-margined (equity - amount >= IM). Never negative.
  Amount withdrawable(uint64_t acct) const
  {
    const Amount free = equity(acct) - initialMargin(acct);
    return free > 0 ? free : 0;
  }

  // Gate a withdrawal: a trader must not pull collateral that backs open
  // positions. Allowed only if the post-withdrawal equity still covers IM.
  bool canWithdraw(uint64_t acct, Amount amount) const
  {
    return amount >= 0 && amount <= withdrawable(acct);
  }

  // Pre-trade portfolio buying-power gate. A fill that reduces net exposure is
  // always allowed (it can only lower IM); an increasing fill is admitted only
  // if the resulting portfolio IM still fits inside current equity.
  bool canOpen(uint64_t acct, SymbolId s, Side side, int64_t qtyRaw, int64_t priceRaw) const
  {
    const int64_t signedFill = (side == Side::BUY ? 1 : -1) * qtyRaw;
    const int64_t cur = positionQty(acct, s);
    const int64_t next = cur + signedFill;
    // Not increasing exposure on this symbol -> always allowed.
    if (iabs64(next) <= iabs64(cur))
    {
      return true;
    }

    const Amount imAfter = marginReqWith(acct, s, next, priceRaw, /*maintenance*/ false);
    return imAfter <= equity(acct);
  }

  // Apply a fill (post-trade). Realizes PnL on the reducing portion into the
  // ledger through the clearing pool; updates the average entry on the opening
  // portion. Mirrors the engine's netting, without per-position margin locks.
  void applyFill(uint64_t acct, SymbolId s, Side side, int64_t qtyRaw, int64_t priceRaw)
  {
    Leg& p = pos_[acct][s];
    const int64_t fillSign = (side == Side::BUY) ? 1 : -1;
    int64_t remaining = qtyRaw;

    const int64_t posSign = (p.qtyRaw > 0) ? 1 : (p.qtyRaw < 0 ? -1 : 0);
    if (posSign != 0 && posSign != fillSign)
    {
      const int64_t reduceQty = std::min<int64_t>(remaining, iabs64(p.qtyRaw));
      const auto [pS, qS] = scalesOf(s);
      const Amount pnl = notionalRaw(priceRaw - p.entryRaw, reduceQty, pS, qS) * posSign;
      led_.credit(acct, collateral_, pnl);
      led_.credit(venue_, collateral_, -pnl);
      p.qtyRaw += fillSign * reduceQty;
      remaining -= reduceQty;
      if (p.qtyRaw == 0)
      {
        p.entryRaw = 0;
      }
    }

    if (remaining > 0)
    {
      const int64_t absOld = iabs64(p.qtyRaw);
      const __int128 num = static_cast<__int128>(absOld) * p.entryRaw +
                           static_cast<__int128>(remaining) * priceRaw;
      p.entryRaw = static_cast<int64_t>(num / (absOld + remaining));
      p.qtyRaw += fillSign * remaining;
    }

    if (p.qtyRaw == 0)
    {
      erasePos(acct, s);
    }
  }

 private:
  struct Leg
  {
    int64_t qtyRaw{0};
    int64_t entryRaw{0};
  };

  static int64_t iabs64(int64_t v) { return v < 0 ? -v : v; }

  int64_t markOf(SymbolId s, int64_t fallbackRaw) const
  {
    auto it = marks_.find(s);
    return it == marks_.end() ? fallbackRaw : it->second;
  }

  Amount unrealizedPnl(uint64_t acct) const
  {
    auto a = pos_.find(acct);
    if (a == pos_.end())
    {
      return 0;
    }
    __int128 total = 0;
    for (const auto& [s, leg] : a->second)
    {
      const int64_t mark = markOf(s, leg.entryRaw);
      const auto [pS, qS] = scalesOf(s);
      total += notionalRaw(mark - leg.entryRaw, leg.qtyRaw, pS, qS);
    }
    return static_cast<Amount>(total);
  }

  Amount marginReq(uint64_t acct, bool maintenance) const
  {
    auto a = pos_.find(acct);
    if (a == pos_.end())
    {
      return 0;
    }
    __int128 total = 0;
    for (const auto& [s, leg] : a->second)
    {
      total += legMargin(s, leg.qtyRaw, markOf(s, leg.entryRaw), maintenance);
    }
    return static_cast<Amount>(total);
  }

  // Portfolio margin requirement if symbol `s` position were `newQty` at mark
  // `refPriceRaw` (used for the pre-trade check on the incoming symbol).
  Amount marginReqWith(uint64_t acct, SymbolId s, int64_t newQty, int64_t refPriceRaw,
                       bool maintenance) const
  {
    __int128 total = legMargin(s, newQty, markOf(s, refPriceRaw), maintenance);
    auto a = pos_.find(acct);
    if (a != pos_.end())
    {
      for (const auto& [os, leg] : a->second)
      {
        if (os == s)
        {
          continue;
        }
        total += legMargin(os, leg.qtyRaw, markOf(os, leg.entryRaw), maintenance);
      }
    }
    return static_cast<Amount>(total);
  }

  __int128 legMargin(SymbolId s, int64_t qtyRaw, int64_t markRaw, bool maintenance) const
  {
    auto c = cfg_.find(s);
    if (c == cfg_.end())
    {
      return 0;
    }
    const int32_t bps = maintenance ? c->second.mmBps : c->second.imBps;
    const Amount notional =
        notionalRaw(markRaw, iabs64(qtyRaw), c->second.priceScale, c->second.qtyScale);
    return notional * bps / 10000;
  }

  void checkLiquidations()
  {
    std::vector<uint64_t> toLiq;
    for (const auto& [acct, legs] : pos_)
    {
      if (legs.empty())
      {
        continue;
      }
      if (equity(acct) < maintenanceMargin(acct))
      {
        toLiq.push_back(acct);
      }
    }
    // Deterministic order: when several accounts breach at once and share ADL
    // counterparties, the liquidation order decides which winner absorbs which
    // deficit vs. which hits insurance -- must not depend on pos_ iteration.
    std::sort(toLiq.begin(), toLiq.end());
    for (uint64_t a : toLiq)
    {
      liquidate(a);
    }
  }

  // Close every position at its mark, realize all PnL into the wallet, and let
  // the insurance fund cover any residual negative equity (bankruptcy). When
  // auto-deleverage is on, a bankruptcy deficit is clawed back from the most
  // profitable opposite-side traders (ADL) before it touches the insurance fund.
  void liquidate(uint64_t acct)
  {
    auto a = pos_.find(acct);
    if (a == pos_.end())
    {
      return;
    }
    std::vector<std::pair<SymbolId, int64_t>> bankruptSides;  // (symbol, signed qty) closed
    for (const auto& [s, leg] : a->second)
    {
      const int64_t mark = markOf(s, leg.entryRaw);
      const auto [pS, qS] = scalesOf(s);
      const Amount pnl = notionalRaw(mark - leg.entryRaw, leg.qtyRaw, pS, qS);
      led_.credit(acct, collateral_, pnl);
      led_.credit(venue_, collateral_, -pnl);
      bankruptSides.emplace_back(s, leg.qtyRaw);
    }
    pos_.erase(a);

    // Before touching insurance, sell any non-quote collateral (BTC/ETH) the
    // account posted to cover the quote deficit -- the venue takes the coin at
    // its haircut value and pays quote. Value stays conserved per asset.
    Amount avail = led_.available(acct, collateral_);
    if (avail < 0 && collat_)
    {
      convertCollateral(acct, -avail);
      avail = led_.available(acct, collateral_);
    }
    const bool bankrupt = avail < 0;
    for (const auto& [s, q] : bankruptSides)
    {
      onLiq_(Liquidation{acct, s, Quantity::fromRaw(iabs64(q)), Price::fromRaw(markOf(s, 0)), bankrupt,
                         /*adl*/ false});
    }
    if (!bankrupt)
    {
      return;
    }

    Amount deficit = -avail;
    led_.credit(acct, collateral_, deficit);  // insurance tops the wallet up to zero
    led_.credit(venue_, collateral_, -deficit);
    if (adl_)
    {
      autoDeleverage(bankruptSides, deficit);  // recover the deficit from winners
    }
  }

  // Claw the bankruptcy deficit back from the most profitable traders on the
  // opposite side of the bankrupt's positions: close each at the mark and haircut
  // their realized gain into the insurance fund, until the deficit is recovered.
  void autoDeleverage(const std::vector<std::pair<SymbolId, int64_t>>& bankruptSides, Amount deficit)
  {
    // Rank candidate winners: opposite side, positive unrealized profit.
    struct Cand
    {
      uint64_t acct;
      SymbolId sym;
      Amount uPnl;
    };
    std::vector<Cand> cands;
    for (const auto& [sym, bqty] : bankruptSides)
    {
      const int64_t bankruptSign = bqty > 0 ? 1 : -1;
      const int64_t mark = markOf(sym, 0);
      for (const auto& [oa, legs] : pos_)
      {
        auto l = legs.find(sym);
        if (l == legs.end())
        {
          continue;
        }
        const int64_t sign = l->second.qtyRaw > 0 ? 1 : (l->second.qtyRaw < 0 ? -1 : 0);
        if (sign == 0 || sign == bankruptSign)
        {
          continue;  // must be the opposite side
        }
        const auto [pS, qS] = scalesOf(sym);
        const Amount up = notionalRaw(mark - l->second.entryRaw, l->second.qtyRaw, pS, qS);
        if (up > 0)
        {
          cands.push_back({oa, sym, up});
        }
      }
    }
    // Total order: most profitable first, then (acct, sym) as a deterministic
    // tie-break. Without it, equal-uPnl winners resolve on unordered_map layout
    // and std::sort's instability -- so which winner is deleveraged (an emitted
    // Liquidation folded into the determinism hash) would differ across a replica
    // rebuilt with a different insertion history, forking HA state.
    std::sort(cands.begin(), cands.end(),
              [](const Cand& x, const Cand& y)
              {
                if (x.uPnl != y.uPnl)
                {
                  return x.uPnl > y.uPnl;
                }
                if (x.acct != y.acct)
                {
                  return x.acct < y.acct;
                }
                return x.sym < y.sym;
              });

    Amount remaining = deficit;
    for (const auto& c : cands)
    {
      if (remaining <= 0)
      {
        break;
      }
      auto pa = pos_.find(c.acct);
      if (pa == pos_.end())
      {
        continue;
      }
      auto l = pa->second.find(c.sym);
      if (l == pa->second.end())
      {
        continue;
      }
      const int64_t mark = markOf(c.sym, 0);
      const Leg leg = l->second;
      // Close the winner at the mark (realize their gain), then haircut it.
      const auto [pS, qS] = scalesOf(c.sym);
      const Amount pnl = notionalRaw(mark - leg.entryRaw, leg.qtyRaw, pS, qS);
      led_.credit(c.acct, collateral_, pnl);
      led_.credit(venue_, collateral_, -pnl);
      const Amount haircut = remaining < c.uPnl ? remaining : c.uPnl;
      // Confiscate the forgone profit to insurance -- but debit is all-or-nothing
      // and the winner's `available` may be below the haircut (funding can drive
      // it negative). Crediting the venue the full haircut while the debit
      // no-ops would MINT money. Take only what is actually there and credit the
      // venue exactly that; any uncovered remainder stays in `remaining` and is
      // borne by the insurance fund (the normal ADL waterfall), never conjured.
      const Amount avail = led_.available(c.acct, collateral_);
      const Amount taken = std::min<Amount>(haircut, avail > 0 ? avail : 0);
      if (taken > 0)
      {
        led_.debit(c.acct, collateral_, taken);
        led_.credit(venue_, collateral_, taken);
      }
      remaining -= taken;
      erasePos(c.acct, c.sym);
      onLiq_(Liquidation{c.acct, c.sym, Quantity::fromRaw(iabs64(leg.qtyRaw)), Price::fromRaw(mark),
                         /*bankrupt*/ false, /*adl*/ true});
    }
  }

  // Liquidate an account's non-quote collateral to cover a quote `deficit`:
  // move coin from the account to the venue (which will offload it externally)
  // and credit the account the haircut value in quote. Balanced per asset.
  void convertCollateral(uint64_t acct, Amount deficit)
  {
    Amount remaining = deficit;
    collat_->forEachAsset(
        [&](AssetId asset)
        {
          if (asset == collateral_ || remaining <= 0)
          {
            return;
          }
          const Amount bal = led_.available(acct, asset);
          if (bal <= 0)
          {
            return;
          }
          const Amount val = collat_->value(asset, bal);
          if (val <= 0)
          {
            return;
          }
          const Amount take = remaining < val ? remaining : val;
          const Amount units = static_cast<Amount>(static_cast<__int128>(bal) * take / val);
          led_.debit(acct, asset, units);  // sell coin to the venue
          led_.credit(venue_, asset, units);
          led_.credit(acct, collateral_, take);  // proceeds in quote
          led_.credit(venue_, collateral_, -take);
          remaining -= take;
        });
  }

  void erasePos(uint64_t acct, SymbolId s)
  {
    auto a = pos_.find(acct);
    if (a == pos_.end())
    {
      return;
    }
    a->second.erase(s);
    if (a->second.empty())
    {
      pos_.erase(a);
    }
  }

  Ledger& led_;
  AssetId collateral_;
  uint64_t venue_;
  OnLiquidation onLiq_;
  const CollateralSchedule* collat_{nullptr};
  bool adl_{false};
  bool liqPaused_{false};
  std::unordered_map<SymbolId, SymCfg> cfg_;
  std::unordered_map<SymbolId, int64_t> marks_;
  std::unordered_map<uint64_t, std::unordered_map<SymbolId, Leg>> pos_;
};

}  // namespace flox::venue
