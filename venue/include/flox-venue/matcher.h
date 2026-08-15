/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/event_sink.h"
#include "flox-venue/messages.h"
#include "flox/book/resting_order.h"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <vector>

namespace flox::venue
{

enum class MatchPolicy : uint8_t
{
  PriceTimeFifo = 0,
  ProRata = 1,  // thick-level proportional distribution (crossProRata)
};

struct MatchOutcome
{
  Quantity filled{};
  Quantity leaves{};
  bool takerComplete{false};
  RejectReason reject{RejectReason::None};  // nothing filled when set
  bool residualRests{false};                // GTC limit remainder should rest
  bool residualCanceled{false};             // IOC / MARKET / STP remainder killed
  CancelReason residualCancelReason{CancelReason::UserRequested};
};

// How much of a prospective bite may actually trade, decided by the engine at
// FILL time rather than at admission (see Matcher::setFillLimitHook). `qty` is
// the allowed quantity (the tighter of the two legs), never more than the
// requested one; when it is zero exactly one of the blocked flags says which
// leg refused, and `reason` is the cancel reason that leg earns. The per-leg
// limits are kept separately because pro-rata allocates a whole level at once:
// it bounds each participant by `makerQty` and the level's total by `takerQty`.
struct FillLimit
{
  Quantity qty{};
  Quantity makerQty{};
  Quantity takerQty{};
  bool makerBlocked{false};
  bool takerBlocked{false};
  CancelReason reason{CancelReason::ReduceOnlyNotReducing};
};

namespace detail
{
inline Quantity qmin(Quantity a, Quantity b) noexcept { return (a < b) ? a : b; }
inline Side opposite(Side s) noexcept { return s == Side::BUY ? Side::SELL : Side::BUY; }

// Self-trade-prevention scope is resolved per Matcher via account->firm-group
// membership (Matcher::stpScope); account-level STP is the default when no group
// is registered.

inline bool crosses(Side takerSide, Price takerPrice, OrderType type, Price restingPrice) noexcept
{
  if (type == OrderType::MARKET)
  {
    return true;
  }
  return takerSide == Side::BUY ? takerPrice >= restingPrice : takerPrice <= restingPrice;
}
}  // namespace detail

template <class Book>
class Matcher
{
 public:
  explicit Matcher(MatchPolicy policy = MatchPolicy::PriceTimeFifo) noexcept : policy_(policy) {}

  MatchPolicy policy() const noexcept { return policy_; }

  using LastLookHook = std::function<void(const RestingOrder& maker, Quantity fill,
                                          const NewOrder& taker)>;
  void setLastLookHook(LastLookHook hook) { onLastLook_ = std::move(hook); }

  // Called before the matcher itself removes a resting order (self-trade
  // prevention, or a fill-time risk block). The engine resolves that order's
  // open last-look holds and returns true if it resolved any, i.e. it may have
  // put restored quantity back on the book and the level must be re-peeked.
  // Every other path that removes an order resolves its holds first; the
  // matcher-owned removals reach that discipline through this hook.
  using RestingHoldHook = std::function<bool(OrderId resting)>;
  void setRestingHoldHook(RestingHoldHook hook) { onRestingHolds_ = std::move(hook); }

  // Called when a resting order is trimmed in place (STP decrement) with the
  // quantity before and after. The matcher does not own reservations; the
  // engine frees the part that no longer rests.
  using RestingReducedHook = std::function<void(OrderId resting, int64_t fromQtyRaw, int64_t toQtyRaw)>;
  void setRestingReducedHook(RestingReducedHook hook) { onRestingReduced_ = std::move(hook); }

  // Called for every prospective bite so the engine can re-check the risk
  // limits that depend on state a resting order was NOT sized against (perp
  // reduce-only and the position cap: the position moves while the order
  // rests). Reads engine state only, so it reproduces on replay.
  using FillLimitHook = std::function<FillLimit(const RestingOrder& maker, const NewOrder& taker,
                                                Quantity want)>;
  void setFillLimitHook(FillLimitHook hook) { onFillLimit_ = std::move(hook); }

