/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/engine/state_hash_tags.h"
#include "flox-venue/event_hash.h"
#include "flox-venue/event_sink.h"
#include "flox-venue/journal.h"
#include "flox-venue/ledger.h"
#include "flox-venue/messages.h"
#include "flox-venue/symbol_config.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace flox::venue::engine
{

// Linear-perp clearing: positions, funding, maintenance-margin liquidation,
// auto-deleveraging. None of it touches the book -- the state is the
// per-account Position, the funding calendar and a pointer to the ledger the
// money moves in -- so it is not parameterised on the book type and is not a
// template. The engine owns one and calls it where these methods used to be
// called; behaviour, event order and snapshot bytes are the ones it had when
// this was inline in matching_engine.h. What clearing cannot reach -- the
// reservation a fill's initial margin comes out of, the resting orders of an
// account being liquidated -- arrives as `Hooks`: a context plus function
// pointers, not virtuals, since every fill's opening leg goes through it.
class Clearing
{
 public:
  struct Position
  {
    int64_t qtyRaw{0};    // signed contracts (Quantity raw)
    int64_t entryRaw{0};  // average entry price (Price raw)
    Amount margin{0};     // posted position margin (quote raw), reserved in the ledger
  };

  // The engine-side work clearing delegates back. `ctx` is the engine.
  struct Hooks
  {
    void* ctx{nullptr};
    // Reserved IM into position margin (stays reserved); returns the amount.
    Amount (*consumeOrderIM)(void*, OrderId, int64_t qtyRaw){nullptr};
    // A reducing order's own reserved IM was not needed -> back to available.
    void (*releaseOrderIM)(void*, OrderId, int64_t qtyRaw, uint64_t acct){nullptr};
    // A liquidated account's resting orders go before its equity is judged.
    void (*cancelAllForAccount)(void*, uint64_t acct){nullptr};
  };

  Clearing(const SymbolConfig& cfg, const EventSink& sink, Hooks hooks) : cfg_(cfg), sink_(sink), hooks_(hooks) {}

  void setLedger(Ledger* ledger, uint64_t venueAccount) noexcept
  {
    ledger_ = ledger;
    venueAccount_ = venueAccount;
  }

  Ledger* ledger() const noexcept { return ledger_; }
  uint64_t venueAccount() const noexcept { return venueAccount_; }

  // ---- positions: signed contracts, average entry ------------------------
  int64_t positionQty(uint64_t account) const
  {
    auto it = positions_.find(account);
    return it == positions_.end() ? 0 : it->second.qtyRaw;
  }

  Price positionEntry(uint64_t account) const
  {
    auto it = positions_.find(account);
    return it == positions_.end() ? Price{} : Price::fromRaw(it->second.entryRaw);
  }

  // The long side of the tracked positions IS the open interest: every
  // contract has both legs, so a signed sum would give zero.
  Quantity openInterest() const
  {
    int64_t oi = 0;
    // order: not observable -- int64 sum of the long legs
    for (const auto& [acct, p] : positions_)
    {
      (void)acct;
      if (p.qtyRaw > 0)
      {
        oi += p.qtyRaw;
      }
    }
    return Quantity::fromRaw(oi);
  }

  // Unrealized PnL of an account's perp position marked at `mark` (quote raw).
  Amount unrealizedPnlRaw(uint64_t account, Price mark) const
  {
    auto it = positions_.find(account);
    if (it == positions_.end())
    {
      return 0;
    }
    const int64_t sign = it->second.qtyRaw > 0 ? 1 : -1;
    return notionalRaw(mark.raw() - it->second.entryRaw, iabs64(it->second.qtyRaw), cfg_.priceScale,
                       cfg_.qtyScale) *
           sign;
  }

  // Posted position margin (quote raw). With every resting order drained it is
  // the ledger's total reserved: each unit backs an order or a position.
  Amount totalPositionMargin() const
  {
    Amount t = 0;
    // order: not observable -- Amount sum of posted margin
    for (const auto& [acct, p] : positions_)
    {
      (void)acct;
      t += p.margin;
    }
    return t;
  }

  // ---- funding calendar --------------------------------------------------
  // Last rate applied, at kFundingRateScale. Carried by the checkpoint
  // (RestoreFunding), so a restored engine publishes the real rate, not 0.
  int64_t fundingRateRaw() const noexcept { return fundingRateRaw_; }

  // The operator-set schedule's interval when there is one, else the config's.
  int64_t fundingIntervalNs() const noexcept
  {
    return (fundingIntervalNs_.count() > 0 ? fundingIntervalNs_ : cfg_.fundingIntervalNs).count();
  }

  // Next funding boundary, in sequencer time. An operator-set schedule is a
  // FACT -- engine state, hashed, checkpointed, advanced by each ApplyFunding
  // -- so the feed publishes what the venue will settle on. With none it falls
  // back to the derivation from (now, config interval), kept so an engine that
  // never learned one behaves as before. 0 = neither: this instrument is unfunded.
  SeqNanos nextFundingNs(SeqNanos now) const noexcept
  {
    if (static_cast<bool>(nextFundingNs_))
    {
      return nextFundingNs_;
    }
    if (cfg_.fundingIntervalNs.count() <= 0)
    {
      return SeqNanos{};
    }
    // Modular math on one domain's raw ticks, restated as the same domain.
    return SeqNanos::fromRaw((now.raw() / cfg_.fundingIntervalNs.count() + 1) *
                             cfg_.fundingIntervalNs.count());
  }

  // A non-positive interval or boundary clears the schedule, dropping back to
  // the config derivation. Publishing is the engine's: only it knows whether
  // the instrument has a mark yet.
  void setFundingSchedule(DurationNs intervalNs, SeqNanos nextFundingNs) noexcept
  {
    fundingIntervalNs_ = intervalNs.count() > 0 ? intervalNs : DurationNs{};
    nextFundingNs_ = nextFundingNs.raw() > 0 ? nextFundingNs : SeqNanos{};
  }

  // A settlement just happened, so the calendar moves on: one whole interval
  // past the boundary settled, and further whole intervals if it ran late
  // enough that one step still leaves it in the past (catching up after an
  // outage must not publish a stale "next funding"). Only a set schedule moves.
  void advanceFundingSchedule(SeqNanos now) noexcept
  {
    if (fundingIntervalNs_.count() <= 0 || nextFundingNs_.raw() <= 0)
    {
      return;
    }
    nextFundingNs_ += fundingIntervalNs_;
    if (nextFundingNs_ <= now)
    {
      const DurationNs behind = now - nextFundingNs_;
      nextFundingNs_ += DurationNs{(behind.count() / fundingIntervalNs_.count() + 1) *
                                   fundingIntervalNs_.count()};
    }
  }

  // Each position transfers |notional|*rate to/from the clearing pool -- longs
  // pay when rate > 0, `mark` values the leg. The caller publishes the feed
  // update afterwards on both paths, as it did when inline.
  void applyFunding(double rate, Price mark, SeqNanos now)
  {
    // The journaled body carries a double; everything past this line is the
    // integer raw, so the rate that is published is the rate that is settled.
    const int64_t rateRaw = fundingRateRawOf(rate);
    // The rate is known with or without a ledger and it is what the feed
    // publishes -- record it before the early return, or a venue with no
    // bound ledger would never publish one.
    fundingRateRaw_ = rateRaw;
    advanceFundingSchedule(now);
    if (ledger_ == nullptr)
    {
      return;
    }
    // order: not observable -- each account's funding payment is an
    // independent integer credit; the pool accumulates by addition
    for (auto& [acct, p] : positions_)
    {
      const Amount notional =
          notionalRaw(mark.raw(), iabs64(p.qtyRaw), cfg_.priceScale, cfg_.qtyScale);
      const Amount mag = rateOnNotional(notional, rateRaw, kFundingRateScale);
      const Amount signedPay = (p.qtyRaw > 0) ? -mag : mag;  // long pays when rate>0
      ledger_->credit(acct, cfg_.quoteAsset, signedPay);
      ledger_->credit(venueAccount_, cfg_.quoteAsset, -signedPay);
    }
    // Funding is charged to `available`; a max-leverage payer with no free
    // collateral drives it negative, and the wallet-drag term below counts that
    // against maintenance equity -- unaffordable funding liquidates instead of
    // accruing silent bad debt.
    checkLiquidations(mark);
  }

  // An operator correction. Not a trade: see the note on AdjustPosition for
  // why no PnL is realized, no fee charged and no margin moved. It makes the
  // position match what is reconciled against, loudly enough to tell from a fill.
  void adjustPosition(const AdjustPosition& a)
  {
    if (a.qtyDeltaRaw == 0 && a.entryRaw == 0)
    {
      sink_(OrderRejected{0, cfg_.id, RejectReason::AdjustmentEmpty, a.accountId, 0});
      return;
    }
    auto it = positions_.find(a.accountId);
    if (it == positions_.end() && a.entryRaw == 0)
    {
      // Opening with no entry price leaves every later PnL computed against
      // zero -- wrong in a way nothing downstream can detect. Refuse.
      sink_(OrderRejected{0, cfg_.id, RejectReason::AdjustmentNeedsEntry, a.accountId, 0});
      return;
    }
    Position& p = positions_[a.accountId];
    p.qtyRaw += a.qtyDeltaRaw;
    if (a.entryRaw != 0)
    {
      p.entryRaw = a.entryRaw;
    }
    if (p.qtyRaw == 0)
    {
      // Flat is flat: an entry left on a zero position reads like it means
      // something and means nothing.
      p.entryRaw = 0;
    }
    PositionAdjusted ev{};
    ev.account = a.accountId;
    ev.symbol = cfg_.id;
    ev.qtyDeltaRaw = a.qtyDeltaRaw;
    ev.qtyAfterRaw = p.qtyRaw;
    ev.entryAfterRaw = p.entryRaw;
    ev.reason = a.reason;
    std::memcpy(ev.note, a.note, kAdjustNoteLen);
    sink_(ev);
  }

  void updatePerpPosition(uint64_t acct, OrderId orderId, bool fillBuy, int64_t qtyRaw,
                          int64_t priceRaw)
  {
    Position& p = positions_[acct];
    const int64_t fillSign = fillBuy ? 1 : -1;
    int64_t remaining = qtyRaw;
    const int64_t posSign = (p.qtyRaw > 0) ? 1 : (p.qtyRaw < 0 ? -1 : 0);
    if (posSign != 0 && posSign != fillSign)
    {
      const int64_t reduceQty = std::min<int64_t>(remaining, iabs64(p.qtyRaw));
      // realized PnL vs entry (long: (price-entry)*qty; short: (entry-price)*qty)
      if (ledger_ != nullptr)
      {
        const Amount pnl =
            notionalRaw(priceRaw - p.entryRaw, reduceQty, cfg_.priceScale, cfg_.qtyScale) * posSign;
        ledger_->credit(acct, cfg_.quoteAsset, pnl);
        ledger_->credit(venueAccount_, cfg_.quoteAsset, -pnl);
        const Amount relMargin =  // position margin for the reduced portion
            static_cast<Amount>(static_cast<__int128>(p.margin) * reduceQty / iabs64(p.qtyRaw));
        ledger_->release(acct, cfg_.quoteAsset, relMargin);
        p.margin -= relMargin;
        hooks_.releaseOrderIM(hooks_.ctx, orderId, reduceQty, acct);
      }
      p.qtyRaw += fillSign * reduceQty;  // toward zero
      remaining -= reduceQty;
      if (p.qtyRaw == 0)
      {
        p.entryRaw = 0;
      }
    }
    if (remaining > 0)
    {
      const Amount im =
          (ledger_ != nullptr) ? hooks_.consumeOrderIM(hooks_.ctx, orderId, remaining) : 0;
      const int64_t absOld = iabs64(p.qtyRaw);
      const __int128 num =
          static_cast<__int128>(absOld) * p.entryRaw + static_cast<__int128>(remaining) * priceRaw;
      p.entryRaw = static_cast<int64_t>(num / (absOld + remaining));
      p.qtyRaw += fillSign * remaining;
      p.margin += im;
    }
    if (p.qtyRaw == 0)
    {
      if (p.margin > 0 && ledger_ != nullptr)
      {
        ledger_->release(acct, cfg_.quoteAsset, p.margin);
      }
      p.margin = 0;
      positions_.erase(acct);
    }
  }

  // ---- liquidation and ADL -----------------------------------------------
  // Liquidate every position whose equity (posted margin + unrealized PnL) fell
  // below the maintenance requirement. Skipped when an external risk owner
  // drives liquidation: two systems closing one position is worse than either.
  void checkLiquidations(Price mark)
  {
    if (cfg_.externalLiquidation)
    {
      return;
    }
    if (ledger_ == nullptr || !cfg_.linearPerp || cfg_.maintenanceMarginBps == 0)
    {
      return;
    }
    std::vector<uint64_t> toLiq;
    // order: the breaching accounts are id-sorted below, before forceClose
    // emits the first Liquidation
    for (const auto& [acct, p] : positions_)
    {
      const Amount uPnl = unrealizedPnlRaw(acct, mark);
      const Amount notional =
          notionalRaw(mark.raw(), iabs64(p.qtyRaw), cfg_.priceScale, cfg_.qtyScale);
      const Amount mmReq = notional * cfg_.maintenanceMarginBps / 10000;
      // A negative wallet (funding/fees with nothing free to absorb them) drags
      // the equity check; otherwise funding pushes a max-leverage payer
      // unboundedly negative with no liquidation. A healthy (>=0) wallet stays
      // isolated: the drag only tightens the check, never prevents one.
      const Amount wallet = ledger_->available(acct, cfg_.quoteAsset);
      const Amount walletDrag = wallet < 0 ? wallet : 0;
      if (p.margin + uPnl + walletDrag < mmReq)
      {
        toLiq.push_back(acct);
      }
    }
    // forceClose emits Liquidation (folded into the determinism hash) and, with
    // ADL on, shared counterparties make the close order matter: it must not
    // depend on positions_ layout.
    std::sort(toLiq.begin(), toLiq.end());
    for (uint64_t a : toLiq)
    {
      forceClose(a, mark);
    }
  }

  // Close at the mark: realize PnL through the clearing pool, return posted
  // margin, let the insurance fund cover any bankruptcy deficit.
  void forceClose(uint64_t acct, Price mark)
  {
    auto it = positions_.find(acct);
    if (it == positions_.end())
    {
      return;
    }
    const Position p = it->second;
    positions_.erase(it);
    // The account's other resting orders go first: their IM is its own
    // collateral, locked in `reserved`. Freeing it before the bankruptcy test
    // makes a solvent account cover its own shortfall instead of the fund.
    hooks_.cancelAllForAccount(hooks_.ctx, acct);
    const int64_t sign = p.qtyRaw > 0 ? 1 : -1;
    const int64_t qtyAbs = iabs64(p.qtyRaw);
    const Amount uPnl =
        notionalRaw(mark.raw() - p.entryRaw, qtyAbs, cfg_.priceScale, cfg_.qtyScale) * sign;
    ledger_->credit(acct, cfg_.quoteAsset, uPnl);
    ledger_->credit(venueAccount_, cfg_.quoteAsset, -uPnl);
    if (p.margin > 0)
    {
      ledger_->release(acct, cfg_.quoteAsset, p.margin);
    }
    bool bankrupt = false;
    const Amount avail = ledger_->available(acct, cfg_.quoteAsset);
    Amount deficit = 0;
    if (avail < 0)
    {
      bankrupt = true;
      deficit = -avail;
      ledger_->credit(acct, cfg_.quoteAsset, -avail);  // insurance fund tops up to zero
      ledger_->credit(venueAccount_, cfg_.quoteAsset, avail);
    }
    sink_(Liquidation{acct, cfg_.id, Quantity::fromRaw(qtyAbs), mark, bankrupt});
    if (bankrupt && cfg_.autoDeleverage)
    {
      autoDeleverage(sign, deficit, mark);  // claw the deficit back from winners
    }
  }

  // ---- checkpoint: clearing's own records --------------------------------
  // Funding and positions are hashed and written at two different points of
  // the engine's traversal, so these are two pairs of methods, not one. The
  // record order, the tags and the bytes are what the engine wrote inline.
  uint64_t hashFunding(uint64_t h) const
  {
    // Folds in only when set, the "zero == absent" rule the balances follow:
    // an engine that saw no funding hashes as it did before these fields.
    if (fundingRateRaw_ != 0 || fundingIntervalNs_.count() != 0 || nextFundingNs_.raw() != 0)
    {
      h = mix(h, hash_tags::kFunding);
      h = mix(h, static_cast<uint64_t>(fundingRateRaw_));
      h = mix(h, static_cast<uint64_t>(fundingIntervalNs_.count()));
      h = mix(h, static_cast<uint64_t>(nextFundingNs_.raw()));
    }
    return h;
  }

  uint64_t hashPositions(uint64_t h) const
  {
    for (uint64_t acct : sortedAccounts())
    {
      const Position& p = positions_.at(acct);
      h = mix(h, hash_tags::kPosition);
      h = mix(h, acct);
      h = mix(h, static_cast<uint64_t>(p.qtyRaw));
      h = mix(h, static_cast<uint64_t>(p.entryRaw));
      h = mixAmount(h, p.margin);
    }
    return h;
  }

  // Written only when there is funding state, so an engine with none produces
  // the file it did before the record existed (and that file still loads).
  void writeFunding(Journal& out, int64_t ts) const
  {
    if (fundingRateRaw_ != 0 || fundingIntervalNs_.count() != 0 || nextFundingNs_.raw() != 0)
    {
      out.append(InboundCommand{RestoreFunding{fundingRateRaw_, nextFundingNs_, fundingIntervalNs_}},
                 ts);
    }
  }

  void writePositions(Journal& out, int64_t ts) const
  {
    for (uint64_t acct : sortedAccounts())
    {
      const Position& p = positions_.at(acct);
      out.append(InboundCommand{RestorePosition{acct, p.qtyRaw, p.entryRaw, {}, p.margin}}, ts);
    }
  }

  // Restored verbatim, not re-derived -- that is the point of the record.
  void restoreFunding(const RestoreFunding& r)
  {
    fundingRateRaw_ = r.fundingRateRaw;
    nextFundingNs_ = r.nextFundingNs.raw() > 0 ? r.nextFundingNs : SeqNanos{};
    fundingIntervalNs_ = r.fundingIntervalNs.count() > 0 ? r.fundingIntervalNs : DurationNs{};
  }

  // `exactBalanceRestore` says the snapshot carried exact signed balance
  // splits, so the margin is already reserved and must not be moved again.
  bool restorePosition(const RestorePosition& r, bool exactBalanceRestore)
  {
    // A zero quantity is a state the engine really holds, not corruption: an
    // operator correction that flattens a position leaves the entry in place
    // deliberately, because it moves no margin (see adjustPosition and the
    // note on AdjustPosition), and hashPositions folds that entry. Refusing
    // the record here made every checkpoint taken after such a correction
    // unloadable -- the writer and the loader have to describe the same
    // engine. A repeated account is still corruption.
    if (positions_.count(r.account) != 0)
    {
      return false;
    }
    if (ledger_ != nullptr && !exactBalanceRestore && r.marginRaw > 0 &&
        !ledger_->reserve(r.account, cfg_.quoteAsset, r.marginRaw))
    {
      return false;  // deposited total cannot back the posted margin -> corrupt
    }
    positions_[r.account] = Position{r.qtyRaw, r.entryRaw, r.marginRaw};
    return true;
  }

  // The clearing half of the snapshot clone: state, not wiring. The clone's
  // cfg_, sink_ and hooks_ were bound at construction, its ledger by setLedger.
  void copyStateFrom(const Clearing& other)
  {
    positions_ = other.positions_;
    fundingRateRaw_ = other.fundingRateRaw_;
    fundingIntervalNs_ = other.fundingIntervalNs_;
    nextFundingNs_ = other.nextFundingNs_;
  }

 private:
  // Recover a bankruptcy deficit from the most profitable opposite-side
  // positions (close at mark, haircut the gain into insurance) instead of
  // socializing it. Same model as cross_margin.h; every ledger op balances.
  void autoDeleverage(int64_t bankruptSign, Amount deficit, Price mark)
  {
    if (deficit <= 0)
    {
      return;
    }
    // clang-format off
    struct Cand { uint64_t acct; Amount uPnl; };
    // clang-format on
    std::vector<Cand> cands;
    // order: candidates are ranked below by (uPnl, acct), a total order, so
    // the ADL victim does not depend on this traversal
    for (const auto& [oa, pos] : positions_)
    {
      const int64_t s = pos.qtyRaw > 0 ? 1 : (pos.qtyRaw < 0 ? -1 : 0);
      if (s == 0 || s == bankruptSign)
      {
        continue;  // opposite side only
      }
      const Amount up =
          notionalRaw(mark.raw() - pos.entryRaw, pos.qtyRaw, cfg_.priceScale, cfg_.qtyScale);
      if (up > 0)
      {
        cands.push_back({oa, up});
      }
    }
    // Most profitable first; `acct` breaks ties, so the victim (an emitted
    // Liquidation) depends on neither traversal order nor sort instability.
    std::sort(cands.begin(), cands.end(),
              [](const Cand& x, const Cand& y)
              { return x.uPnl != y.uPnl ? x.uPnl > y.uPnl : x.acct < y.acct; });
    Amount remaining = deficit;
    for (const auto& c : cands)
    {
      if (remaining <= 0)
      {
        break;
      }
      auto it = positions_.find(c.acct);
      if (it == positions_.end())
      {
        continue;
      }
      const Position p = it->second;
      positions_.erase(it);
      const int64_t qtyAbs = iabs64(p.qtyRaw);
      const Amount uPnl =
          notionalRaw(mark.raw() - p.entryRaw, p.qtyRaw, cfg_.priceScale, cfg_.qtyScale);
      ledger_->credit(c.acct, cfg_.quoteAsset, uPnl);  // realize gain at mark
      ledger_->credit(venueAccount_, cfg_.quoteAsset, -uPnl);
      if (p.margin > 0)
      {
        ledger_->release(c.acct, cfg_.quoteAsset, p.margin);
      }
      const Amount haircut = remaining < uPnl ? remaining : uPnl;
      // Debit is all-or-nothing and `available` may be below the haircut, so
      // credit the venue only what is confiscated -- an unconditional credit
      // against a no-op debit mints money. The remainder falls to insurance.
      const Amount avail = ledger_->available(c.acct, cfg_.quoteAsset);
      const Amount taken = std::min<Amount>(haircut, avail > 0 ? avail : 0);
      if (taken > 0)
      {
        ledger_->debit(c.acct, cfg_.quoteAsset, taken);
        ledger_->credit(venueAccount_, cfg_.quoteAsset, taken);
      }
      remaining -= taken;
      sink_(Liquidation{c.acct, cfg_.id, Quantity::fromRaw(qtyAbs), mark, /*bankrupt*/ false,
                        /*adl*/ true});
    }
  }

  // Account id ascending: the order the snapshot and the hash are written in.
  std::vector<uint64_t> sortedAccounts() const
  {
    std::vector<uint64_t> keys;
    keys.reserve(positions_.size());
    // order: sorted below, before the vector is handed out
    for (const auto& [acct, p] : positions_)
    {
      (void)p;
      keys.push_back(acct);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
  }

  static int64_t iabs64(int64_t v) { return v < 0 ? -v : v; }
  // Amount is __int128; mix() takes 64 bits at a time. Spelled out here so
  // the component stays standalone -- why cross_margin.h has its own iabs64.
  static uint64_t mixAmount(uint64_t h, Amount a) noexcept
  {
    h = mix(h, static_cast<uint64_t>(static_cast<unsigned __int128>(a)));
    h = mix(h, static_cast<uint64_t>(static_cast<unsigned __int128>(a) >> 64));
    return h;
  }

  const SymbolConfig& cfg_;
  const EventSink& sink_;
  Hooks hooks_;
  Ledger* ledger_{nullptr};
  uint64_t venueAccount_{0};
  std::unordered_map<uint64_t, Position> positions_;  // perp positions per account

  // Last rate applied at kFundingRateScale (published, never matched on) and
  // the live calendar (0 = none: nextFundingNs falls back to the config
  // derivation). All three hashed and checkpointed as RestoreFunding.
  int64_t fundingRateRaw_{0};
  DurationNs fundingIntervalNs_{};
  SeqNanos nextFundingNs_{};
};

}  // namespace flox::venue::engine
