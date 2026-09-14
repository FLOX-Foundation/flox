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

// One bite an all-or-none precheck has approved, in the order it walked the
// crossing range. `allowedRaw` is what the risk limits left that maker at plan
// time; `blocked` says the maker itself may not trade at all, which is the
// only case where an allowance of zero means "pull it from the book".
struct PlannedBite
{
  OrderId maker{};
  int64_t allowedRaw{};
  bool blocked{false};
  CancelReason reason{CancelReason::ReduceOnlyNotReducing};
};

// Signed position changes (raw contracts) an all-or-none precheck has already
// simulated, keyed by account. A crossing range holds a handful of distinct
// accounts, so a linear scan beats a hash map here and allocates nothing after
// the first few entries.
class PositionDeltas
{
 public:
  int64_t of(uint64_t account) const noexcept
  {
    for (const auto& [a, d] : v_)
    {
      if (a == account)
      {
        return d;
      }
    }
    return 0;
  }
  void add(uint64_t account, int64_t delta)
  {
    for (auto& [a, d] : v_)
    {
      if (a == account)
      {
        d += delta;
        return;
      }
    }
    v_.emplace_back(account, delta);
  }
  void clear() noexcept { v_.clear(); }

 private:
  std::vector<std::pair<uint64_t, int64_t>> v_;
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

  // The same question as the fill-limit hook, asked about a state that has not
  // happened yet: `deltas` carries the signed position change each account
  // would already have taken on earlier in the same sweep. The all-or-none
  // precheck needs it. Measuring every maker against the position as it stands
  // right now answers a different question from the one the sweep will ask,
  // because each print moves the position the next maker is measured against --
  // which is how an order whose precheck said "fully fillable" ends up half
  // printed and half killed.
  using FillLimitDryHook = std::function<FillLimit(const RestingOrder& maker,
                                                   const NewOrder& taker, Quantity want,
                                                   const PositionDeltas& deltas)>;
  void setFillLimitDryHook(FillLimitDryHook hook) { onFillLimitDry_ = std::move(hook); }

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

  // All-or-none accounting, in the order the questions are asked:
  // how many prechecks ran, how many of those met a maker a risk limit would
  // not let trade in full, and how many ended in a refusal. The middle one is
  // the population that a "refuse on any risk-limited maker in range" rule
  // would refuse, so it says what that rule would cost on live flow.
  uint64_t fillOrKillPrechecks() const noexcept { return fokPrechecks_; }
  uint64_t fillOrKillRiskConstrained() const noexcept { return fokRiskConstrained_; }
  uint64_t fillOrKillRejected() const noexcept { return fokRejected_; }