  // Firm-group STP: map an account to a firm/group id so self-trade prevention
  // fires across all accounts of the same firm, not just the same account.
  // group 0 removes the membership (back to account-level STP), keeping the
  // table canonical for the checkpoint state hash.
  void setStpGroup(uint64_t account, uint64_t group)
  {
    if (group == 0)
    {
      stpGroup_.erase(account);
    }
    else
    {
      stpGroup_[account] = group;
    }
  }

  // Live STP-group table (checkpoint serialization / state hash / clone).
  const std::unordered_map<uint64_t, uint64_t>& stpGroups() const noexcept { return stpGroup_; }

  // Defensive-path counter: pro-rata allocation met a resting lastLook maker
  // (possible only when admission was bypassed) and skipped it instead of
  // filling it as firm. See crossProRata.
  uint64_t skippedLastLookProRata() const noexcept { return skippedLastLookProRata_; }

  // Pro-rata allocation excluded a maker whose fill-time risk limit left it
  // nothing to trade (perp reduce-only against a position that moved, or the
  // position cap). See crossProRata.
  uint64_t skippedRiskProRata() const noexcept { return skippedRiskProRata_; }

  MatchOutcome cross(const NewOrder& order, Book& book,
                     const std::function<uint64_t()>& nextTradeId, const EventSink& sink) const
  {
    if (policy_ == MatchPolicy::ProRata)
    {
      return crossProRata(order, book, nextTradeId, sink);
    }
    using namespace detail;
    MatchOutcome out;
    const Side restingSide = opposite(order.side);
    const bool isMarket = order.type == OrderType::MARKET;

    // POST_ONLY: must never take. Reject outright if the opposite top crosses.
    if (order.postOnly)
    {
      if (RestingOrder* top = book.peekBest(restingSide);
          top && crosses(order.side, order.price, order.type, top->price))
      {
        out.reject = RejectReason::PostOnlyWouldCross;
        return out;
      }
    }

    // FOK: all-or-none. Precheck crossable liquidity before touching the book.
    // With STP on, exclude same-scope liquidity: it will be canceled/decremented
    // rather than traded, so it cannot fill the FOK -- counting it would let the
    // FOK pass the precheck and then rest with 0 fills after STP removes it.
    // With last look active, last-look makers are NON-FIRM liquidity: they can
    // only be held, never guaranteed, so (a) they do not count toward the FOK
    // precheck and (b) a FOK whose crossing range contains ANY last-look maker
    // is rejected outright -- the sweep is strict price-time, so a last-look
    // maker inside the range could be hit before the FOK completes, and a
    // partial execution followed by a hold would violate all-or-none.
    if (order.tif == TimeInForce::FOK)
    {
      const bool lastLookActive = static_cast<bool>(onLastLook_);
      const bool stpActive = (order.stp != STPMode::None);
      auto skipStp = [&](const RestingOrder& m)
      { return stpActive && stpScope(m.accountId) == stpScope(order.accountId); };
      auto skipStpOrLastLook = [&](const RestingOrder& m)
      { return (lastLookActive && m.lastLook) || skipStp(m); };
      const Quantity firm =
          book.availableWithinExcl(order.side, order.price, isMarket, skipStpOrLastLook);
      if (firm < order.quantity)
      {
        out.reject = RejectReason::FillOrKillUnfulfillable;
        return out;
      }
      if (lastLookActive)
      {
        const Quantity withLastLook =
            book.availableWithinExcl(order.side, order.price, isMarket, skipStp);
        if (firm < withLastLook)  // a last-look maker sits inside the crossing range
        {
          out.reject = RejectReason::FillOrKillUnfulfillable;
          return out;
        }
      }
    }

    Quantity leaves = order.quantity;
    bool takerCanceledBySTP = false;
    bool takerRiskBlocked = false;
    CancelReason takerRiskReason = CancelReason::ReduceOnlyNotReducing;

    while (!leaves.isZero())
    {
      RestingOrder* m = book.peekBest(restingSide);
      if (m == nullptr || !crosses(order.side, order.price, order.type, m->price))
      {
        break;
      }

      switch (applySelfTradePrevention(order, *m, book, leaves, sink))
      {
        case StpOutcome::NotApplicable:
          break;
        case StpOutcome::RePeek:
          continue;  // the book changed under us: re-read the best maker
        case StpOutcome::CancelTaker:
          takerCanceledBySTP = true;
          break;
      }
      if (takerCanceledBySTP)
      {
        break;
      }

      // Only the displayed peak is executable per bite; the book refills the
      // hidden reserve (and re-queues at the tail) inside fillBest.
      const Quantity want = qmin(leaves, m->leaves);
      Quantity allowed = want;
      if (onFillLimit_)
      {
        // Fill-time risk re-check (perp reduce-only / position cap). A resting
        // order was sized against the position it saw at admission; by the time
        // it fills that position can be smaller, gone, or on the other side.
        // The disallowed part of the bite simply does not trade.
        const FillLimit lim = onFillLimit_(*m, order, want);
        allowed = lim.qty;
        if (allowed.isZero())
        {
          if (lim.makerBlocked)
          {
            // The maker can no longer trade at all: pull it, holds first (same
            // discipline as the STP removal above).
            const OrderId blockedId = m->id;
            const uint64_t blockedAcct = m->accountId;
            if (onRestingHolds_ && onRestingHolds_(blockedId))
            {
              continue;  // hook may have reshaped the book -- re-peek
            }
            sink(OrderCanceled{blockedId, order.symbol, lim.reason, blockedAcct});
            book.cancel(blockedId);
            continue;
          }
          takerRiskBlocked = true;  // the aggressor is the blocked leg: stop the sweep
          takerRiskReason = lim.reason;
          break;
        }
      }

      // Last look: the maker holds this fill pending its decision. Reserve the
      // hit size out of the book and hand it to the engine; no trade yet.
      if (m->lastLook && onLastLook_)
      {
        const Quantity heldQty = allowed;
        onLastLook_(*m, heldQty, order);
        book.fillBest(restingSide, heldQty);
        leaves -= heldQty;
        continue;
      }

      const Quantity fill = allowed;
      const OrderId makerId = m->id;
      const uint64_t makerAccount = m->accountId;
      const Price makerPrice = m->price;
      // Total (displayed + hidden) remaining after this fill -- so an iceberg
      // reports leaves/complete against its whole size, not just the peak.
      const Quantity makerTotalAfter = (m->leaves + m->hidden) - fill;
      // Displayed peak remaining (for the public feed): a partial peak fill leaves
      // (peak - fill) shown; a full-peak lift refills to min(peak, hidden). Must
      // be read before fillBest, which refills/re-queues and may invalidate `m`.
      const Quantity makerDisplayAfter =
          (fill < m->leaves) ? (m->leaves - fill) : qmin(m->peak, m->hidden);

      sink(Trade{nextTradeId(), order.symbol, makerPrice, fill, makerId, order.id, order.side,
                 makerAccount, order.accountId});

      book.fillBest(restingSide, fill);  // may invalidate `m`
      leaves -= fill;
      out.filled += fill;

      sink(OrderExecuted{makerId, order.symbol, fill, makerTotalAfter, false,
                         makerTotalAfter.isZero(), makerPrice, makerDisplayAfter, makerAccount});
      sink(OrderExecuted{order.id, order.symbol, fill, leaves, true, leaves.isZero(), makerPrice,
                         leaves, order.accountId});
    }

    out.leaves = leaves;

    if (takerCanceledBySTP)
    {
      out.residualCanceled = true;
      out.residualCancelReason = CancelReason::SelfTradePrevention;
    }
    else if (takerRiskBlocked)
    {
      // The aggressor's own limit stopped the sweep: what it was allowed to
      // trade traded, and the rest is killed with the reason it earned rather
      // than resting as an order that may never fill within its limit.
      out.residualCanceled = true;
      out.residualCancelReason = takerRiskReason;
    }
    else if (leaves.isZero())
    {
      out.takerComplete = true;
    }
    else if (isMarket)
    {
      out.residualCanceled = true;
      out.residualCancelReason = CancelReason::MarketResidual;
    }
    else if (order.tif == TimeInForce::IOC)
    {
      out.residualCanceled = true;
      out.residualCancelReason = CancelReason::ImmediateOrCancelResidual;
    }
    else if (order.tif == TimeInForce::FOK)
    {
      // All-or-none: a FOK that did not fully fill must be killed, never rest.
      // The STP-aware precheck above should already have rejected it pre-match;
      // this is the safety net so a FOK residual can never become a resting GTC.
      out.residualCanceled = true;
      out.residualCancelReason = CancelReason::FillOrKillResidual;
    }
    else
    {
      out.residualRests = true;  // GTC / GTD / POST_ONLY that did not cross
    }

    return out;
  }

