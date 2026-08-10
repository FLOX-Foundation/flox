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
  ProRata = 1,  // TODO: thick-level distribution for derivatives
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

  using LastLookHook = std::function<void(const RestingOrder& maker, Quantity fill,
                                          const NewOrder& taker)>;
  void setLastLookHook(LastLookHook hook) { onLastLook_ = std::move(hook); }

  // Firm-group STP: map an account to a firm/group id so self-trade prevention
  // fires across all accounts of the same firm, not just the same account.
  void setStpGroup(uint64_t account, uint64_t group) { stpGroup_[account] = group; }

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
    if (order.tif == TimeInForce::FOK)
    {
      const Quantity avail =
          (order.stp == STPMode::None)
              ? book.availableWithin(order.side, order.price, isMarket)
              : book.availableWithinExcl(order.side, order.price, isMarket, [&](uint64_t maker)
                                         { return stpScope(maker) == stpScope(order.accountId); });
      if (avail < order.quantity)
      {
        out.reject = RejectReason::FillOrKillUnfulfillable;
        return out;
      }
    }

    Quantity leaves = order.quantity;
    bool takerCanceledBySTP = false;

    while (!leaves.isZero())
    {
      RestingOrder* m = book.peekBest(restingSide);
      if (m == nullptr || !crosses(order.side, order.price, order.type, m->price))
      {
        break;
      }

      if (order.stp != STPMode::None && stpScope(m->accountId) == stpScope(order.accountId))
      {
        switch (order.stp)
        {
          case STPMode::CancelOldest:
            sink(OrderCanceled{m->id, order.symbol, CancelReason::SelfTradePrevention});
            book.cancel(m->id);
            continue;
          case STPMode::CancelNewest:
            takerCanceledBySTP = true;
            break;
          case STPMode::CancelBoth:
            sink(OrderCanceled{m->id, order.symbol, CancelReason::SelfTradePrevention});
            book.cancel(m->id);
            takerCanceledBySTP = true;
            break;
          case STPMode::Decrement:
          {
            // Cancel the smaller leg fully; reduce the larger by the smaller qty.
            // No trade occurs. (Iceberg reserve is ignored for STP.)
            const Quantity restTotal = m->leaves;
            const Quantity dec = qmin(leaves, restTotal);
            if (!(dec < restTotal))  // resting <= incoming: resting fully removed
            {
              sink(OrderCanceled{m->id, order.symbol, CancelReason::SelfTradePrevention});
              book.cancel(m->id);
            }
            else  // incoming smaller: reduce resting, incoming fully decremented
            {
              book.reduce(m->id, Quantity::fromRaw(restTotal.raw() - dec.raw()));
            }
            leaves = Quantity::fromRaw(leaves.raw() - dec.raw());
            if (leaves.isZero())
            {
              takerCanceledBySTP = true;
              break;
            }
            continue;  // incoming still has qty: re-peek next maker
          }
          case STPMode::None:
            break;
        }
        if (takerCanceledBySTP)
        {
          break;
        }
      }

      // Only the displayed peak is executable per bite; the book refills the
      // hidden reserve (and re-queues at the tail) inside fillBest.
      // Last look: the maker holds this fill pending its decision. Reserve the
      // hit size out of the book and hand it to the engine; no trade yet.
      if (m->lastLook && onLastLook_)
      {
        const Quantity heldQty = qmin(leaves, m->leaves);
        onLastLook_(*m, heldQty, order);
        book.fillBest(restingSide, heldQty);
        leaves -= heldQty;
        continue;
      }

      const Quantity fill = qmin(leaves, m->leaves);
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
                         makerTotalAfter.isZero(), makerPrice, makerDisplayAfter});
      sink(OrderExecuted{order.id, order.symbol, fill, leaves, true, leaves.isZero(), makerPrice,
                         leaves});
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

  // Pro-rata matching: at each crossing level distribute the aggressor across all
  // resting orders proportionally to size (deterministic floor + FIFO remainder),
  // for instruments with thick display levels. Scope: no
  // iceberg refill and STP treated as None on pro-rata instruments.
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
      int64_t tot = 0;
      for (const auto& o : level)
      {
        tot += o.leaves.raw();
      }
      const int64_t want = std::min<int64_t>(leaves.raw(), tot);

      alloc.assign(level.size(), 0);  // reuses capacity across levels/aggressors
      int64_t assigned = 0;
      for (size_t i = 0; i < level.size(); ++i)
      {
        alloc[i] = static_cast<int64_t>(static_cast<__int128>(want) * level[i].leaves.raw() / tot);
        assigned += alloc[i];
      }
      int64_t leftover = want - assigned;  // deterministic FIFO remainder
      for (size_t i = 0; i < level.size() && leftover > 0; ++i)
      {
        const int64_t room = level[i].leaves.raw() - alloc[i];
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
                           makerTotalAfter.isZero(), levelPrice, makerDisplayAfter});
        sink(OrderExecuted{order.id, order.symbol, fill, leaves, true, leaves.isZero(), levelPrice,
                           leaves});
      }
      if (want < tot)
      {
        break;  // level partially consumed; aggressor exhausted
      }
    }

    out.leaves = leaves;
    if (leaves.isZero())
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
  std::unordered_map<uint64_t, uint64_t> stpGroup_;  // account -> firm group (empty = account-level STP)
};

}  // namespace flox::venue