  MatchOutcome cross(const NewOrder& order, Book& book,
                     const std::function<uint64_t()>& nextTradeId, const EventSink& sink) const
  {
    planActive_ = false;
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

    if (order.tif == TimeInForce::FOK && !planFillOrKill(order, book, isMarket))
    {
      out.reject = RejectReason::FillOrKillUnfulfillable;
      return out;
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
      if (planActive_ || onFillLimit_)
      {
        // Fill-time risk re-check (perp reduce-only / position cap). A resting
        // order was sized against the position it saw at admission; by the time
        // it fills that position can be smaller, gone, or on the other side.
        // The disallowed part of the bite simply does not trade.
        //
        // An all-or-none order spends its plan here instead. Its limits were
        // settled before the first print, on the same state and the same
        // thread, precisely so that no answer arriving mid-sweep can leave it
        // half filled with nothing to undo.
        const FillLimit lim =
            planActive_ ? plannedFillLimit(*m, order, want) : onFillLimit_(*m, order, want);
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
      spendPlan(makerId, fill.raw());
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
  // All-or-none plan: walk the crossing range once, decide every bite the
  // sweep will take, and refuse before a single trade prints unless those
  // bites add up to the whole order.
  //
  // Three classes of depth are visible in the book and cannot fill the order:
  //   - same-STP-scope makers, which the sweep cancels or decrements instead of
  //     trading;
  //   - last-look makers, which are non-firm -- they can only be held, never
  //     guaranteed. One inside the crossing range is enough to refuse the order
  //     outright: the sweep is strict price-time, so it could be hit before the
  //     FOK completes, and a partial print followed by a hold is exactly the
  //     outcome all-or-none exists to rule out;
  //   - depth a fill-time risk limit will not let trade (a reduce-only maker on
  //     a perp, the position cap). This one has to be measured maker by maker
  //     in sweep order, because each prospective print moves the position the
  //     next maker is measured against: two reduce-only sells of 10 against a
  //     long of 10 look like depth 20 to the book and are worth 10 at the fill.
  //
  // The plan is what makes the promise whole. Asking the risk limits again in
  // the middle of the sweep asks about a state the earlier prints have already
  // moved, and a different answer arriving after the first print cannot be
  // acted on: a venue does not un-print a trade. So the question is asked once,
  // before anything is printed, and the sweep spends the answer.
  //
  // Walking stops at the taker's own quantity. Past that point the sweep never
  // reaches, and counting simulated position moves the sweep will not make is
  // how an order that fills perfectly well gets refused.
  bool planFillOrKill(const NewOrder& order, const Book& book, bool isMarket) const
  {
    using namespace detail;
    const bool lastLookActive = static_cast<bool>(onLastLook_);
    const bool stpActive = (order.stp != STPMode::None);
    auto skipStp = [&](const RestingOrder& m)
    { return stpActive && stpScope(m.accountId) == stpScope(order.accountId); };

    // Reused across aggressors on the matching thread, like the pro-rata
    // scratch: one matcher runs on one sequenced-shard thread.
    static thread_local PositionDeltas deltas;
    deltas.clear();
    plan_.clear();
    planCursor_ = 0;
    const int64_t wantRaw = order.quantity.raw();
    int64_t planned = 0;
    bool riskConstrained = false;

    auto measure = [&](const RestingOrder& m)
    {
      if ((lastLookActive && m.lastLook) || skipStp(m))
      {
        return true;  // excluded from the total entirely
      }
      const bool covered = (planned >= wantRaw);
      const Quantity total = m.leaves + m.hidden;
      PlannedBite bite;
      bite.maker = m.id;
      bite.allowedRaw = total.raw();
      if (onFillLimitDry_)
      {
        const FillLimit lim = onFillLimitDry_(m, order, total, deltas);
        bite.allowedRaw = std::min(lim.qty.raw(), total.raw());
        if (bite.allowedRaw < 0)
        {
          bite.allowedRaw = 0;
        }
        bite.blocked = lim.makerBlocked;
        bite.reason = lim.reason;
        riskConstrained = riskConstrained || (bite.allowedRaw < total.raw());
      }
      if (bite.allowedRaw > 0 && !covered)
      {
        // Past the point where the plan covers the order the aggressor stops
        // moving: it will not take more than it asked for. Adding simulated
        // position for prints the sweep is never going to make is how an order
        // that fills perfectly well ends up refused, and how a maker deeper in
        // the range is measured against a position that never happens.
        const int64_t signedQty = bite.allowedRaw;
        deltas.add(m.accountId, m.side == Side::BUY ? signedQty : -signedQty);
        deltas.add(order.accountId, order.side == Side::BUY ? signedQty : -signedQty);
      }
      planned += bite.allowedRaw;
      plan_.push_back(bite);
      return false;
    };

    const Quantity firm = book.availableWithinExcl(order.side, order.price, isMarket, measure);
    ++fokPrechecks_;
    if (riskConstrained)
    {
      ++fokRiskConstrained_;
    }
    if (planned < wantRaw)
    {
      ++fokRejected_;
      return false;
    }
    if (lastLookActive)
    {
      const Quantity withLastLook =
          book.availableWithinExcl(order.side, order.price, isMarket, skipStp);
      if (firm < withLastLook)  // a last-look maker sits inside the crossing range
      {
        ++fokRejected_;
        return false;
      }
    }
    planActive_ = true;
    return true;
  }

  // The plan's entry for `maker`, or null when the plan does not cover it --
  // which means the sweep has walked past the end of the plan, and the live
  // limit answers instead of an invented allowance.
  //
  // The sweep visits makers in the order the plan wrote them down, so the scan
  // starts where the last one was found and wraps once. A market-priced
  // all-or-none order plans the whole opposite book; scanning it from the top
  // for every bite would make the sweep quadratic in the depth it crosses.
  PlannedBite* plannedBite(OrderId maker) const
  {
    const size_t n = plan_.size();
    for (size_t i = 0; i < n; ++i)
    {
      const size_t at = (planCursor_ + i) % n;
      if (plan_[at].maker == maker)
      {
        planCursor_ = at;
        return &plan_[at];
      }
    }
    return nullptr;
  }

  // What the plan allows for one bite of `maker`. Read only: an iceberg is
  // bitten once per displayed peak and the pro-rata allocator asks about every
  // participant before it decides, so the allowance is drawn down by
  // spendPlan once the quantity is actually settled.
  FillLimit plannedFillLimit(const RestingOrder& maker, const NewOrder& order,
                             Quantity want) const
  {
    const PlannedBite* bite = plannedBite(maker.id);
    if (bite == nullptr)
    {
      return onFillLimit_
                 ? onFillLimit_(maker, order, want)
                 : FillLimit{want, want, want, false, false, CancelReason::ReduceOnlyNotReducing};
    }
    const int64_t give = std::min(want.raw(), bite->allowedRaw);
    FillLimit lim;
    lim.qty = Quantity::fromRaw(give > 0 ? give : 0);
    lim.makerQty = lim.qty;
    lim.takerQty = want;  // the plan already bounded the aggressor's own leg
    lim.reason = bite->reason;
    // A spent allowance is always the MAKER's exhaustion, never the taker's.
    // The aggressor's own room was settled once, over the whole order, before
    // anything printed; blocking it here would stop a sweep the plan has
    // already promised will complete, and a stopped all-or-none sweep is a
    // partial print. The maker, on the other hand, has traded everything its
    // limits allow, which is exactly the resting order the sweep pulls.
    lim.makerBlocked = lim.qty.isZero();
    return lim;
  }

  void spendPlan(OrderId maker, int64_t qtyRaw) const
  {
    if (!planActive_ || qtyRaw <= 0)
    {
      return;
    }
    if (PlannedBite* bite = plannedBite(maker); bite != nullptr)
    {
      bite->allowedRaw -= std::min(qtyRaw, bite->allowedRaw);
    }
  }

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
        // No trade occurs. "Smaller" is measured on what the resting order
        // actually holds -- displayed peak plus hidden reserve -- not on what
        // it shows. Comparing against the peak makes an iceberg look like the
        // smaller leg whenever the aggressor is bigger than one peak, and the
        // rule then cancels an order many times the aggressor's size.
        const Quantity restTotal = m.leaves + m.hidden;
        const Quantity dec = qmin(leaves, restTotal);
        if (!(dec < restTotal))  // resting <= incoming: resting fully removed
        {
          sink(OrderCanceled{m.id, order.symbol, CancelReason::SelfTradePrevention, m.accountId});
          book.cancel(m.id);
        }
        else  // incoming smaller: reduce resting, incoming fully decremented
        {
          const int64_t trimmedTo = restTotal.raw() - dec.raw();
          book.reduceTotal(m.id, Quantity::fromRaw(trimmedTo));
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
    // Same all-or-none precheck price-time runs. Pro-rata used to count raw
    // depth here, which counts liquidity the STP pass is about to remove: the
    // allocation then had nothing to distribute and the residual fell through
    // to the resting branch below as a GTC.
    if (order.tif == TimeInForce::FOK && !planFillOrKill(order, book, isMarket))
    {
      out.reject = RejectReason::FillOrKillUnfulfillable;
      return out;
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
        if (planActive_)
        {
          // All-or-none spends the allowance its plan already approved for this
          // maker. The aggressor's own leg was bounded once, over the whole
          // plan, so `takerRoom` stays at what is left of the order.
          const PlannedBite* bite = plannedBite(level[i].id);
          sz = (bite != nullptr) ? std::min(sz, bite->allowedRaw) : sz;
          if (sz <= 0)
          {
            ++skippedRiskProRata_;
            continue;
          }
        }
        else if (onFillLimit_)
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
        spendPlan(makerId, alloc[i]);
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
    else if (order.tif == TimeInForce::FOK)
    {
      // The same safety net cross() carries: whatever the precheck concluded, a
      // FOK residual must never become a resting GTC.
      out.residualCanceled = true;
      out.residualCancelReason = CancelReason::FillOrKillResidual;
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
  FillLimitDryHook onFillLimitDry_;
  std::unordered_map<uint64_t, uint64_t> stpGroup_;  // account -> firm group (empty = account-level STP)
  // mutable: cross() is const; these are diagnostic counters, not matching state.
  mutable uint64_t skippedLastLookProRata_{0};
  mutable uint64_t skippedRiskProRata_{0};
  mutable uint64_t fokPrechecks_{0};
  mutable uint64_t fokRiskConstrained_{0};
  mutable uint64_t fokRejected_{0};
  // The bites an all-or-none plan approved, and whether one is in force. Both
  // live for the length of one cross() on the matching thread; cross() resets
  // the flag on entry so no other time-in-force can inherit a stale plan.
  mutable std::vector<PlannedBite> plan_;
  mutable size_t planCursor_{0};
  mutable bool planActive_{false};
};

}  // namespace flox::venue