 private:
  // Self-trade-prevention scope of an account: its firm group if registered,
  // else the account itself (identity -- account-level STP). Fast path when no
  // groups are configured.
  uint64_t stpScope(uint64_t account) const
  {
    if (stpGroup_.empty())
    {
      return account;
    }
    auto it = stpGroup_.find(account);
    return it == stpGroup_.end() ? account : it->second;
  }

  enum class StpOutcome
  {
    NotApplicable,  ///< No self-trade interaction with this maker.
    RePeek,         ///< The book changed; the caller must re-read it.
    CancelTaker,    ///< The aggressor itself must be canceled.
  };

  // Self-trade prevention for one prospective aggressor/maker pair.
  //
  // Both matching policies route through this: an account must never be on
  // both sides of a print, and which policy chooses the counterparty has no
  // bearing on that. Keeping one implementation is the point -- a second copy
  // is how the two drift apart.
  //
  // STP is about to remove (or shrink) the maker, so its open last-look holds
  // are resolved FIRST, exactly as every engine-side cancel path does.
  // Skipping that frees collateral an open hold still needs, and the later
  // accept then settles with no reservation behind it (value created). The
  // hook can restore quantity to the book, hence RePeek; it resolves at most
  // once per maker, so this cannot spin. The aggressor can never be a leg of
  // those holds: STP is tested before the last-look branch on every iteration,
  // so a same-scope maker is removed before it can hold a slice of this order.
  StpOutcome applySelfTradePrevention(const NewOrder& order, const RestingOrder& m, Book& book,
                                      Quantity& leaves, const EventSink& sink) const
  {
    using namespace detail;
    if (order.stp == STPMode::None || stpScope(m.accountId) != stpScope(order.accountId))
    {
      return StpOutcome::NotApplicable;
    }
    if (onRestingHolds_ && onRestingHolds_(m.id))
    {
      return StpOutcome::RePeek;
    }
    switch (order.stp)
    {
      case STPMode::CancelOldest:
        sink(OrderCanceled{m.id, order.symbol, CancelReason::SelfTradePrevention, m.accountId});
        book.cancel(m.id);
        return StpOutcome::RePeek;
      case STPMode::CancelNewest:
        return StpOutcome::CancelTaker;
      case STPMode::CancelBoth:
        sink(OrderCanceled{m.id, order.symbol, CancelReason::SelfTradePrevention, m.accountId});
        book.cancel(m.id);
        return StpOutcome::CancelTaker;
      case STPMode::Decrement:
      {
        // Cancel the smaller leg fully; reduce the larger by the smaller qty.
        // No trade occurs. (Iceberg reserve is ignored for STP.)
        const Quantity restTotal = m.leaves;
        const Quantity dec = qmin(leaves, restTotal);
        if (!(dec < restTotal))  // resting <= incoming: resting fully removed
        {
          sink(OrderCanceled{m.id, order.symbol, CancelReason::SelfTradePrevention, m.accountId});
          book.cancel(m.id);
        }
        else  // incoming smaller: reduce resting, incoming fully decremented
        {
          const int64_t trimmedTo = restTotal.raw() - dec.raw();
          book.reduce(m.id, Quantity::fromRaw(trimmedTo));
          if (onRestingReduced_)
          {
            onRestingReduced_(m.id, restTotal.raw(), trimmedTo);
          }
        }
        leaves = Quantity::fromRaw(leaves.raw() - dec.raw());
        return leaves.isZero() ? StpOutcome::CancelTaker : StpOutcome::RePeek;
      }
      case STPMode::None:
        break;
    }
    return StpOutcome::NotApplicable;
  }

  // Pro-rata matching: at each crossing level distribute the aggressor across all
  // resting orders proportionally to size (deterministic floor + FIFO remainder),
  // for instruments with thick display levels. Icebergs ARE refilled here (via
  // consumeById). Self-trade prevention binds here exactly as it does under
  // price-time, through the shared applySelfTradePrevention, and is applied to
  // the whole level before the split so the allocation only ever runs over
  // participants that may actually trade with this aggressor. Scope limitation:
  // last-look is NOT honoured. The engine refuses lastLook orders on pro-rata instruments
  // at admission (RejectReason::LastLookUnsupported), so a lastLook maker can
  // only appear here if the caller bypassed validate(). DEFENSIVE PATH: such
  // a maker is NOT filled as firm (pro-rata cannot hold a slice, so filling
  // would fake firmness the maker never granted) -- it is skipped, the
  // skippedLastLookProRata counter is bumped, and the allocation runs over
  // the remaining (firm) participants of the level unchanged. A level whose
  // firm size is zero stops the sweep.
  MatchOutcome crossProRata(const NewOrder& order, Book& book,
                            const std::function<uint64_t()>& nextTradeId,
                            const EventSink& sink) const
  {
    using namespace detail;
    MatchOutcome out;
    const Side restingSide = opposite(order.side);
    const bool isMarket = order.type == OrderType::MARKET;

    if (order.postOnly)
    {
      if (RestingOrder* top = book.peekBest(restingSide);
          top && crosses(order.side, order.price, order.type, top->price))
      {
        out.reject = RejectReason::PostOnlyWouldCross;
        return out;
      }
    }
    if (order.tif == TimeInForce::FOK)
    {
      if (book.availableWithin(order.side, order.price, isMarket) < order.quantity)
      {
        out.reject = RejectReason::FillOrKillUnfulfillable;
        return out;
      }
    }

    Quantity leaves = order.quantity;
    // Reused scratch, persistent across aggressors: the level snapshot and the
    // per-order allocation vector. Pro-rata instruments have thick levels, so
    // these are the hot allocations -- keep them off the matching thread's heap.
    // thread_local: one matcher runs on one sequenced-shard thread, and this
    // keeps the FIFO path's zero-alloc guarantee for pro-rata too.
    static thread_local std::vector<RestingOrder> level;
    static thread_local std::vector<int64_t> alloc;
    // Per-participant allocable size: the resting quantity, cut to what the
    // fill-time risk limit allows that maker right now (0 = excluded).
    static thread_local std::vector<int64_t> allocable;
    bool takerCanceledBySTP = false;
    while (!leaves.isZero())
    {
      RestingOrder* top = book.peekBest(restingSide);
      if (top == nullptr || !crosses(order.side, order.price, order.type, top->price))
      {
        break;
      }
      const Price levelPrice = top->price;
      book.bestLevel(restingSide, level);
      if (level.empty())
      {
        break;
      }

      // Self-trade prevention runs BEFORE the allocation, not inside it. The
      // pro-rata split has to be computed over the participants that may
      // actually trade with this aggressor, so every same-scope maker is
      // resolved (canceled, trimmed) and the level re-read first. Any book
      // mutation invalidates the snapshot, hence the restart.
      if (order.stp != STPMode::None)
      {
        bool bookChanged = false;
        for (const RestingOrder& participant : level)
        {
          const StpOutcome so = applySelfTradePrevention(order, participant, book, leaves, sink);
          if (so == StpOutcome::CancelTaker)
          {
            takerCanceledBySTP = true;
            break;
          }
          if (so == StpOutcome::RePeek)
          {
            bookChanged = true;
            break;
          }
        }
        if (takerCanceledBySTP)
        {
          break;
        }
        if (bookChanged)
        {
          continue;  // re-read the level: the snapshot is stale
        }
      }

      // Defensive: a resting lastLook maker here means validate() was bypassed
      // (admission rejects the combination). It is non-firm liquidity and is
      // excluded from the allocation entirely -- see the method comment.
      //
      // Fill-time risk (perp reduce-only / position cap) cuts each participant
      // to what it may still trade; a participant left with nothing is excluded
      // and counted (pro-rata cannot cancel a maker mid-allocation, and leaving
      // it out is what keeps the allocation from opening a position the maker's
      // reduce-only flag forbids). The aggressor's own limit bounds the LEVEL
      // total below -- one bound over the whole level is exactly right, since
      // every fill in it moves the taker's position the same way.
      int64_t tot = 0;
      int64_t takerRoom = leaves.raw();
      allocable.assign(level.size(), 0);
      for (size_t i = 0; i < level.size(); ++i)
      {
        if (level[i].lastLook)
        {
          ++skippedLastLookProRata_;
          continue;
        }
        int64_t sz = level[i].leaves.raw();
        if (onFillLimit_)
        {
          const FillLimit lim = onFillLimit_(level[i], order, Quantity::fromRaw(sz));
          sz = std::min(sz, lim.makerQty.raw());
          takerRoom = std::min(takerRoom, lim.takerQty.raw());
          if (sz <= 0)
          {
            ++skippedRiskProRata_;
            continue;
          }
        }
        allocable[i] = sz;
        tot += sz;
      }
      if (tot == 0 || takerRoom <= 0)
      {
        break;  // nothing firm and allocable here, or the aggressor may take no more
      }
      const int64_t want = std::min<int64_t>(takerRoom, tot);

      alloc.assign(level.size(), 0);  // reuses capacity across levels/aggressors
      int64_t assigned = 0;
      for (size_t i = 0; i < level.size(); ++i)
      {
        if (allocable[i] <= 0)
        {
          continue;  // excluded participant keeps alloc 0
        }
        alloc[i] = static_cast<int64_t>(static_cast<__int128>(want) * allocable[i] / tot);
        assigned += alloc[i];
      }
      int64_t leftover = want - assigned;  // deterministic FIFO remainder
      for (size_t i = 0; i < level.size() && leftover > 0; ++i)
      {
        if (allocable[i] <= 0)
        {
          continue;
        }
        const int64_t room = allocable[i] - alloc[i];
        if (room > 0)
        {
          const int64_t add = std::min(room, leftover);
          alloc[i] += add;
          leftover -= add;
        }
      }

      for (size_t i = 0; i < level.size(); ++i)
      {
        if (alloc[i] <= 0)
        {
          continue;
        }
        const Quantity fill = Quantity::fromRaw(alloc[i]);
        const OrderId makerId = level[i].id;
        // total (displayed + hidden) remaining, so an iceberg reports against
        // its whole size; consumeById refills the peak and re-queues.
        const Quantity makerTotalAfter =
            Quantity::fromRaw(level[i].leaves.raw() + level[i].hidden.raw() - alloc[i]);
        const Quantity makerDisplayAfter =
            (fill < level[i].leaves) ? (level[i].leaves - fill) : qmin(level[i].peak, level[i].hidden);
        sink(Trade{nextTradeId(), order.symbol, levelPrice, fill, makerId, order.id, order.side,
                   level[i].accountId, order.accountId});
        book.consumeById(makerId, fill);
        leaves = Quantity::fromRaw(leaves.raw() - alloc[i]);
        out.filled += fill;
        sink(OrderExecuted{makerId, order.symbol, fill, makerTotalAfter, false,
                           makerTotalAfter.isZero(), levelPrice, makerDisplayAfter,
                           level[i].accountId});
        sink(OrderExecuted{order.id, order.symbol, fill, leaves, true, leaves.isZero(), levelPrice,
                           leaves, order.accountId});
      }
      if (want < tot)
      {
        break;  // level partially consumed; aggressor exhausted
      }
    }

    out.leaves = leaves;
    if (takerCanceledBySTP)
    {
      out.residualCanceled = true;
      out.residualCancelReason = CancelReason::SelfTradePrevention;
    }
    else if (leaves.isZero())
    {
      out.takerComplete = true;
    }
    else if (isMarket)
    {
      out.residualCanceled = true;
      out.residualCancelReason = CancelReason::MarketResidual;
    }
    else if (order.tif == TimeInForce::IOC)
    {
      out.residualCanceled = true;
      out.residualCancelReason = CancelReason::ImmediateOrCancelResidual;
    }
    else
    {
      out.residualRests = true;
    }
    return out;
  }

  MatchPolicy policy_;
  LastLookHook onLastLook_;
  RestingHoldHook onRestingHolds_;
  RestingReducedHook onRestingReduced_;
  FillLimitHook onFillLimit_;
  std::unordered_map<uint64_t, uint64_t> stpGroup_;  // account -> firm group (empty = account-level STP)
  // mutable: cross() is const; these are diagnostic counters, not matching state.
  mutable uint64_t skippedLastLookProRata_{0};
  mutable uint64_t skippedRiskProRata_{0};
};

}  // namespace flox::venue
