/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/event_hash.h"
#include "flox-venue/journal.h"
#include "flox-venue/ledger.h"
#include "flox-venue/matcher.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/messages.h"
#include "flox-venue/stop_book.h"
#include "flox/book/resting_order.h"

#include "flox/backtest/fee_schedule.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace flox::venue
{

// TriggerRef (the conditional-order reference price selector) lives in
// messages.h next to the SetTriggerRef command that carries it.

struct SymbolConfig
{
  SymbolId id{};
  Price tickSize{};           // 0 = unchecked
  Quantity lotSize{};         // 0 = unchecked
  Quantity minQty{};          // 0 = unchecked
  Price minPrice{};           // 0 = unchecked (price band / collar lower bound)
  Price maxPrice{};           // 0 = unchecked (price band / collar upper bound)
  Quantity maxOrderQty{};     // 0 = unchecked (fat-finger max size)
  Volume maxOrderNotional{};  // 0 = unchecked (fat-finger max notional, limit orders)
  bool halted{false};
  TriggerRef triggerRef{TriggerRef::Last};
  DurationNs lastLookWindowNs{};        // 0 = last look disabled venue-wide
  bool lastLookAcceptOnTimeout{false};  // window elapses with no decision -> accept vs reject
  // Symmetric price tolerance. When set, the VENUE decides whether the price
  // moved too far during the hold, and it applies the same threshold in both
  // directions: outside it, the fill is rejected whoever it would have
  // favoured.
  //
  // This is what removes the free option. A maker allowed to answer a held
  // fill however it likes will, over enough samples, fill the ones that moved
  // its way and refuse the ones that did not -- and that asymmetry is
  // invisible to the taker, who sees only a reject rate. Enforcing magnitude
  // at the venue leaves nothing to be asymmetric about outside the band, and
  // lastLookStats makes what happens inside it visible.
  //
  // 0 = no venue-side check: the maker's answer stands whatever the price did.
  int64_t lastLookToleranceRaw{0};
  AssetId baseAsset{0};             // e.g. BTC in BTC-USD (settled to the seller/buyer)
  AssetId quoteAsset{1};            // e.g. USD in BTC-USD
  int32_t luldBps{0};               // limit-up/limit-down band around the reference (0 = off)
  DurationNs luldHaltNs{};          // trading pause LENGTH on a band breach
  bool linearPerp{false};           // derivatives: linear perpetual (margin, no asset delivery)
  int32_t initialMarginBps{0};      // IM as bps of notional (1000 = 10% = 10x leverage)
  int32_t maintenanceMarginBps{0};  // MM; position liquidated when equity < MM (0 = off)
  // Liquidation decided elsewhere (portfolio margin above the per-symbol
  // engines). The engine still posts isolated IM and settles, but never
  // liquidates on its own -- it closes only on ForceClosePosition.
  bool externalLiquidation{false};
  bool autoDeleverage{false};  // ADL: recover a bankruptcy deficit from winners before insurance
  Quantity maxPositionQty{};   // 0 = unchecked (max |position| per account, perp risk cap)
  uint32_t maxOpenOrders{0};   // 0 = unchecked (max live resting orders per account)

  // Per-symbol fixed-point scale, same semantics as core SymbolInfo. Default
  // 1e8 = the compile-time Price/Quantity scale. Money always settles at
  // kMoneyScale regardless. Must satisfy scalesValid().
  int64_t priceScale{Price::Scale};
  int64_t qtyScale{Quantity::Scale};

  // Startup FALLBACK funding interval (0 = the venue does not fund this
  // instrument). The engine settles funding only when told to (the sequenced
  // ApplyFunding command); this is the calendar it publishes when no schedule
  // has been set -- the next boundary of a fixed-interval grid anchored at the
  // sequencer-ts origin, a function of (now, interval). The AUTHORITATIVE
  // calendar is engine state set by SetFundingSchedule, which overrides this;
  // see MatchingEngine::nextFundingNs. Excluded from configHash for the same
  // reason the other mutable knobs are: it reinterprets no stored number, so it
  // cannot invalidate a snapshot.
  DurationNs fundingIntervalNs{};
};

// Book selects the resting-book implementation. The default MatchingBook is
// the allocation-heavy correctness oracle (fine for backtests and tests, needs
// no per-symbol config). For latency-sensitive simulation instantiate with
// flox::LadderBook and pass a configured instance:
//   MatchingEngine<LadderBook> e(cfg, sink, LadderBook{ladderCfg});
// Equivalence of the two books is enforced by test_venue_differential_fuzz.
template <class Book = MatchingBook>
class MatchingEngine
{
 public:
  MatchingEngine(SymbolConfig cfg, EventSink sink, Book book = Book{},
                 MatchPolicy policy = MatchPolicy::PriceTimeFifo)
      : cfg_(cfg), sink_(std::move(sink)), matcher_(policy), book_(std::move(book))
  {
    assert(scalesValid(cfg_.priceScale, cfg_.qtyScale));
    // Wrap the user sink to observe the trade stream (last price for stops,
    // fees, MM-protection counters) and maintain per-account resting state.
    emit_ = [this](const OutboundEvent& e)
    {
      if (const auto* t = std::get_if<Trade>(&e))
      {
        lastPrice_ = t->price;
        hasLast_ = true;
        onTradeObserved(*t);
      }
      else if (const auto* x = std::get_if<OrderExecuted>(&e))
      {
        // Hold-aware: an order can report complete while a last-look hold on it
        // is still pending (its held slice already left `leaves`); releasing the
        // whole reservation here would strand the eventual accept with nothing
        // to settle from. Cleanup is deferred to resolveHeld in that case.
        if (x->complete && !hasHoldsFor(x->id))
        {
          releaseReservation(x->id);  // free any over-reserved remainder
          forgetOrder(x->id);
        }
      }
      else if (const auto* c = std::get_if<OrderCanceled>(&e))
      {
        // Only the matcher's own removals (self-trade prevention, a fill-time
        // risk block) reach this branch -- every engine-side cancel path emits
        // through sink_ and does its own cleanup. Those removals resolve the
        // order's holds first (Matcher's resting-hold hook), so the common case
        // has no hold left here. The guard is the belt to that brace: stripping
        // the reservation while a hold is still open would leave the eventual
        // accept nothing to settle from, and settlement without a reservation
        // is exactly the path that used to mint value. Hold-aware, like the
        // executed branch above: release only what no hold needs, and defer
        // dropping the order until resolveHeld cleans it up.
        if (!hasHoldsFor(c->id))
        {
          releaseReservation(c->id);
          forgetOrder(c->id);
        }
        else
        {
          releaseReservationExceptHeld(c->id);
        }
      }
      sink_(e);
      if (const auto* t = std::get_if<Trade>(&e))
      {
        settleTrade(*t);  // move base/quote + fees; emits FeeCharged via sink_
      }
    };

    // Last look: the matcher hands each held fill here. lastLookWindowNs == 0
    // means last look is disabled venue-wide (as the config promises): the hook
    // is never installed, so a lastLook-flagged maker fills like any other.
    if (cfg_.lastLookWindowNs.count() > 0)
    {
      matcher_.setLastLookHook(
          [this](const RestingOrder& maker, Quantity fill, const NewOrder& taker)
          { createHeld(maker, fill, taker); });
      // The matcher removes resting orders on two of its own paths (STP and a
      // fill-time risk block). Both must resolve that order's open holds first,
      // like every engine-side cancel path does -- otherwise the removal frees
      // collateral an open hold still needs. Only wired when last look is on:
      // with the window at 0 no hold can exist.
      matcher_.setRestingHoldHook(
          [this](OrderId resting)
          {
            if (!hasHoldsFor(resting))
            {
              return false;
            }
            rejectHoldsFor(resting);
            return true;  // liquidity may have been restored: the caller re-peeks
          });
    }

    // An STP decrement trims a resting order in place. Freeing the reservation
    // for the part that no longer rests has nothing to do with last look, so
    // this is wired unconditionally -- inside the block above it would only
    // work on instruments that happen to enable holds.
    matcher_.setRestingReducedHook([this](OrderId id, int64_t fromQtyRaw, int64_t toQtyRaw)
                                   { releaseReservationPro(id, fromQtyRaw, toQtyRaw); });

    // Fill-time perp risk re-check. Spot has no positions to re-check, so the
    // hook stays unwired there and the matching hot path is untouched.
    if (cfg_.linearPerp)
    {
      matcher_.setFillLimitHook([this](const RestingOrder& maker, const NewOrder& taker,
                                       Quantity want)
                                { return fillLimit(maker, taker, want); });
    }
  }

  void submit(const InboundCommand& cmd) { submit(cmd, SeqNanos::fromRaw(++timeCounter_)); }

  // Timestamped submit (sequencer-stamped). Drives last-look expiry and MMP
  // windows deterministically.
  // The ingestion boundary is the one legitimate crossing into sequencer time:
  // the caller hands a raw tick (wall-derived at capture, journal-derived on
  // replay) and it becomes SeqNanos here, in exactly one place.
  void submit(const InboundCommand& cmd, int64_t tsRawNs) { submit(cmd, SeqNanos::fromRaw(tsRawNs)); }

  void submit(const InboundCommand& cmd, SeqNanos tsNs)
  {
    // Snapshot-only records are forbidden in live traffic: a client that could
    // sneak a Restore* through submit would "restore" itself an order or a
    // balance. They apply exclusively through applySnapshotRecord (recovery).
    // Dropping (not rejecting per-order) is deterministic on replay too: a
    // journaled stray record is dropped identically.
    if (isSnapshotRecord(cmd))
    {
      ++droppedSnapshotRecords_;
      std::fprintf(stderr, "flox-venue: dropped snapshot-only record (tag %zu) from live traffic\n",
                   cmd.index());
      return;
    }
    now_ = tsNs;
    if (cfg_.halted && static_cast<bool>(haltUntil_) && now_ >= haltUntil_)
    {
      cfg_.halted = false;  // timed LULD volatility pause elapsed
      haltUntil_ = SeqNanos{};
      publishStatus(TradingStatusReason::LuldPauseElapsed);
    }
    expireHolds();
    expireOrders();
    if (const auto* n = std::get_if<NewOrder>(&cmd))
    {
      onNew(*n);
    }
    else if (const auto* c = std::get_if<CancelOrder>(&cmd))
    {
      onCancel(*c);
    }
    else if (const auto* m = std::get_if<ModifyOrder>(&cmd))
    {
      onModify(*m);
    }
    else if (const auto* mc = std::get_if<MassCancel>(&cmd))
    {
      onMassCancel(*mc);
    }
    else if (const auto* q = std::get_if<Quote>(&cmd))
    {
      onQuote(*q);
    }
    else if (const auto* ll = std::get_if<LastLookDecision>(&cmd))
    {
      onLastLookDecision(*ll);
    }
    else if (const auto* sm = std::get_if<SetMark>(&cmd))
    {
      setMarkPrice(sm->mark);  // sequenced -> journaled -> replayed (liquidations reproduce)
    }
    else if (const auto* af = std::get_if<ApplyFunding>(&cmd))
    {
      applyFunding(af->rate, af->mark);
    }
    else if (const auto* ad = std::get_if<AdminCmd>(&cmd))
    {
      onAdmin(ad->action);  // sequenced -> journaled -> replayed (auction/halt reproduce)
    }
    else if (const auto* d = std::get_if<Deposit>(&cmd))
    {
      onDeposit(*d);  // journaled genesis: replay from an empty ledger reproduces balances
    }
    else if (const auto* w = std::get_if<Withdraw>(&cmd))
    {
      onWithdraw(*w);
    }
    else if (const auto* sb = std::get_if<SetBands>(&cmd))
    {
      if (sb->symbol == cfg_.id)
      {
        setPriceBand(sb->minPrice, sb->maxPrice);  // sequenced -> journaled -> replayed
      }
    }
    else if (const auto* st = std::get_if<SetTriggerRef>(&cmd))
    {
      if (st->symbol == cfg_.id)
      {
        setTriggerRef(st->ref);  // sequenced -> journaled -> replayed
      }
    }
    else if (const auto* rl = std::get_if<SetRiskLimits>(&cmd))
    {
      applyRiskLimits(*rl);  // sequenced -> journaled -> replayed
    }
    else if (const auto* ap = std::get_if<SetAdmissionProfile>(&cmd))
    {
      setAdmissionProfile(ap->account, ap->profile);  // sequenced -> journaled -> replayed
    }
    else if (const auto* sg = std::get_if<SetStpGroup>(&cmd))
    {
      if (sg->symbol == cfg_.id)
      {
        setStpGroup(sg->account, sg->group);  // sequenced -> journaled -> replayed
      }
    }
    else if (const auto* fs = std::get_if<SetFundingSchedule>(&cmd))
    {
      if (fs->symbol == cfg_.id)
      {
        setFundingSchedule(fs->intervalNs, fs->nextFundingNs);  // sequenced -> journaled -> replayed
      }
    }
    else if (const auto* fc = std::get_if<ForceClosePosition>(&cmd))
    {
      if (fc->symbol == cfg_.id)
      {
        onForceClose(*fc);
      }
    }
    else if (std::get_if<TimeTick>(&cmd) != nullptr)
    {
      // Pure time sweep: expireHolds/expireOrders already ran above. Sequenced
      // and journaled so a replay reproduces the same timeouts (see tick()).
    }
    // ListInstrument is consumed above the engine (InstrumentRegistry / router);
    // an existing engine has nothing to do with its own listing record.
    processOco();
    repeg();
    mmpEnforce();
  }

  // Idempotent time sweep: advance engine time and resolve overdue last-look
  // holds and GTD expiries without any order-flow traffic (a quiet symbol must
  // not hold liquidity forever). NOT journaled here -- when the engine runs
  // under a SequencedShard, route the sweep through the command stream as a
  // TimeTick so a replay reproduces the timeouts (the shard's idle sweeper does
  // exactly that). Direct callers (tests, embedded use) may call this freely.
  // Same ingestion boundary as submit(): a raw tick becomes sequencer time
  // here and nowhere deeper.
  void tick(int64_t nowRawNs)
  {
    const SeqNanos nowNs = SeqNanos::fromRaw(nowRawNs);
    if (nowNs > now_)
    {
      now_ = nowNs;
    }
    expireHolds();
    expireOrders();
  }

  // Open last-look holds (approximate cross-thread gauge for the idle sweeper).
  uint64_t openHolds() const noexcept { return heldOpen_.load(std::memory_order_relaxed); }

  void setFeeSchedule(flox::FeeSchedule fees)
  {
    fees_ = std::move(fees);
    feesEnabled_ = true;
  }

  // Market-maker protection: if `qtyLimit` is filled for `account` within
  // `windowNs`, all its resting orders are pulled.
  void setMmp(uint64_t account, Quantity qtyLimit, DurationNs windowNs)
  {
    mmpCfg_[account] = MmpCfg{qtyLimit, windowNs};
  }

  // Pre-trade credit / buying-power gate. Returns true if the account may place
  // the order. A real deployment binds this to the account/balance service.
  // What an external risk owner is told about an order it must approve. The
  // old shape (account, side, price, quantity) could not answer a portfolio
  // question: it did not say WHICH instrument -- the engine knows its own, the
  // risk layer serves many -- nor whether the order reduces exposure, and a
  // bare false gave the client no reason for the refusal.
  struct CreditRequest
  {
    OrderId order{};
    uint64_t account{};
    SymbolId symbol{};
    Side side{};
    OrderType type{};
    Price price{};
    Quantity quantity{};
    bool reduceOnly{false};
  };
  struct CreditDecision
  {
    bool allowed{true};
    RejectReason reason{RejectReason::InsufficientFunds};  // used when allowed == false
  };
  using CreditCheck = std::function<CreditDecision(const CreditRequest&)>;
  void setCreditCheck(CreditCheck c) { credit_ = std::move(c); }

  // Bind a settlement ledger. When set, order entry reserves buying power
  // (quote for a bid, base for an ask), fills settle base<->quote and fees, and
  // cancels release the remainder. `venueAccount` receives net fees.
  void setLedger(Ledger* ledger, uint64_t venueAccount = 0)
  {
    ledger_ = ledger;
    venueAccount_ = venueAccount;
  }

  // Perp position queries (signed contracts; average entry).
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

  // Live resting orders tracked on this symbol (observability gauge).
  uint64_t restingOrderCount() const noexcept { return orderAccount_.size(); }

  // Sequencer-ts of the last command the engine applied. This is the ONLY
  // honest "when" an outbound event has: the events themselves carry no time,
  // and a wall-clock read taken downstream would not reproduce on replay. A
  // publisher that stamps market data with it (market_data.h) produces the same
  // timestamps on a journal replay as it did live.
  SeqNanos engineTime() const noexcept { return now_; }
  int64_t engineTimeNs() const noexcept { return now_.raw(); }

  // Open interest: the long side of the perp positions the engine tracks for
  // this symbol. Every contract has a long and a short leg, so the long side is
  // the open interest; summing signed quantities would give zero. Integer sum
  // over an unordered map -- addition is associative, so the result does not
  // depend on the map's layout.
  Quantity openInterest() const
  {
    int64_t oi = 0;
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

  // Next funding boundary, in sequencer time. A schedule set by the operator
  // (SetFundingSchedule) is a FACT: it is engine state, hashed, checkpointed
  // and advanced by each ApplyFunding, so what the feed publishes is what the
  // venue will actually settle on. With no schedule set the value falls back to
  // the historical derivation from (now, SymbolConfig::fundingIntervalNs) --
  // a computation over startup config, kept so an engine that never learned a
  // schedule behaves exactly as it did before the command existed. 0 = neither
  // a schedule nor a configured interval, i.e. the venue does not fund this
  // instrument.
  SeqNanos nextFundingNs() const noexcept
  {
    if (static_cast<bool>(nextFundingNs_))
    {
      return nextFundingNs_;
    }
    if (cfg_.fundingIntervalNs.count() <= 0)
    {
      return SeqNanos{};
    }
    // Boundary alignment is modular math on one domain's raw ticks; the result
    // is restated as the same domain.
    return SeqNanos::fromRaw((now_.raw() / cfg_.fundingIntervalNs.count() + 1) *
                             cfg_.fundingIntervalNs.count());
  }

  // The live funding interval: the operator-set schedule when there is one,
  // otherwise the startup config value it falls back to.
  int64_t fundingIntervalNs() const noexcept
  {
    return (fundingIntervalNs_.count() > 0 ? fundingIntervalNs_ : cfg_.fundingIntervalNs).count();
  }

  // Last funding rate the engine applied, at kFundingRateScale. Carried by the
  // checkpoint (RestoreFunding), so a restored engine publishes the real rate
  // instead of 0 until the next ApplyFunding.
  int64_t fundingRateRaw() const noexcept { return fundingRateRaw_; }

  // Operator-set funding calendar. Sequenced as SetFundingSchedule in
  // production (journaled, replayed, checkpointed); this direct setter is the
  // pre-start wiring / recovery path, like setStpGroup. A non-positive interval
  // or boundary clears the schedule, dropping back to the config derivation.
  void setFundingSchedule(DurationNs intervalNs, SeqNanos nextFundingNs)
  {
    fundingIntervalNs_ = intervalNs.count() > 0 ? intervalNs : DurationNs{};
    nextFundingNs_ = nextFundingNs.raw() > 0 ? nextFundingNs : SeqNanos{};
    // The calendar is a published field, so a change is news -- but only for an
    // instrument the engine has a mark for. Publishing before the first SetMark
    // would break the feed's standing promise that an unmarked instrument gets
    // no DerivativesUpdate at all (the first mark carries the new schedule
    // anyway).
    if (hasMark_)
    {
      publishDerivatives(markPrice_);
    }
  }

  // Current trading state, derived from the engine's own session / halt /
  // pause / auction flags -- the same value the transition events publish.
  // Closed wins over everything: a closed session is the instrument's outermost
  // state, and the halt or auction phase underneath it is preserved untouched
  // so reopening returns to exactly the state the close interrupted.
  TradingStatus tradingStatus() const noexcept
  {
    if (delisted_)
    {
      return TradingStatus::Delisted;
    }
    if (closed_)
    {
      return TradingStatus::Closed;
    }
    if (auctionMode_)
    {
      return TradingStatus::AuctionPreOpen;
    }
    if (cfg_.halted)
    {
      return static_cast<bool>(haltUntil_) ? TradingStatus::LuldPause : TradingStatus::Halted;
    }
    return TradingStatus::Trading;
  }

  // Session state (see AdminAction::CloseSession / OpenSession).
  bool sessionClosed() const noexcept { return closed_; }

  // Withdrawn from trading (see AdminAction::Delist / Relist).
  bool delisted() const noexcept { return delisted_; }

  struct OrderView
  {
    OrderId id{};
    Side side{};
    Price price{};
    Quantity leaves{};  // total remaining (displayed + hidden)
  };
  struct PendingStopView
  {
    OrderId id{};
    Side side{};
    OrderType type{};
    Price trigger{};
    Quantity quantity{};
  };
  struct AccountSnapshot
  {
    std::vector<OrderView> openOrders;
    std::vector<PendingStopView> pendingStops;  // conditional orders not yet triggered
    int64_t positionQty{0};                     // signed perp contracts (0 for spot / flat)
    Price positionEntry{};
  };

  // Client reconnect reconciliation: the account's live resting orders and perp
  // position. Balances come from the ledger; combine at the gateway. Off the
  // hot path (a query, not order flow).
  AccountSnapshot snapshotAccount(uint64_t acct) const
  {
    AccountSnapshot s;
    if (auto it = byAccount_.find(acct); it != byAccount_.end())
    {
      for (OrderId id : it->second)
      {
        if (const RestingOrder* r = book_.find(id))
        {
          s.openOrders.push_back(
              {id, r->side, r->price, Quantity::fromRaw(r->leaves.raw() + r->hidden.raw())});
        }
      }
    }
    stops_.forEachPending(
        [&](const NewOrder& o, Price trigger)
        {
          if (o.accountId == acct)
          {
            s.pendingStops.push_back({o.id, o.side, o.type, trigger, o.quantity});
          }
        });
    s.positionQty = positionQty(acct);
    s.positionEntry = positionEntry(acct);
    return s;
  }

  // Total posted position margin across accounts (quote raw). After all resting
  // orders are drained, this must equal the ledger's total reserved -- every
  // reserved unit is backed by either a live order or an open position.
  Amount totalPositionMargin() const
  {
    Amount t = 0;
    for (const auto& [acct, p] : positions_)
    {
      (void)acct;
      t += p.margin;
    }
    return t;
  }

  // Apply a funding payment (perps): each position transfers |notional|*rate
  // to/from the clearing pool -- longs pay when rate > 0. `mark` values the leg.
  void applyFunding(double rate, Price mark)
  {
    // The rate is known whether or not there is a ledger to settle against, and
    // it is what the derivatives feed publishes -- record it before the early
    // return, or a venue running without a bound ledger would never publish one.
    fundingRateRaw_ = static_cast<int64_t>(rate * static_cast<double>(kFundingRateScale));
    advanceFundingSchedule();
    if (ledger_ == nullptr)
    {
      publishDerivatives(mark);
      return;
    }
    for (auto& [acct, p] : positions_)
    {
      const Amount notional =
          notionalRaw(mark.raw(), iabs64(p.qtyRaw), cfg_.priceScale, cfg_.qtyScale);
      const Amount mag = static_cast<Amount>(static_cast<double>(notional) * rate);
      const Amount signedPay = (p.qtyRaw > 0) ? -mag : mag;  // long pays when rate>0
      ledger_->credit(acct, cfg_.quoteAsset, signedPay);
      ledger_->credit(venueAccount_, cfg_.quoteAsset, -signedPay);
    }
    // Funding is charged to the wallet (`available`); when a max-leverage payer
    // has no free collateral, that drives `available` negative, which the
    // wallet-drag term in checkLiquidations now counts against maintenance
    // equity -- so an unaffordable funding payment triggers liquidation instead
    // of accruing silent bad debt.
    checkLiquidations(mark);
    publishDerivatives(mark);
  }

  // External mark-price update (derivatives). Drives mark-referenced stops and
  // maintenance-margin liquidations.
  void setMarkPrice(Price mark)
  {
    markPrice_ = mark;
    hasMark_ = true;
    processTriggers();
    checkLiquidations(mark);
    // After the consequences, so the published open interest matches the state
    // the mark left behind (a liquidation the mark caused has already closed
    // its position).
    publishDerivatives(mark);
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
    return notionalRaw(mark.raw() - it->second.entryRaw, iabs64(it->second.qtyRaw),
                       cfg_.priceScale, cfg_.qtyScale) *
           sign;
  }

  const Book& book() const noexcept { return book_; }
  uint64_t tradesGenerated() const noexcept { return tradeSeq_; }

  const SymbolConfig& config() const noexcept { return cfg_; }

  // Control-plane hook (and the AdminCmd Halt/Resume path). Clears any timed
  // pause deadline on resume, so the state the feed publishes is the state the
  // engine is actually in.
  void setHalted(bool halted)
  {
    cfg_.halted = halted;
    if (!halted)
    {
      haltUntil_ = SeqNanos{};
    }
    publishStatus(TradingStatusReason::Administrative);
  }

  // Live risk-limit adjustment (control-plane): operators tighten these during
  // volatility without a restart. Only the risk knobs are mutable -- structural
  // fields (symbol id, tick size, assets, linearPerp) stay fixed. Applies to
  // subsequent orders; existing resting orders are unaffected.
  // Apply only the limits the record claims. The direct setters below still
  // exist for pre-start wiring; on a running engine this is the route that
  // survives a restart and reproduces on a replica.
  void applyRiskLimits(const SetRiskLimits& r) noexcept
  {
    if ((r.fields & RiskLimitField::RiskLuld) != 0)
    {
      cfg_.luldBps = r.luldBps;
      cfg_.luldHaltNs = r.luldHaltNs;
    }
    if ((r.fields & RiskLimitField::RiskFatFinger) != 0)
    {
      cfg_.maxOrderQty = r.maxOrderQty;
      cfg_.maxOrderNotional = r.maxOrderNotional;
    }
    if ((r.fields & RiskLimitField::RiskMaxOpenOrders) != 0)
    {
      cfg_.maxOpenOrders = r.maxOpenOrders;
    }
    if ((r.fields & RiskLimitField::RiskMaxPosition) != 0)
    {
      cfg_.maxPositionQty = r.maxPositionQty;
    }
    if ((r.fields & RiskLimitField::RiskMargin) != 0)
    {
      cfg_.initialMarginBps = r.initialMarginBps;
      cfg_.maintenanceMarginBps = r.maintenanceMarginBps;
    }
  }

  // The live limits, as a record that would reproduce them.
  SetRiskLimits riskLimits() const noexcept
  {
    SetRiskLimits r;
    r.symbol = cfg_.id;
    r.fields = RiskLimitField::RiskLuld | RiskLimitField::RiskFatFinger |
               RiskLimitField::RiskMaxOpenOrders | RiskLimitField::RiskMaxPosition |
               RiskLimitField::RiskMargin;
    r.luldBps = cfg_.luldBps;
    r.luldHaltNs = cfg_.luldHaltNs;
    r.maxOrderQty = cfg_.maxOrderQty;
    r.maxOrderNotional = cfg_.maxOrderNotional;
    r.maxOpenOrders = cfg_.maxOpenOrders;
    r.maxPositionQty = cfg_.maxPositionQty;
    r.initialMarginBps = cfg_.initialMarginBps;
    r.maintenanceMarginBps = cfg_.maintenanceMarginBps;
    return r;
  }

  void setLuldBps(int32_t bps) noexcept { cfg_.luldBps = bps; }
  void setTriggerRef(TriggerRef ref) noexcept { cfg_.triggerRef = ref; }
  void setPriceBand(Price minPrice, Price maxPrice) noexcept
  {
    cfg_.minPrice = minPrice;
    cfg_.maxPrice = maxPrice;
  }
  void setFatFinger(Quantity maxOrderQty, Volume maxOrderNotional) noexcept
  {
    cfg_.maxOrderQty = maxOrderQty;
    cfg_.maxOrderNotional = maxOrderNotional;
  }
  void setPositionLimit(Quantity maxPositionQty) noexcept { cfg_.maxPositionQty = maxPositionQty; }
  void setMaxOpenOrders(uint32_t n) noexcept { cfg_.maxOpenOrders = n; }
  void setMarginBps(int32_t initialBps, int32_t maintenanceBps) noexcept
  {
    cfg_.initialMarginBps = initialBps;
    cfg_.maintenanceMarginBps = maintenanceBps;
  }

  // Firm-group STP: register an account under a firm/group id so self-trade
  // prevention fires across all of a firm's accounts, not just the same account
  // (group 0 removes the membership). Runtime mutations MUST arrive as the
  // sequenced SetStpGroup command (journaled, snapshot-carried, hashed) --
  // this direct setter is for pre-start() wiring and the recovery path.
  void setStpGroup(uint64_t account, uint64_t group) { matcher_.setStpGroup(account, group); }

  // Admission profile of one account. A default-constructed profile clears the
  // entry (back to "everything permitted"), which keeps the table canonical
  // for the checkpoint state hash.
  void setAdmissionProfile(uint64_t account, const AdmissionProfile& p)
  {
    if (p.allowedTypes == 0 && p.allowedTif == 0 && p.deny == 0)
    {
      admission_.erase(account);
      return;
    }
    admission_[account] = p;
  }

  const std::unordered_map<uint64_t, AdmissionProfile>& admissionProfiles() const noexcept
  {
    return admission_;
  }

  // Orders refused because the sender was not entitled to send them. Non-zero
  // means a counterparty is sending what it may not -- visible immediately
  // rather than a week later as a position nobody can explain.
  uint64_t admissionRejects() const noexcept { return admissionRejects_; }

  // Pro-rata defensive-path counter (see Matcher::crossProRata): a resting
  // lastLook maker met by a pro-rata allocation was skipped, not filled firm.
  uint64_t skippedLastLookProRata() const noexcept { return matcher_.skippedLastLookProRata(); }

  // Operator emergency stop: halt the symbol (reject new orders) AND pull the
  // entire resting book -- every live limit order and pending conditional --
  // releasing reservations. Used on a fat-finger event or system anomaly.
  void haltAndCancelAll()
  {
    cfg_.halted = true;
    haltUntil_ = SeqNanos{};  // an operator halt has no deadline, unlike the LULD pause
    // The halt reaches the feed BEFORE the flood of cancels it causes, so a
    // subscriber reads them as consequences of a halt rather than as an
    // unexplained mass cancel.
    publishStatus(TradingStatusReason::Administrative);
    cancelEntireBook(CancelReason::VenueHalt);
  }

  // Pull every resting and pending order. Shared by the emergency halt and by
  // delisting, because "nothing survives" has exactly one correct
  // implementation and a second copy is how the two drift apart.
  void cancelEntireBook(CancelReason reason)
  {
    // Resolve every open hold first: restored quantity lands back on the book
    // and is then swept by the loop below, so nothing survives.
    rejectAllHolds();
    std::vector<OrderId> resting;
    for (const auto& [acct, ids] : byAccount_)
    {
      (void)acct;
      resting.insert(resting.end(), ids.begin(), ids.end());
    }
    std::sort(resting.begin(), resting.end());  // deterministic cancel order (layout-independent)
    for (OrderId id : resting)
    {
      if (book_.cancel(id).has_value())
      {
        const uint64_t acct = ownerOf(id);
        releaseReservation(id);
        forgetOrder(id);
        sink_(OrderCanceled{id, cfg_.id, reason, acct});
      }
    }
    for (OrderId id : stops_.ids())
    {
      const uint64_t acct = stops_.accountOf(id);
      if (stops_.cancel(id))
      {
        releaseReservation(id);
        sink_(OrderCanceled{id, cfg_.id, reason, acct});
      }
    }
  }

  // ---- session ----
  // Close the trading session: new orders are rejected with MarketClosed, and
  // the resting book STANDS (a close is not a cancel-all -- an operator who
  // wants the book pulled has HaltAndCancelAll). The halt / auction state
  // underneath is deliberately untouched, so a close during a halt reopens
  // still halted: the session boundary must not silently clear an exception
  // state an operator raised for a reason.
  //
  // The engine owns the STATE, never the calendar: nothing here fires on a
  // clock. The schedule that decides when to send CloseSession / OpenSession is
  // the operator's / control plane's (docs/venue/runtime.md).
  void closeSession()
  {
    closed_ = true;
    publishStatus(TradingStatusReason::Session);
  }

  void openSession()
  {
    closed_ = false;
    publishStatus(TradingStatusReason::Session);
  }

  // Withdraw the instrument from trading. Unlike a halt or a closed session,
  // this carries no promise of a return, so leaving orders resting would leave
  // them waiting for an open that is not coming -- the book is pulled on the
  // way out, through the same path an emergency halt uses.
  void delist()
  {
    if (delisted_)
    {
      return;
    }
    delisted_ = true;
    // The status reaches the feed before the cancels it causes, so a subscriber
    // reads them as a consequence rather than as an unexplained mass cancel.
    publishStatus(TradingStatusReason::Administrative);
    cancelEntireBook(CancelReason::VenueHalt);
  }

  void relist()
  {
    delisted_ = false;
    publishStatus(TradingStatusReason::Administrative);
  }

  // ---- session / auctions ----
  // Pre-open: orders accumulate without matching (a crossed book is allowed).
  void beginPreOpen()
  {
    auctionMode_ = true;
    publishStatus(TradingStatusReason::Auction);
  }

  // Dispatch a sequenced operator action (see AdminCmd). Routing admin actions
  // through the command stream (not direct method calls) is what makes them
  // survive journal replay / HA -- the auction uncross fills and the emergency
  // cancel-all are reproduced at the same point relative to the orders.
  void onAdmin(AdminAction a)
  {
    switch (a)
    {
      case AdminAction::BeginPreOpen:
        beginPreOpen();
        break;
      case AdminAction::OpenContinuous:
        openContinuous();
        break;
      case AdminAction::ResumeAuction:
        resumeWithAuction();
        break;
      case AdminAction::HaltAndCancelAll:
        haltAndCancelAll();
        break;
      case AdminAction::Halt:
        setHalted(true);
        break;
      case AdminAction::Resume:
        setHalted(false);
        break;
      case AdminAction::CloseSession:
        closeSession();
        break;
      case AdminAction::OpenSession:
        openSession();
        break;
      case AdminAction::Delist:
        delist();
        break;
      case AdminAction::Relist:
        relist();
        break;
    }
  }

  // Resume a halted symbol through a re-opening auction: clear the halt (stop
  // rejecting) and enter pre-open accumulation. Orders build a (possibly crossed)
  // book without matching until the operator calls openContinuous(), which
  // uncrosses at the single volume-maximizing price and switches to continuous.
  // This is how venues reopen after a halt -- never straight into continuous.
  void resumeWithAuction()
  {
    cfg_.halted = false;
    haltUntil_ = SeqNanos{};
    auctionMode_ = true;
    publishStatus(TradingStatusReason::Auction);
  }
  // Run the (opening / closing) uncross auction: match everything at the single
  // volume-maximizing price, then resume continuous trading.
  void openContinuous()
  {
    // The uncross is a state of its own for exactly as long as it runs: a
    // subscriber must be able to attribute the burst of fills to it rather than
    // to continuous trading that has not resumed yet.
    emitStatus(TradingStatus::AuctionUncross, TradingStatusReason::Auction, 0);
    runAuction();
    auctionMode_ = false;
    publishStatus(TradingStatusReason::Auction);
  }
  // Explicit uncross without changing session mode (closing auction, etc.).
  void runAuction()
  {
    std::vector<std::pair<Price, Quantity>> bids;
    std::vector<std::pair<Price, Quantity>> asks;
    book_.levels(Side::BUY, bids);
    book_.levels(Side::SELL, asks);
    if (bids.empty() || asks.empty())
    {
      return;
    }
    // Candidate prices = every distinct order price. Pick the one maximizing
    // executable volume; tie-break on smallest demand/supply imbalance.
    std::vector<Price> cands;
    for (const auto& b : bids)
    {
      cands.push_back(b.first);
    }
    for (const auto& a : asks)
    {
      cands.push_back(a.first);
    }
    std::sort(cands.begin(), cands.end(), [](Price x, Price y)
              { return x.raw() < y.raw(); });
    cands.erase(std::unique(cands.begin(), cands.end(),
                            [](Price x, Price y)
                            { return x.raw() == y.raw(); }),
                cands.end());

    bool have = false;
    Price bestP{};
    Quantity bestExec{};
    __int128 bestImb = 0;
    for (Price p : cands)
    {
      Quantity dem{};
      for (const auto& b : bids)
      {
        if (!(b.first < p))
        {
          dem += b.second;  // bid price >= p
        }
      }
      Quantity sup{};
      for (const auto& a : asks)
      {
        if (!(p < a.first))
        {
          sup += a.second;  // ask price <= p
        }
      }
      const Quantity exec = (dem < sup) ? dem : sup;
      if (exec.raw() == 0)
      {
        continue;
      }
      __int128 imb = static_cast<__int128>(dem.raw()) - static_cast<__int128>(sup.raw());
      if (imb < 0)
      {
        imb = -imb;
      }
      if (!have || bestExec < exec || (exec.raw() == bestExec.raw() && imb < bestImb))
      {
        have = true;
        bestExec = exec;
        bestP = p;
        bestImb = imb;
      }
    }
    if (!have)
    {
      return;
    }

    // Uncross at bestP: repeatedly match best bid vs best ask, all at bestP.
    const Price P = bestP;
    while (true)
    {
      RestingOrder* bid = book_.peekBest(Side::BUY);
      RestingOrder* ask = book_.peekBest(Side::SELL);
      if (bid == nullptr || ask == nullptr || bid->price < P || P < ask->price)
      {
        break;
      }
      Quantity fill = (bid->leaves < ask->leaves) ? bid->leaves : ask->leaves;
      const OrderId bidId = bid->id;
      const OrderId askId = ask->id;
      const uint64_t bAcct = bid->accountId;
      const uint64_t aAcct = ask->accountId;
      // Self-trade prevention. An auction has no aggressor -- both legs are
      // resting -- so the mode is read off each order and applied from the
      // requester's point of view: its counterparty is the "oldest" leg (it is
      // resting) and its own order is the "newest". When both legs ask, the
      // cancellations union, which needs no precedence rule between modes and
      // lands the same way whichever order the book hands them to us in.
      if (stpScope(bAcct) == stpScope(aAcct))
      {
        const STPMode bidStp = stpOf(bidId);
        const STPMode askStp = stpOf(askId);
        if (bidStp != STPMode::None || askStp != STPMode::None)
        {
          if (bidStp == STPMode::Decrement || askStp == STPMode::Decrement)
          {
            // Trim both legs by the overlap; no print, and whatever is left of
            // the larger leg stays in the auction.
            decrementForStp(bidId, fill);
            decrementForStp(askId, fill);
            continue;
          }
          bool killBid = false, killAsk = false;
          if (bidStp == STPMode::CancelOldest || bidStp == STPMode::CancelBoth)
          {
            killAsk = true;
          }
          if (bidStp == STPMode::CancelNewest || bidStp == STPMode::CancelBoth)
          {
            killBid = true;
          }
          if (askStp == STPMode::CancelOldest || askStp == STPMode::CancelBoth)
          {
            killBid = true;
          }
          if (askStp == STPMode::CancelNewest || askStp == STPMode::CancelBoth)
          {
            killAsk = true;
          }
          if (killBid)
          {
            cancelForStp(bidId, bAcct);
          }
          if (killAsk)
          {
            cancelForStp(askId, aAcct);
          }
          continue;  // this pair never prints
        }
      }

      // The uncross prints its own fills instead of going through the matcher,
      // so it applies the fill-time perp limits itself: an auction is a fill
      // moment like any other, and a reduce-only order must not flip a position
      // (with no margin) just because it was filled here.
      if (cfg_.linearPerp && ledger_ != nullptr)
      {
        const FillLimit lim = pairFillLimit(aAcct, Side::SELL, ask->reduceOnly, bAcct, Side::BUY,
                                            bid->reduceOnly, fill);
        if (lim.qty.isZero())
        {
          // Nothing this pair may trade: pull the blocked leg so the uncross
          // moves on to the next order at this price (and terminates).
          const OrderId blocked = lim.makerBlocked ? askId : bidId;
          const uint64_t blockedAcct = lim.makerBlocked ? aAcct : bAcct;
          book_.cancel(blocked);
          releaseReservation(blocked);
          forgetOrder(blocked);
          sink_(OrderCanceled{blocked, cfg_.id, lim.reason, blockedAcct});
          continue;
        }
        fill = lim.qty;
      }
      emit_(Trade{++tradeSeq_, cfg_.id, P, fill, askId, bidId, Side::BUY, aAcct, bAcct});
      book_.consumeById(bidId, fill);
      book_.consumeById(askId, fill);
      const RestingOrder* b2 = book_.find(bidId);
      const RestingOrder* a2 = book_.find(askId);
      const Quantity bl = b2 ? Quantity::fromRaw(b2->leaves.raw() + b2->hidden.raw()) : Quantity{};
      const Quantity al = a2 ? Quantity::fromRaw(a2->leaves.raw() + a2->hidden.raw()) : Quantity{};
      // Post-consume displayed peak (b2/a2 already refilled) for the public feed.
      const Quantity bDisp = b2 ? b2->leaves : Quantity{};
      const Quantity aDisp = a2 ? a2->leaves : Quantity{};
      emit_(OrderExecuted{bidId, cfg_.id, fill, bl, false, bl.isZero(), P, bDisp, bAcct});
      emit_(OrderExecuted{askId, cfg_.id, fill, al, false, al.isZero(), P, aDisp, aAcct});
    }
  }

  // ---- checkpoint (journal-format snapshot) ----

  // Deterministic digest of the full engine state over a canonical traversal
  // (event_hash-style FNV fold): instrument config and market state, sequence
  // counters, book (price levels best-first, FIFO within each level), stops,
  // pegs, open holds, positions, MMP config, clientOrderId dedup sets and
  // ledger balances (available AND reserved per account x asset). Written into
  // SnapshotBegin/SnapshotEnd and re-verified on load: a mismatch marks the
  // snapshot corrupt and recovery falls back a generation.
  uint64_t stateHash() const
  {
    uint64_t h = 0xcbf29ce484222325ULL;
    h = mix(h, cfg_.id);
    h = mix(h, cfg_.halted ? 1U : 0U);
    h = mix(h, static_cast<uint64_t>(cfg_.minPrice.raw()));
    h = mix(h, static_cast<uint64_t>(cfg_.maxPrice.raw()));
    h = mix(h, static_cast<uint64_t>(cfg_.triggerRef));
    h = mix(h, auctionMode_ ? 1U : 0U);
    if (delisted_)
    {
      h = mix(h, 0xB00EU);  // only when set: an engine that never delisted hashes as before
    }
    h = mix(h, static_cast<uint64_t>(haltUntil_.raw()));
    // Session and funding state fold in only when they are set, the same
    // "zero == absent" rule the balance traversal follows. An engine that has
    // never been closed and never seen a funding rate or schedule therefore
    // hashes exactly as it did before these fields existed -- which is what
    // lets a snapshot written without them still verify on load.
    if (closed_)
    {
      h = mix(h, 0xB00AU);
      h = mix(h, 1U);
    }
    if (fundingRateRaw_ != 0 || fundingIntervalNs_.count() != 0 || nextFundingNs_.raw() != 0)
    {
      h = mix(h, 0xB00BU);
      h = mix(h, static_cast<uint64_t>(fundingRateRaw_));
      h = mix(h, static_cast<uint64_t>(fundingIntervalNs_.count()));
      h = mix(h, static_cast<uint64_t>(nextFundingNs_.raw()));
    }
    h = mix(h, hasLast_ ? 1U : 0U);
    h = mix(h, static_cast<uint64_t>(lastPrice_.raw()));
    h = mix(h, hasMark_ ? 1U : 0U);
    h = mix(h, static_cast<uint64_t>(markPrice_.raw()));
    h = mix(h, tradeSeq_);
    h = mix(h, heldSeq_);
    h = mix(h, static_cast<uint64_t>(timeCounter_));
    h = mix(h, static_cast<uint64_t>(now_.raw()));

    book_.forEachOrder(
        [&](const RestingOrder& o)
        {
          h = mix(h, 0xB001U);
          h = mix(h, o.id);
          h = mix(h, o.accountId);
          h = mix(h, static_cast<uint64_t>(o.price.raw()));
          h = mix(h, static_cast<uint64_t>(o.leaves.raw()));
          h = mix(h, static_cast<uint64_t>(o.hidden.raw()));
          h = mix(h, static_cast<uint64_t>(o.peak.raw()));
          h = mix(h, static_cast<uint64_t>(o.side));
          h = mix(h, o.lastLook ? 1U : 0U);
          h = mix(h, o.reduceOnly ? 1U : 0U);
          h = mix(h, static_cast<uint64_t>(expiryOf(o.id).raw()));
          h = mix(h, ocoOf(o.id));
        });

    for (const auto& [o, trig] : sortedStops())
    {
      h = mix(h, 0xB002U);
      h = mix(h, o.id);
      h = mix(h, o.accountId);
      h = mix(h, static_cast<uint64_t>(o.side));
      h = mix(h, static_cast<uint64_t>(o.type));
      h = mix(h, static_cast<uint64_t>(o.price.raw()));
      h = mix(h, static_cast<uint64_t>(o.quantity.raw()));
      h = mix(h, static_cast<uint64_t>(o.tif));
      h = mix(h, static_cast<uint64_t>(o.visibleQuantity.raw()));
      h = mix(h, static_cast<uint64_t>(o.triggerPrice.raw()));
      h = mix(h, static_cast<uint64_t>(o.trailingOffset.raw()));
      h = mix(h, o.lastLook ? 1U : 0U);
      h = mix(h, o.reduceOnly ? 1U : 0U);
      h = mix(h, static_cast<uint64_t>(o.expiryNs.raw()));
      h = mix(h, o.ocoGroup);
      h = mix(h, static_cast<uint64_t>(trig.raw()));
    }

    for (uint64_t acct : sortedKeys(admission_))
    {
      const AdmissionProfile& p = admission_.at(acct);
      h = mix(h, 0xB00DU);
      h = mix(h, acct);
      h = mix(h, p.allowedTypes);
      h = mix(h, p.allowedTif);
      h = mix(h, static_cast<uint64_t>(p.deny));
    }
    for (OrderId id : sortedKeys(orderStp_))
    {
      h = mix(h, 0xB00CU);
      h = mix(h, static_cast<uint64_t>(id));
      h = mix(h, static_cast<uint64_t>(orderStp_.at(id)));
    }
    for (OrderId id : sortedKeys(pegged_))
    {
      const Peg& p = pegged_.at(id);
      h = mix(h, 0xB003U);
      h = mix(h, id);
      h = mix(h, static_cast<uint64_t>(p.side));
      h = mix(h, static_cast<uint64_t>(p.ref));
      h = mix(h, static_cast<uint64_t>(p.offsetRaw));
    }

    for (uint64_t hid : sortedKeys(held_))
    {
      const Held& x = held_.at(hid);
      h = mix(h, 0xB004U);
      h = mix(h, x.id);
      h = mix(h, x.taker);
      h = mix(h, x.takerAccount);
      h = mix(h, static_cast<uint64_t>(x.takerSide));
      h = mix(h, x.maker);
      h = mix(h, x.makerAccount);
      h = mix(h, static_cast<uint64_t>(x.price.raw()));
      h = mix(h, static_cast<uint64_t>(x.qty.raw()));
      h = mix(h, static_cast<uint64_t>(x.deadline.raw()));
      h = mix(h, static_cast<uint64_t>(x.takerTif));
      h = mix(h, static_cast<uint64_t>(x.takerType));
      h = mix(h, static_cast<uint64_t>(x.takerPrice.raw()));
      h = mix(h, static_cast<uint64_t>(x.takerExpiryNs.raw()));
      h = mix(h, x.makerReduceOnly ? 1U : 0U);
      h = mix(h, x.takerReduceOnly ? 1U : 0U);
      // Live-tracking truth of the maker (see RestoreHeld::makerTracked).
      h = mix(h, orderAccount_.count(x.maker) != 0 ? 1U : 0U);
    }

    for (OrderId id : sortedKeys(reserve_))
    {
      const Reservation& r = reserve_.at(id);
      h = mix(h, 0xB009U);
      h = mix(h, id);
      h = mix(h, r.account);
      h = mix(h, r.asset);
      h = mix(h, static_cast<uint64_t>(r.side));
      h = mix(h, static_cast<uint64_t>(r.limitPriceRaw));
      h = mixAmount(h, r.reservedRaw);
    }

    for (uint64_t acct : sortedKeys(positions_))
    {
      const Position& p = positions_.at(acct);
      h = mix(h, 0xB005U);
      h = mix(h, acct);
      h = mix(h, static_cast<uint64_t>(p.qtyRaw));
      h = mix(h, static_cast<uint64_t>(p.entryRaw));
      h = mixAmount(h, p.margin);
    }

    for (uint64_t acct : sortedKeys(mmpCfg_))
    {
      const MmpCfg& c = mmpCfg_.at(acct);
      h = mix(h, 0xB006U);
      h = mix(h, acct);
      h = mix(h, static_cast<uint64_t>(c.qtyLimit.raw()));
      h = mix(h, static_cast<uint64_t>(c.windowNs.count()));
    }

    // MMP sliding-window fills, deque (time) order. An EMPTY window contributes
    // nothing, so it is indistinguishable from absence -- which also keeps
    // pre-window-serialization snapshots (that restored windows empty) hashing
    // identically when the windows really were empty.
    for (uint64_t acct : sortedKeys(mmpFills_))
    {
      const MmpWindow& w = mmpFills_.at(acct);
      if (w.fills.empty())
      {
        continue;
      }
      h = mix(h, 0xB00AU);
      h = mix(h, acct);
      for (const auto& [ts, q] : w.fills)
      {
        h = mix(h, static_cast<uint64_t>(ts.raw()));
        h = mix(h, static_cast<uint64_t>(q.raw()));
      }
    }

    // Firm-group STP table (feeds matching decisions, journaled as SetStpGroup).
    if (const auto& groups = matcher_.stpGroups(); !groups.empty())
    {
      for (uint64_t acct : sortedKeys(groups))
      {
        h = mix(h, 0xB00BU);
        h = mix(h, acct);
        h = mix(h, groups.at(acct));
      }
    }

    for (uint64_t acct : sortedKeys(clientOrderIds_))
    {
      h = mix(h, 0xB007U);
      h = mix(h, acct);
      const auto& seen = clientOrderIds_.at(acct);
      std::vector<uint64_t> ids(seen.begin(), seen.end());
      std::sort(ids.begin(), ids.end());
      for (uint64_t id : ids)
      {
        h = mix(h, id);
      }
    }

    if (ledger_ != nullptr)
    {
      std::vector<std::tuple<uint64_t, AssetId, Amount, Amount>> bals;
      ledger_->forEachBalanceSplit(
          [&](uint64_t acct, AssetId asset, Amount avail, Amount rsvd)
          {
            if (avail != 0 || rsvd != 0)  // a zeroed entry is indistinguishable from absence
            {
              bals.emplace_back(acct, asset, avail, rsvd);
            }
          });
      std::sort(bals.begin(), bals.end(),
                [](const auto& a, const auto& b)
                {
                  return std::get<0>(a) != std::get<0>(b) ? std::get<0>(a) < std::get<0>(b)
                                                          : std::get<1>(a) < std::get<1>(b);
                });
      for (const auto& [acct, asset, avail, rsvd] : bals)
      {
        h = mix(h, 0xB008U);
        h = mix(h, acct);
        h = mix(h, asset);
        h = mixAmount(h, avail);
        h = mixAmount(h, rsvd);
      }
    }
    return h;
  }

  // Digest of the CONSTRUCTOR configuration -- the structural parameters a
  // snapshot cannot restore and recovery cannot verify through stateHash
  // alone: fixed-point scales, assets, tick/lot/minQty, last-look window,
  // perp mode and the match policy. Written into SnapshotBegin and compared
  // on load: restoring raw fixed-point state into an engine constructed with
  // different scales (or a different policy) would silently reinterpret every
  // number, so a mismatch rejects the snapshot. Mutable knobs (bands, halted,
  // triggerRef, LULD, fat-finger, margins, position/order caps) are excluded:
  // they are carried by the snapshot itself or owned by the control plane.
  uint64_t configHash() const
  {
    uint64_t h = 0xcbf29ce484222325ULL;
    h = mix(h, 0xC0F1U);
    h = mix(h, cfg_.id);
    h = mix(h, static_cast<uint64_t>(cfg_.tickSize.raw()));
    h = mix(h, static_cast<uint64_t>(cfg_.lotSize.raw()));
    h = mix(h, static_cast<uint64_t>(cfg_.minQty.raw()));
    h = mix(h, cfg_.baseAsset);
    h = mix(h, cfg_.quoteAsset);
    h = mix(h, static_cast<uint64_t>(cfg_.lastLookWindowNs.count()));
    h = mix(h, cfg_.lastLookAcceptOnTimeout ? 1U : 0U);
    h = mix(h, cfg_.linearPerp ? 1U : 0U);
    h = mix(h, cfg_.autoDeleverage ? 1U : 0U);
    h = mix(h, static_cast<uint64_t>(cfg_.priceScale));
    h = mix(h, static_cast<uint64_t>(cfg_.qtyScale));
    h = mix(h, static_cast<uint64_t>(matcher_.policy()));
    return h;
  }

  // Serialize the engine into `out` as a journal-format snapshot: SnapshotBegin
  // (with the constructor-config hash), existing config records
  // (ListInstrument/SetBands/SetTriggerRef/SetStpGroup/AdminCmd), one
  // RestoreBalance per (account, asset) carrying the EXACT signed
  // available/reserved split, then the Restore* records in canonical order,
  // closed by SnapshotEnd. The canonical traversal (levels by price
  // best-first, FIFO within; everything else sorted by key) makes the file
  // byte-for-byte deterministic, and tail-appending RestoreOrder application
  // reproduces the exact book layout. Because balances restore exactly,
  // RestoreReservation / RestorePosition rebuild only the engine-side tables
  // on apply (no ledger re-reservation); the records still carry the exact
  // live amounts (history-dependent -- partial fills, held slices, STP; a
  // formula re-derivation is NOT faithful).
  void writeSnapshot(Journal& out) const
  {
    const int64_t ts = now_.raw();  // snapshot records carry raw sequencer ticks
    const uint64_t h = stateHash();
    out.append(InboundCommand{SnapshotBegin{kSnapshotFormatVersion, ts, h, configHash()}}, ts);

    out.append(InboundCommand{ListInstrument{cfg_.id, cfg_.tickSize, cfg_.lotSize, cfg_.minPrice,
                                             cfg_.maxPrice}},
               ts);
    out.append(InboundCommand{SetBands{cfg_.id, cfg_.minPrice, cfg_.maxPrice}}, ts);
    // Risk limits ride the config section for the same reason the bands do:
    // they decide what is admitted, so a recovered engine that lost them would
    // admit orders the live one refused.
    out.append(InboundCommand{riskLimits()}, ts);
    out.append(InboundCommand{SetTriggerRef{cfg_.id, cfg_.triggerRef}}, ts);
    // STP groups are engine state journaled as SetStpGroup commands; the
    // snapshot re-emits the live table as the same records (config section,
    // applied through the ordinary submit path on load).
    if (const auto& groups = matcher_.stpGroups(); !groups.empty())
    {
      for (uint64_t acct : sortedKeys(groups))
      {
        out.append(InboundCommand{SetStpGroup{cfg_.id, acct, groups.at(acct)}}, ts);
      }
    }
    // Admission profiles are engine state of the same kind: they decide what
    // is accepted, so they are re-emitted as the command that set them and
    // applied through the ordinary submit path on load.
    for (uint64_t acct : sortedKeys(admission_))
    {
      out.append(InboundCommand{SetAdmissionProfile{cfg_.id, acct, admission_.at(acct)}}, ts);
    }
    out.append(InboundCommand{AdminCmd{cfg_.id, cfg_.halted ? AdminAction::Halt
                                                            : AdminAction::Resume}},
               ts);
    if (auctionMode_)
    {
      out.append(InboundCommand{AdminCmd{cfg_.id, AdminAction::BeginPreOpen}}, ts);
    }
    // The session state rides the same existing AdminCmd path the halt and the
    // auction phase do. Written last of the three so it restores as the
    // outermost state, exactly as tradingStatus() ranks it.
    if (closed_)
    {
      out.append(InboundCommand{AdminCmd{cfg_.id, AdminAction::CloseSession}}, ts);
    }
    // Delisting is outermost of all, so it is written after the session state.
    // Only when set, so an engine that never delisted writes the file it always
    // did and its state hash is unchanged.
    if (delisted_)
    {
      out.append(InboundCommand{AdminCmd{cfg_.id, AdminAction::Delist}}, ts);
    }
    // Funding state: written only when there is any, so an engine with none
    // produces the same file it did before the record existed (and that file
    // still loads -- see RestoreFunding).
    if (fundingRateRaw_ != 0 || fundingIntervalNs_.count() != 0 || nextFundingNs_.raw() != 0)
    {
      out.append(InboundCommand{RestoreFunding{fundingRateRaw_, nextFundingNs_, fundingIntervalNs_}},
                 ts);
    }

    if (ledger_ != nullptr)
    {
      // Exact signed split per (account, asset): every live moment is
      // representable, including a negative wallet mid-liquidation and
      // non-positive totals (states the v1 Deposit-total encoding could not
      // express, forcing recovery a generation back). A fully zero entry is
      // skipped -- indistinguishable from absence, matching stateHash.
      std::vector<std::tuple<uint64_t, AssetId, Amount, Amount>> bals;
      ledger_->forEachBalanceSplit(
          [&](uint64_t acct, AssetId asset, Amount avail, Amount rsvd)
          {
            if (avail != 0 || rsvd != 0)
            {
              bals.emplace_back(acct, asset, avail, rsvd);
            }
          });
      std::sort(bals.begin(), bals.end(),
                [](const auto& a, const auto& b)
                {
                  return std::get<0>(a) != std::get<0>(b) ? std::get<0>(a) < std::get<0>(b)
                                                          : std::get<1>(a) < std::get<1>(b);
                });
      for (const auto& [acct, asset, avail, rsvd] : bals)
      {
        out.append(InboundCommand{RestoreBalance{acct, asset, avail, rsvd}}, ts);
      }
    }

    for (uint64_t acct : sortedKeys(mmpCfg_))
    {
      const MmpCfg& c = mmpCfg_.at(acct);
      out.append(InboundCommand{RestoreMmpCfg{acct, c.qtyLimit, c.windowNs}}, ts);
    }

    // MMP sliding-window fills, exact, in deque (time) order -- a maker one
    // fill from its limit stays one fill from it across recovery.
    for (uint64_t acct : sortedKeys(mmpFills_))
    {
      const MmpWindow& w = mmpFills_.at(acct);
      if (w.fills.empty())
      {
        continue;
      }
      RestoreMmpFills batch{};
      batch.account = acct;
      for (const auto& [fts, q] : w.fills)
      {
        batch.tsNs[batch.count] = fts.raw();  // wire batch: raw ticks
        batch.qtyRaw[batch.count] = q.raw();
        if (++batch.count == kMmpFillBatch)
        {
          out.append(InboundCommand{batch}, ts);
          batch = RestoreMmpFills{};
          batch.account = acct;
        }
      }
      if (batch.count > 0)
      {
        out.append(InboundCommand{batch}, ts);
      }
    }

    for (uint64_t acct : sortedKeys(clientOrderIds_))
    {
      const auto& seen = clientOrderIds_.at(acct);
      std::vector<uint64_t> ids(seen.begin(), seen.end());
      std::sort(ids.begin(), ids.end());
      RestoreClOrdIds batch{};
      batch.account = acct;
      for (uint64_t id : ids)
      {
        batch.ids[batch.count++] = id;
        if (batch.count == kClOrdIdBatch)
        {
          out.append(InboundCommand{batch}, ts);
          batch = RestoreClOrdIds{};
          batch.account = acct;
        }
      }
      if (batch.count > 0)
      {
        out.append(InboundCommand{batch}, ts);
      }
    }

    book_.forEachOrder(
        [&](const RestingOrder& o)
        {
          RestoreOrder r{o.id, o.accountId, o.price, o.leaves, o.side, o.hidden,
                         o.peak, o.lastLook, o.reduceOnly};
          r.expiryNs = expiryOf(o.id);
          r.ocoGroup = ocoOf(o.id);
          out.append(InboundCommand{r}, ts);
        });

    for (const auto& [o, trig] : sortedStops())
    {
      out.append(InboundCommand{RestoreStop{o, trig}}, ts);
    }

    for (OrderId id : sortedKeys(pegged_))
    {
      const Peg& p = pegged_.at(id);
      out.append(InboundCommand{RestorePeg{id, p.side, p.ref, p.offsetRaw}}, ts);
    }

    for (OrderId id : sortedKeys(orderStp_))
    {
      out.append(InboundCommand{RestoreOrderStp{id, static_cast<uint8_t>(orderStp_.at(id))}}, ts);
    }

    for (uint64_t acct : sortedKeys(positions_))
    {
      const Position& p = positions_.at(acct);
      out.append(InboundCommand{RestorePosition{acct, p.qtyRaw, p.entryRaw, p.margin}}, ts);
    }

    for (uint64_t hid : sortedKeys(held_))
    {
      const Held& x = held_.at(hid);
      RestoreHeld r{x.id, x.taker, x.takerAccount, x.takerSide, x.maker,
                    x.makerAccount, x.price, x.qty, x.deadline, x.takerTif,
                    x.takerType, x.takerPrice, x.takerExpiryNs, x.makerReduceOnly,
                    x.takerReduceOnly};
      r.makerTracked = orderAccount_.count(x.maker) != 0;
      r.refAtHoldRaw = x.refAtHoldRaw;
      out.append(InboundCommand{r}, ts);
    }

    for (OrderId id : sortedKeys(reserve_))
    {
      const Reservation& r = reserve_.at(id);
      out.append(
          InboundCommand{RestoreReservation{id, r.account, r.asset, r.side, r.limitPriceRaw,
                                            r.reservedRaw}},
          ts);
    }

    SnapshotEnd end{};
    end.stateHash = h;
    end.tradeSeq = tradeSeq_;
    end.heldSeq = heldSeq_;
    end.timeCounter = timeCounter_;
    end.nowNs = now_.raw();  // snapshot wire: raw
    end.mdEpoch = 0;         // the engine carries no MD epoch today
    end.lastPriceRaw = lastPrice_.raw();
    end.hasLast = hasLast_;
    end.markPriceRaw = markPrice_.raw();
    end.hasMark = hasMark_;
    end.haltUntilNs = haltUntil_.raw();
    out.append(InboundCommand{end}, ts);
  }

  // Recovery-only application of one snapshot record. Existing journaled
  // record types (config, deposits) route through the normal submit path;
  // Restore* records rebuild state directly, WITHOUT a matching pass -- the
  // snapshot book is uncrossed by invariant, and a crossing RestoreOrder
  // marks the snapshot corrupt. Returns false when the record proves the
  // snapshot corrupt (version mismatch, crossed book, un-backable
  // reservation, hash mismatch at SnapshotEnd); the caller then discards the
  // generation. NEVER wire this to live traffic: submit() drops snapshot tags
  // for exactly that reason.
  bool applySnapshotRecord(const InboundCommand& cmd, int64_t tsNs)
  {
    if (const auto* b = std::get_if<SnapshotBegin>(&cmd))
    {
      if (b->formatVersion != kSnapshotFormatVersion)
      {
        return false;
      }
      // Constructor-config guard: a snapshot written by an engine with other
      // structural parameters (scales, assets, policy, ...) must not restore
      // -- the raw fixed-point state would be silently reinterpreted.
      if (b->configHash != 0 && b->configHash != configHash())
      {
        std::fprintf(stderr,
                     "flox-venue: snapshot rejected for symbol %llu: constructor-config hash "
                     "mismatch (snapshot %016llx, engine %016llx) -- scales/assets/policy differ "
                     "from the writer's\n",
                     static_cast<unsigned long long>(cfg_.id),
                     static_cast<unsigned long long>(b->configHash),
                     static_cast<unsigned long long>(configHash()));
        return false;
      }
      return true;
    }
    if (const auto* r = std::get_if<RestoreOrder>(&cmd))
    {
      return applyRestoreOrder(*r);
    }
    if (const auto* r = std::get_if<RestoreStop>(&cmd))
    {
      return applyRestoreStop(*r);
    }
    if (const auto* r = std::get_if<RestoreOrderStp>(&cmd))
    {
      const auto mode = static_cast<STPMode>(r->mode);
      if (mode != STPMode::None)
      {
        orderStp_[r->id] = mode;
      }
      return true;
    }
    if (const auto* r = std::get_if<RestorePeg>(&cmd))
    {
      if (!book_.contains(r->id))
      {
        return false;  // a peg spec must reference a restored resting order
      }
      pegged_[r->id] = Peg{r->side, r->ref, r->offsetRaw};
      return true;
    }
    if (const auto* r = std::get_if<RestoreHeld>(&cmd))
    {
      return applyRestoreHeld(*r);
    }
    if (const auto* r = std::get_if<RestorePosition>(&cmd))
    {
      return applyRestorePosition(*r);
    }
    if (const auto* r = std::get_if<RestoreMmpCfg>(&cmd))
    {
      // Fill windows arrive separately as RestoreMmpFills records (exact
      // restore); a snapshot without them restores the windows empty.
      mmpCfg_[r->account] = MmpCfg{r->qtyLimit, r->windowNs};
      return true;
    }
    if (const auto* r = std::get_if<RestoreMmpFills>(&cmd))
    {
      if (r->count > kMmpFillBatch)
      {
        return false;
      }
      auto& w = mmpFills_[r->account];
      for (uint32_t i = 0; i < r->count; ++i)
      {
        w.fills.emplace_back(SeqNanos::fromRaw(r->tsNs[i]), Quantity::fromRaw(r->qtyRaw[i]));
        w.sumRaw += r->qtyRaw[i];
      }
      return true;
    }
    if (const auto* r = std::get_if<RestoreBalance>(&cmd))
    {
      if (ledger_ != nullptr)
      {
        ledger_->restore(r->account, r->asset, r->availableRaw, r->reservedRaw);
        // Balances are now exact: the RestoreReservation / RestorePosition
        // records that follow must NOT re-reserve (the reserved side is
        // already in place); they only rebuild the engine-side tables.
        exactBalanceRestore_ = true;
      }
      return true;
    }
    if (const auto* r = std::get_if<RestoreClOrdIds>(&cmd))
    {
      if (r->count > kClOrdIdBatch)
      {
        return false;
      }
      auto& seen = clientOrderIds_[r->account];
      for (uint32_t i = 0; i < r->count; ++i)
      {
        seen.insert(r->ids[i]);
      }
      return true;
    }
    if (const auto* r = std::get_if<RestoreFunding>(&cmd))
    {
      // Restored verbatim: the rate is a fact the venue published and the
      // calendar is a fact it will settle on. Neither is re-derived from
      // config, which is the whole point of the record.
      fundingRateRaw_ = r->fundingRateRaw;
      nextFundingNs_ = r->nextFundingNs.raw() > 0 ? r->nextFundingNs : SeqNanos{};
      fundingIntervalNs_ = r->fundingIntervalNs.count() > 0 ? r->fundingIntervalNs : DurationNs{};
      return true;
    }
    if (const auto* r = std::get_if<RestoreReservation>(&cmd))
    {
      return applyRestoreReservation(*r);
    }
    if (const auto* e = std::get_if<SnapshotEnd>(&cmd))
    {
      return applySnapshotEnd(*e);
    }
    submit(cmd, tsNs);  // existing record types apply through the live path
    return true;
  }

  // Live-traffic snapshot-tag drops (observability; see the guard in submit).
  uint64_t droppedSnapshotRecords() const noexcept { return droppedSnapshotRecords_; }

  // Trades that printed but could not be settled without creating value, so
  // nothing moved (see reportUnsettled). Must stay at zero: a non-zero value
  // means a fill reached clearing with neither a reservation nor the balance to
  // pay for it, and the venue refused to invent the difference.
  uint64_t unsettledTrades() const noexcept { return unsettledTrades_; }

  // Last-look accepts turned into rejects because the fill would have breached
  // a perp risk limit by the time the maker answered (see resolveHeld).
  uint64_t riskRejectedHolds() const noexcept { return riskRejectedHolds_; }

  // Per-maker last-look conduct.
  //
  // The reject RATE on its own says nothing: a maker with a wide tolerance and
  // a maker cherry-picking its fills can post the same number. What separates
  // them is which way the price had moved when they refused. A maker applying a
  // symmetric rule refuses roughly as often when the move favoured it as when
  // it did not; one taking the free option refuses almost only when it was
  // losing. That single split is what makes the behaviour visible without
  // anyone having to see the maker's code.
  struct LastLookStats
  {
    uint64_t held{0};
    uint64_t accepted{0};
    uint64_t rejected{0};
    uint64_t adverse{0};          // holds where the move went against the maker
    uint64_t rejectedAdverse{0};  // ... of which it refused
    uint64_t favourable{0};       // holds where the move went its way
    uint64_t rejectedFavourable{0};
  };

  const std::unordered_map<uint64_t, LastLookStats>& lastLookStats() const noexcept
  {
    return lastLookStats_;
  }

  // Holds refused by the venue's own tolerance rather than by the maker.
  uint64_t toleranceRejectedHolds() const noexcept { return toleranceRejectedHolds_; }

  // Pro-rata participants excluded by the fill-time risk limits.
  uint64_t skippedRiskProRata() const noexcept { return matcher_.skippedRiskProRata(); }

  // ---- asynchronous checkpoint support ----
  // A full engine clone taken under the consumer pause; serialization (fsync,
  // rename, rotation bookkeeping) then runs on a background thread against the
  // clone while matching continues. fork()-based copy-on-write snapshotting
  // was considered and REJECTED deliberately: the venue process is
  // multi-threaded (journal writer semantics aside -- gateway/ingress threads,
  // the idle sweeper, metrics/monitor threads), and fork() in a multi-threaded
  // process clones only the calling thread while every lock keeps the state
  // its holder left it in; a malloc arena lock held by another thread at fork
  // time deadlocks the child on its first allocation inside writeSnapshot.
  // An explicit deep copy is O(state) but safe, and the measured pause is the
  // clone alone, not serialize+fsync.
  struct SnapshotClone
  {
    std::unique_ptr<Ledger> ledger;  // owned deep copy (null: live engine had no ledger)
    std::unique_ptr<MatchingEngine> engine;
  };

  // `emptyBook` must be a PRISTINE book instance carrying only construction
  // config (ladder geometry); the resting orders are re-added canonically
  // (levels best-first, FIFO within -- forEachOrder order), which reproduces
  // the exact live layout the same way snapshot restore does. A default
  // MatchingBook needs no config, so the default argument suffices for it.
  SnapshotClone cloneForSnapshot(Book emptyBook = Book{}) const
  {
    SnapshotClone c;
    c.engine = std::make_unique<MatchingEngine>(cfg_, EventSink{[](const OutboundEvent&) {}},
                                                std::move(emptyBook), matcher_.policy());
    MatchingEngine& e = *c.engine;
    book_.forEachOrder([&](const RestingOrder& o)
                       { e.book_.addResting(o.side, o); });
    e.stops_ = stops_;
    e.lastPrice_ = lastPrice_;
    e.hasLast_ = hasLast_;
    e.markPrice_ = markPrice_;
    e.hasMark_ = hasMark_;
    e.tradeSeq_ = tradeSeq_;
    e.now_ = now_;
    e.timeCounter_ = timeCounter_;
    e.orderAccount_ = orderAccount_;
    e.byAccount_ = byAccount_;
    e.expiry_ = expiry_;
    e.orderOco_ = orderOco_;
    e.ocoMembers_ = ocoMembers_;
    e.ocoPending_ = ocoPending_;  // empty at a command boundary; copied for completeness
    e.pegged_ = pegged_;
    e.orderStp_ = orderStp_;
    e.admission_ = admission_;
    e.fees_ = fees_;
    e.feesEnabled_ = feesEnabled_;
    e.mmpCfg_ = mmpCfg_;
    e.mmpFills_ = mmpFills_;
    e.mmpBreached_ = mmpBreached_;
    e.held_ = held_;
    e.heldSeq_ = heldSeq_;
    e.heldOpen_.store(e.held_.size(), std::memory_order_relaxed);
    e.clientOrderIds_ = clientOrderIds_;
    e.reserve_ = reserve_;
    e.positions_ = positions_;
    e.auctionMode_ = auctionMode_;
    e.haltUntil_ = haltUntil_;
    e.closed_ = closed_;
    e.lastStatus_ = lastStatus_;
    e.lastStatusUntil_ = lastStatusUntil_;
    e.statusPublished_ = statusPublished_;
    e.fundingRateRaw_ = fundingRateRaw_;
    e.fundingIntervalNs_ = fundingIntervalNs_;
    e.nextFundingNs_ = nextFundingNs_;
    for (const auto& [acct, grp] : matcher_.stpGroups())
    {
      e.matcher_.setStpGroup(acct, grp);
    }
    if (ledger_ != nullptr)
    {
      c.ledger = std::make_unique<Ledger>(*ledger_);
      e.setLedger(c.ledger.get(), venueAccount_);
    }
    else
    {
      e.venueAccount_ = venueAccount_;
    }
    return c;
  }

  Ledger* ledger() const noexcept { return ledger_; }
  uint64_t venueAccount() const noexcept { return venueAccount_; }

 private:
  // Instrument conformance for a conditional order: the trigger is a price and
  // the size is a size, so tick, band and lot apply exactly as they do to a
  // resting limit. State and duplicate checks already ran before this point.
  RejectReason validateConditional(const NewOrder& o) const
  {
    if (o.quantity.raw() <= 0)
    {
      return RejectReason::InvalidQuantity;
    }
    if (!cfg_.lotSize.isZero() && (o.quantity.raw() % cfg_.lotSize.raw()) != 0)
    {
      return RejectReason::InvalidQuantity;
    }
    const int64_t trig = o.triggerPrice.raw();
    if (trig > 0)
    {
      if (!cfg_.tickSize.isZero() && (trig % cfg_.tickSize.raw()) != 0)
      {
        return RejectReason::InvalidPrice;
      }
      if (!cfg_.minPrice.isZero() && trig < cfg_.minPrice.raw())
      {
        return RejectReason::InvalidPrice;
      }
      if (!cfg_.maxPrice.isZero() && trig > cfg_.maxPrice.raw())
      {
        return RejectReason::InvalidPrice;
      }
    }
    // A stop-LIMIT also carries the limit price it will rest at.
    if (o.type == OrderType::STOP_LIMIT || o.type == OrderType::TAKE_PROFIT_LIMIT)
    {
      if (!cfg_.tickSize.isZero() && (o.price.raw() % cfg_.tickSize.raw()) != 0)
      {
        return RejectReason::InvalidPrice;
      }
      if (!cfg_.minPrice.isZero() && o.price.raw() < cfg_.minPrice.raw())
      {
        return RejectReason::InvalidPrice;
      }
      if (!cfg_.maxPrice.isZero() && o.price.raw() > cfg_.maxPrice.raw())
      {
        return RejectReason::InvalidPrice;
      }
    }
    return RejectReason::None;
  }

  RejectReason validate(const NewOrder& o) const
  {
    if (o.symbol != cfg_.id)
    {
      return RejectReason::UnknownSymbol;
    }
    // Outermost first: a client whose order is refused deserves the reason that
    // tells it what to do next. Delisted means do not come back; closed means
    // next session; halted means something is wrong with the instrument now.
    if (delisted_)
    {
      return RejectReason::InstrumentDelisted;
    }
    if (closed_)
    {
      return RejectReason::MarketClosed;
    }
    if (cfg_.halted)
    {
      return RejectReason::Halted;
    }
    if (o.quantity.raw() <= 0)
    {
      return RejectReason::InvalidQuantity;
    }
    // Pro-rata allocation does not honour last look (a held slice cannot be
    // carved out of a proportional round), so the combination is refused at
    // admission instead of silently filling the maker as firm.
    if (o.lastLook && matcher_.policy() == MatchPolicy::ProRata)
    {
      return RejectReason::LastLookUnsupported;
    }
    if (!cfg_.minQty.isZero() && o.quantity < cfg_.minQty)
    {
      return RejectReason::InvalidQuantity;
    }
    if (!cfg_.lotSize.isZero() && (o.quantity.raw() % cfg_.lotSize.raw()) != 0)
    {
      return RejectReason::LotSizeViolation;
    }
    if (!cfg_.maxOrderQty.isZero() && cfg_.maxOrderQty < o.quantity)
    {
      return RejectReason::OrderTooLarge;  // fat-finger size
    }
    if (o.type == OrderType::LIMIT && !cfg_.maxOrderNotional.isZero() &&
        cfg_.maxOrderNotional < (o.quantity * o.price))
    {
      return RejectReason::OrderTooLarge;  // fat-finger notional
    }
    if (o.type == OrderType::LIMIT)
    {
      if (o.price.raw() <= 0)
      {
        return RejectReason::InvalidPrice;
      }
      if (!cfg_.tickSize.isZero() && (o.price.raw() % cfg_.tickSize.raw()) != 0)
      {
        return RejectReason::TickSizeViolation;
      }
      if (!cfg_.minPrice.isZero() && o.price < cfg_.minPrice)
      {
        return RejectReason::InvalidPrice;
      }
      if (!cfg_.maxPrice.isZero() && cfg_.maxPrice < o.price)
      {
        return RejectReason::InvalidPrice;
      }
    }
    if (book_.contains(o.id) || stops_.contains(o.id))
    {
      return RejectReason::DuplicateOrderId;
    }
    // An id referenced by an active last-look hold is still live even when its
    // order is (fully held) out of the book: a reject will restore quantity
    // under that id, so a reused id would merge two unrelated orders.
    if (!held_.empty())
    {
      for (const auto& [hid, h] : held_)
      {
        (void)hid;
        if (h.maker == o.id || h.taker == o.id)
        {
          return RejectReason::DuplicateOrderId;
        }
      }
    }
    return RejectReason::None;
  }

  // Per-order perp risk gates shared by onNew and the trigger path (a triggered
  // stop re-enters matching directly, NOT through onNew, so it must run these too
  // -- otherwise a reduce-only stop opens an uncollateralized position and a
  // plain stop bypasses the position cap). Caps a reduce-only order to the
  // opposing position (0 reducible -> reject: nothing to reduce) and rejects a
  // fill that would breach maxPositionQty. Mutates o.quantity for the cap.
  // Entitlement gate. Every admission path consults this before anything else
  // decides the order's fate, so a counterparty cannot reach the book through
  // a path that forgot to ask. Declared in scripts/check_gate_reachability.py,
  // which fails the build if a path stops calling it.
  RejectReason admissionGate(const NewOrder& o) const
  {
    auto it = admission_.find(o.accountId);
    if (it == admission_.end())
    {
      return RejectReason::None;  // no profile: everything permitted
    }
    const AdmissionProfile& p = it->second;
    if (p.allowedTypes != 0 && (p.allowedTypes & (1u << static_cast<uint32_t>(o.type))) == 0)
    {
      return RejectReason::OrderTypeNotPermitted;
    }
    // Tested before the TIF list so the answer names the policy rather than the
    // field: "you may not leave an order resting" tells the counterparty what
    // to change, "this time in force is not allowed" leaves it guessing which
    // of the allowed ones is safe.
    //
    // Refused on admission, not killed on the way out: an order that could rest
    // must never be accepted from a counterparty that does not track resting
    // orders. POST_ONLY exists only to rest; GTC and GTD outlive the message
    // that carried them.
    //
    // A call auction rests EVERYTHING it admits -- there is no matching to be
    // immediate about, so an IOC accumulates like any other order and sits in
    // the book until the uncross. A counterparty that does not track resting
    // orders therefore cannot participate in one at all: its order would sit
    // there for the length of the auction while it believes the order either
    // filled or died on arrival, and at the uncross it can trade against that
    // counterparty's own other side.
    if ((p.deny & AdmissionDeny::DenyResting) != 0 &&
        (auctionMode_ || o.tif == TimeInForce::GTC || o.tif == TimeInForce::GTD ||
         o.tif == TimeInForce::POST_ONLY || o.postOnly))
    {
      return RejectReason::RestingNotPermitted;
    }
    if (p.allowedTif != 0 && (p.allowedTif & (1u << static_cast<uint32_t>(o.tif))) == 0)
    {
      return RejectReason::TimeInForceNotPermitted;
    }
    return RejectReason::None;
  }

  bool admissionDenies(uint64_t account, uint8_t bit) const
  {
    auto it = admission_.find(account);
    return it != admission_.end() && (it->second.deny & bit) != 0;
  }

  RejectReason perpRiskGate(NewOrder& o)
  {
    if (!cfg_.linearPerp)
    {
      return RejectReason::None;
    }
    if (o.reduceOnly)
    {
      const auto pit = positions_.find(o.accountId);
      const int64_t posQ = (pit == positions_.end()) ? 0 : pit->second.qtyRaw;
      const int64_t reducible = (o.side == Side::BUY && posQ < 0)    ? -posQ
                                : (o.side == Side::SELL && posQ > 0) ? posQ
                                                                     : 0;
      if (o.quantity.raw() > reducible)
      {
        o.quantity = Quantity::fromRaw(reducible);
      }
      if (o.quantity.raw() <= 0)
      {
        return RejectReason::InvalidQuantity;  // nothing to reduce -> reduce-only never opens
      }
    }
    if (!cfg_.maxPositionQty.isZero())
    {
      const auto pit = positions_.find(o.accountId);
      const int64_t posQ = (pit == positions_.end()) ? 0 : pit->second.qtyRaw;
      const int64_t worst = posQ + (o.side == Side::BUY ? o.quantity.raw() : -o.quantity.raw());
      if (iabs64(worst) > cfg_.maxPositionQty.raw())
      {
        return RejectReason::PositionLimitExceeded;
      }
    }
    return RejectReason::None;
  }

  // How much of one leg's prospective fill the perp risk limits still allow,
  // measured against the position the account holds RIGHT NOW. `reason` names
  // the limit that cut it (meaningful only when the result is below `want`).
  int64_t legFillLimit(uint64_t account, Side side, bool reduceOnly, int64_t want,
                       CancelReason& reason) const
  {
    const auto pit = positions_.find(account);
    const int64_t posQ = (pit == positions_.end()) ? 0 : pit->second.qtyRaw;
    int64_t allowed = want;
    if (reduceOnly)
    {
      // A reduce-only order may only close what is open on the other side. Its
      // reserved IM is 0 by construction, so any part of it that opened a
      // position would open it with NO margin at all.
      const int64_t reducible = (side == Side::BUY && posQ < 0)    ? -posQ
                                : (side == Side::SELL && posQ > 0) ? posQ
                                                                   : 0;
      if (reducible < allowed)
      {
        allowed = reducible;
        reason = CancelReason::ReduceOnlyNotReducing;
      }
    }
    if (!cfg_.maxPositionQty.isZero())
    {
      // Room left before the RESULTING position breaches the cap. Checking the
      // incoming order alone (the admission gate) lets several orders, each
      // under the cap, settle into a position past it.
      const int64_t room =
          (side == Side::BUY) ? cfg_.maxPositionQty.raw() - posQ : cfg_.maxPositionQty.raw() + posQ;
      if (room < allowed)
      {
        allowed = room;
        reason = CancelReason::PositionLimitExceeded;
      }
    }
    return allowed < 0 ? 0 : allowed;
  }

  // Fill-time risk re-check for one prospective bite (see the FillLimit hook).
  // reduceOnly and maxPositionQty are gated at submit / stop-trigger / modify,
  // but a RESTING order fills later, against a position that has since moved:
  // a reduce-only order whose position shrank would open or flip it (with zero
  // margin -- reduce-only reserves none), and orders that each passed the cap
  // individually would settle past it together. Both legs are therefore
  // re-measured here, on live engine state, so a replay reproduces it.
  FillLimit fillLimit(const RestingOrder& maker, const NewOrder& taker, Quantity want) const
  {
    if (ledger_ == nullptr)
    {
      // No ledger: the engine keeps no positions to measure against.
      return FillLimit{want, want, want, false, false, CancelReason::ReduceOnlyNotReducing};
    }
    return pairFillLimit(maker.accountId, maker.side, maker.reduceOnly, taker.accountId, taker.side,
                         taker.reduceOnly, want);
  }

  // fillLimit over two legs described directly -- used by the auction uncross,
  // where BOTH legs are resting orders and neither is an incoming NewOrder.
  FillLimit pairFillLimit(uint64_t makerAcct, Side makerSide, bool makerReduceOnly,
                          uint64_t takerAcct, Side takerSide, bool takerReduceOnly,
                          Quantity want) const
  {
    FillLimit out;
    out.qty = want;
    out.makerQty = want;
    out.takerQty = want;
    CancelReason makerReason = CancelReason::ReduceOnlyNotReducing;
    CancelReason takerReason = CancelReason::ReduceOnlyNotReducing;
    const int64_t makerAllowed =
        legFillLimit(makerAcct, makerSide, makerReduceOnly, want.raw(), makerReason);
    const int64_t takerAllowed =
        legFillLimit(takerAcct, takerSide, takerReduceOnly, want.raw(), takerReason);
    out.makerQty = Quantity::fromRaw(makerAllowed);
    out.takerQty = Quantity::fromRaw(takerAllowed);
    // The maker is the leg reported as blocked when both are: it is the one the
    // matcher can act on (pull it from the book) without killing an aggressor
    // that may still trade elsewhere.
    if (makerAllowed <= takerAllowed)
    {
      out.qty = out.makerQty;
      out.makerBlocked = makerAllowed <= 0;
      out.reason = makerReason;
    }
    else
    {
      out.qty = out.takerQty;
      out.takerBlocked = takerAllowed <= 0;
      out.reason = takerReason;
    }
    return out;
  }

  void onNew(NewOrder o)
  {
    // Entitlement first: an order the counterparty may not send should not
    // consume a clientOrderId, link an OCO group or reach any later gate.
    if (const RejectReason r = admissionGate(o); r != RejectReason::None)
    {
      ++admissionRejects_;
      sink_(OrderRejected{o.id, o.symbol, r, o.accountId});
      return;
    }
    // clientOrderId dedup (per account, window = engine session/uptime; see
    // docs/venue/matching.md). Registered on receipt, BEFORE any other gate:
    // a resend of an already-seen clOrdId must reject deterministically even
    // when the first instance has long filled or canceled -- that is the whole
    // point (double execution after an ambiguous disconnect). clientOrderId 0
    // means "not set" and is never deduplicated. Replay-safe: the index is
    // rebuilt by the same submits during journal replay.
    if (o.clientOrderId != 0)
    {
      auto& seen = clientOrderIds_[o.accountId];
      if (!seen.insert(o.clientOrderId).second)
      {
        sink_(OrderRejected{o.id, o.symbol, RejectReason::DuplicateClientOrderId, o.accountId});
        return;
      }
    }
    if (o.ocoGroup > 0)
    {
      orderOco_[o.id] = o.ocoGroup;  // link before matching so a taker fill triggers OCO too
      ocoMembers_[o.ocoGroup].push_back(o.id);
    }
    // Any early exit before the order commits (parks as a stop, or passes every
    // gate and reaches matching/resting) must unlink it from its OCO group --
    // otherwise a rejected leg lingers in ocoMembers_ and later cancels a reused
    // id. `committed` is set once the order is live; the guard cleans up the rest.
    bool committed = false;
    struct OcoCleanup
    {
      MatchingEngine* self;
      OrderId id;
      bool linked;
      const bool* committed;
      ~OcoCleanup()
      {
        if (linked && !*committed)
        {
          self->unlinkOco(id);
        }
      }
    } ocoCleanup{this, o.id, o.ocoGroup > 0, &committed};
    if (o.peg != PegRef::None)
    {
      o.type = OrderType::LIMIT;  // a peg is a passive limit priced off the book
      o.price = Price::fromRaw(pegTargetRaw(o.side, o.peg, o.pegOffsetRaw));
    }
    if (isConditional(o.type))
    {
      // Conditional orders branch off before validate(), which is written for
      // an order carrying a live limit price. They still have a price (the
      // trigger) and a quantity, and both must obey the instrument -- an
      // off-tick or out-of-band trigger, or a sub-lot size, used to be parked
      // happily and only surfaced when the stop fired.
      if (const RejectReason r = validateConditional(o); r != RejectReason::None)
      {
        sink_(OrderRejected{o.id, o.symbol, r, o.accountId});
        return;
      }
      committed = onStop(o);  // parked in the stop book -> committed (keep OCO link)
      return;
    }
    if (cfg_.maxOpenOrders > 0)
    {
      // Ingress DoS / risk gate: cap live resting orders per account. Once at
      // the cap the account must cancel before adding more.
      auto it = byAccount_.find(o.accountId);
      if (it != byAccount_.end() && it->second.size() >= cfg_.maxOpenOrders)
      {
        sink_(OrderRejected{o.id, o.symbol, RejectReason::TooManyOpenOrders, o.accountId});
        return;
      }
    }
    // Perp risk gate (reduce-only cap + position-limit check). The single
    // source of truth shared with the triggered-stop and modify paths -- see
    // perpRiskGate. Runs BEFORE validate so the fat-finger size check sees the
    // reduce-only-capped quantity, matching the pre-refactor behaviour. Spot:
    // a no-op.
    if (const RejectReason r = perpRiskGate(o); r != RejectReason::None)
    {
      sink_(OrderRejected{o.id, o.symbol, r, o.accountId});
      return;
    }
    if (const RejectReason r = validate(o); r != RejectReason::None)
    {
      sink_(OrderRejected{o.id, o.symbol, r, o.accountId});
      return;
    }
    if (!reserveFunds(o))
    {
      sink_(OrderRejected{o.id, o.symbol, creditReason_, o.accountId});
      creditReason_ = RejectReason::InsufficientFunds;  // reset for the next order
      return;                                           // pre-trade buying-power (ledger reservation or credit hook)
    }
    committed = true;  // past all reject gates: the order will match/rest, and any
                       // OCO resolution is now owned by processOco / forgetOrder.

    if (auctionMode_)
    {
      // Pre-open / auction: accumulate without matching (a crossed book is
      // allowed). Market orders rest at the band edge so they always execute in
      // the uncross.
      Price restPx = o.price;
      if (o.type == OrderType::MARKET)
      {
        restPx = (o.side == Side::BUY) ? cfg_.maxPrice : cfg_.minPrice;
      }
      RestingOrder ro{o.id, o.accountId, restPx, o.quantity, o.side};
      ro.reduceOnly = o.reduceOnly;
      book_.addResting(o.side, ro);
      trackResting(o.id, o.accountId, o.stp);
      sink_(OrderAccepted{o.id, o.symbol, o.side, restPx, o.quantity, true, Quantity{}, o.accountId});
      return;
    }

    // LULD band, captured from the pre-trade reference (matching below moves the
    // last price). Absent before the first trade -- documented pre-first-trade
    // window where no band exists yet.
    int64_t luldLo = 0, luldHi = 0;
    const bool luldOn = luldBand(luldLo, luldHi);

    if (luldOn && o.type != OrderType::MARKET)
    {
      // A limit order priced outside the band is rejected pre-trade and trips the
      // pause -- it never executes or rests out of band.
      if ((o.side == Side::BUY && luldHi < o.price.raw()) ||
          (o.side == Side::SELL && o.price.raw() < luldLo))
      {
        releaseReservation(o.id);
        sink_(OrderRejected{o.id, o.symbol, RejectReason::LuldBreach, o.accountId});
        tripLuldHalt();
        return;
      }
    }

    const MatchOutcome out =
        matcher_.cross(o, book_, [this]()
                       { return ++tradeSeq_; }, emit_);
    stampFreshHolds();

    if (out.reject != RejectReason::None)
    {
      releaseReservation(o.id);  // post-only-would-cross / FOK-unfulfillable: free the reserve
      sink_(OrderRejected{o.id, o.symbol, out.reject, o.accountId});
      return;
    }
    if (out.residualRests)
    {
      RestingOrder ro{o.id, o.accountId, o.price, out.leaves, o.side};
      ro.lastLook = o.lastLook && cfg_.lastLookWindowNs.count() > 0;  // window 0 = feature off
      ro.reduceOnly = o.reduceOnly;                                   // carried so a later modify preserves it
      if (o.visibleQuantity.raw() > 0 && o.visibleQuantity < out.leaves)
      {
        ro.peak = o.visibleQuantity;    // iceberg: show a peak, hide the rest
        ro.leaves = o.visibleQuantity;  // displayed
        ro.hidden = out.leaves - o.visibleQuantity;
      }
      book_.addResting(o.side, ro);
      trackResting(o.id, o.accountId, o.stp);
      if (o.tif == TimeInForce::GTD && static_cast<bool>(o.expiryNs))
      {
        expiry_[o.id] = o.expiryNs;
      }
      if (o.peg != PegRef::None)
      {
        pegged_[o.id] = {o.side, o.peg, o.pegOffsetRaw};
      }
      // Public feed shows only the displayed peak (ro.leaves); the hidden iceberg
      // reserve (out.leaves - ro.leaves) is not leaked. Non-iceberg: they match.
      sink_(OrderAccepted{o.id, o.symbol, o.side, o.price, out.leaves, true, ro.leaves, o.accountId});
    }
    else if (out.residualCanceled)
    {
      // The unfilled residual (IOC/FOK/MARKET/STP-newest) never rests, so its
      // buying-power reservation must be released here -- this raw-sink path does
      // not run the emit_ wrapper's release-on-cancel. Held slices stay
      // reserved: their accept still has to settle (reject releases later).
      releaseReservationExceptHeld(o.id);
      sink_(OrderCanceled{o.id, o.symbol, out.residualCancelReason, o.accountId});
    }

    processTriggers();  // this order's trades may have crossed resting stops

    // Post-trade LULD: a MARKET order (no limit to gate it pre-trade) can sweep
    // the book and print outside the band, and a stop cascade in processTriggers
    // can too. The breaching print stands, but it trips the volatility pause so
    // subsequent trading halts -- the exchange-style volatility interruption. A
    // limit order cannot breach here: it never trades through its own in-band
    // limit, so this condition is naturally false for it.
    if (luldOn && hasLast_ && (lastPrice_.raw() > luldHi || lastPrice_.raw() < luldLo))
    {
      tripLuldHalt();
    }
  }

  // LULD band [loRaw, hiRaw] around the current last price, computed in 128-bit
  // to avoid int64 overflow on a high-priced instrument and clamped so the upper
  // bound cannot overflow. Returns false when LULD is off or there is no last
  // price yet (no reference -> no band).
  bool luldBand(int64_t& loRaw, int64_t& hiRaw) const
  {
    if (cfg_.luldBps <= 0 || !hasLast_)
    {
      return false;
    }
    const __int128 prod =
        static_cast<__int128>(lastPrice_.raw()) * static_cast<__int128>(cfg_.luldBps) / 10000;
    const int64_t maxBand = INT64_MAX - lastPrice_.raw();
    const int64_t band = prod > static_cast<__int128>(maxBand) ? maxBand : static_cast<int64_t>(prod);
    loRaw = lastPrice_.raw() - band;
    hiRaw = lastPrice_.raw() + band;
    return true;
  }

  void tripLuldHalt()
  {
    cfg_.halted = true;  // trip a timed volatility pause
    haltUntil_ = now_ + cfg_.luldHaltNs;
    publishStatus(TradingStatusReason::LuldBreach);
  }

  // ---- instrument-wide publications ----
  // Emit the state the engine is now in, if it differs from the last one
  // published. Every halt / pause / auction transition routes through here, so
  // the feed carries transitions and only transitions: no duplicate on a
  // re-halt of an already halted symbol, and nothing to infer downstream.
  void publishStatus(TradingStatusReason reason)
  {
    const TradingStatus s = tradingStatus();
    // The deadline belongs to the timed pause and to nothing else. A state that
    // is not the pause publishes 0 even when a pause deadline is still stored
    // underneath it (a closed session or an auction phase over a paused
    // instrument) -- a subscriber must never be handed an expiry for a state
    // that does not expire.
    emitStatus(s, reason, s == TradingStatus::LuldPause ? haltUntil_.raw() : 0);
  }

  void emitStatus(TradingStatus status, TradingStatusReason reason, int64_t untilNs)
  {
    if (statusPublished_ && status == lastStatus_ && untilNs == lastStatusUntil_)
    {
      return;
    }
    statusPublished_ = true;
    lastStatus_ = status;
    lastStatusUntil_ = untilNs;
    sink_(TradingStatusChanged{cfg_.id, status, reason, untilNs});
  }

  // A settlement just happened, so the calendar moves on: one whole interval
  // past the boundary that was settled, and further whole intervals if the
  // settlement ran late enough that one step would still leave the boundary in
  // the past (an operator catching up after an outage must not leave a stale
  // "next funding" in the feed). Only an operator-set schedule moves -- with no
  // schedule the published value is derived from `now` and moves by itself.
  void advanceFundingSchedule()
  {
    if (fundingIntervalNs_.count() <= 0 || nextFundingNs_.raw() <= 0)
    {
      return;
    }
    nextFundingNs_ += fundingIntervalNs_;
    if (nextFundingNs_ <= now_)
    {
      const DurationNs behind = now_ - nextFundingNs_;
      nextFundingNs_ += DurationNs{(behind.count() / fundingIntervalNs_.count() + 1) *
                                   fundingIntervalNs_.count()};
    }
  }

  // Emit the derivatives layer the engine knows: the mark it was just given,
  // the last funding rate it applied, the next funding boundary of the
  // configured schedule and the live open interest.
  void publishDerivatives(Price mark)
  {
    sink_(DerivativesUpdated{cfg_.id, mark, fundingRateRaw_, nextFundingNs(), openInterest()});
  }

  // Conditional order (stop / take-profit / trailing): park in the stop book
  // until the last-trade price crosses the trigger.
  // Park a conditional (stop / take-profit / trailing) order. Returns true if it
  // was accepted into the stop book, false if rejected -- the caller uses this to
  // decide whether to keep or unlink an OCO group membership.
  bool onStop(const NewOrder& o)
  {
    if (o.symbol != cfg_.id)
    {
      sink_(OrderRejected{o.id, o.symbol, RejectReason::UnknownSymbol, o.accountId});
      return false;
    }
    if (delisted_)
    {
      sink_(OrderRejected{o.id, o.symbol, RejectReason::InstrumentDelisted, o.accountId});
      return false;
    }
    if (closed_)
    {
      sink_(OrderRejected{o.id, o.symbol, RejectReason::MarketClosed, o.accountId});
      return false;
    }
    if (cfg_.halted)
    {
      sink_(OrderRejected{o.id, o.symbol, RejectReason::Halted, o.accountId});
      return false;
    }
    if (o.quantity.raw() <= 0)
    {
      sink_(OrderRejected{o.id, o.symbol, RejectReason::InvalidQuantity, o.accountId});
      return false;
    }
    const bool trailing = (o.type == OrderType::TRAILING_STOP);
    if (!trailing && o.triggerPrice.raw() <= 0)
    {
      sink_(OrderRejected{o.id, o.symbol, RejectReason::InvalidPrice, o.accountId});
      return false;
    }
    if (trailing && o.trailingOffset.raw() <= 0)
    {
      sink_(OrderRejected{o.id, o.symbol, RejectReason::InvalidPrice, o.accountId});
      return false;
    }
    if (isLimitStop(o.type) && o.price.raw() <= 0)
    {
      sink_(OrderRejected{o.id, o.symbol, RejectReason::InvalidPrice, o.accountId});
      return false;
    }
    if (book_.contains(o.id) || stops_.contains(o.id))
    {
      sink_(OrderRejected{o.id, o.symbol, RejectReason::DuplicateOrderId, o.accountId});
      return false;
    }

    Price initTrig{};
    if (trailing)
    {
      if (hasLast_)
      {
        initTrig = (o.side == Side::SELL)
                       ? Price::fromRaw(lastPrice_.raw() - o.trailingOffset.raw())
                       : Price::fromRaw(lastPrice_.raw() + o.trailingOffset.raw());
      }
    }
    else
    {
      initTrig = o.triggerPrice;
    }
    stops_.add(o, initTrig, trailing);
    // GTD binds a conditional order as much as a resting one: a stop whose
    // deadline passes before it ever triggers has to expire, not wait forever
    // for a price that may never come.
    if (o.tif == TimeInForce::GTD && static_cast<bool>(o.expiryNs))
    {
      expiry_[o.id] = o.expiryNs;
    }
    sink_(OrderAccepted{o.id, o.symbol, o.side, o.triggerPrice, o.quantity, false, Quantity{},
                        o.accountId});  // pending, not on book
    processTriggers();                  // may already be in-the-money
    return true;
  }

  // Fire every stop the current last price crosses, cascading: each fired stop
  // trades, which can move the last price and trigger further stops.
  std::optional<Price> triggerReference() const
  {
    if (cfg_.triggerRef == TriggerRef::Mark)
    {
      return hasMark_ ? std::optional<Price>{markPrice_} : std::nullopt;
    }
    return hasLast_ ? std::optional<Price>{lastPrice_} : std::nullopt;
  }

  void processTriggers()
  {
    // A stop re-enters matching here rather than through onNew, so the
    // instrument-state check that guards new orders has to be repeated -- it
    // is not inherited. Without this a mark update fires stops straight
    // through a halt, a LULD pause, a closed session or a pre-open auction:
    // the trigger reference keeps moving even when trading does not.
    if (tradingStatus() != TradingStatus::Trading)
    {
      return;
    }
    auto ref = triggerReference();
    if (!ref)
    {
      return;
    }
    stops_.updateTrailing(*ref);
    while (auto agg = stops_.popTriggered(*ref))
    {
      sink_(OrderTriggered{agg->id, cfg_.id, *ref, agg->accountId});
      // Perp risk gates: a triggered stop re-enters matching HERE, not through
      // onNew, so it must run the same reduce-only cap + position-limit checks --
      // else a reduce-only stop opens an uncollateralized (im=0) position and a
      // plain stop bypasses maxPositionQty. Reject (and free nothing -- not yet
      // reserved) if the reduce-only cap leaves nothing or the cap is breached.
      if (const RejectReason r = perpRiskGate(*agg); r != RejectReason::None)
      {
        sink_(OrderRejected{agg->id, cfg_.id, r, agg->accountId});
        ref = triggerReference();
        if (!ref)
        {
          break;
        }
        stops_.updateTrailing(*ref);
        continue;
      }
      // Reserve buying power now that the stop is a live aggressor -- else an
      // underfunded triggered stop would settle via unchecked debit and create
      // value (buyer receives base it can't pay for).
      if (!reserveFunds(*agg))
      {
        sink_(OrderRejected{agg->id, cfg_.id, RejectReason::InsufficientFunds, agg->accountId});
        ref = triggerReference();
        if (!ref)
        {
          break;
        }
        stops_.updateTrailing(*ref);
        continue;
      }
      const MatchOutcome out =
          matcher_.cross(*agg, book_, [this]()
                         { return ++tradeSeq_; }, emit_);
      stampFreshHolds();
      if (out.reject != RejectReason::None)
      {
        releaseReservation(agg->id);
        sink_(OrderRejected{agg->id, cfg_.id, out.reject, agg->accountId});
      }
      else if (out.residualRests)
      {
        RestingOrder rro{agg->id, agg->accountId, agg->price, out.leaves, agg->side};
        rro.reduceOnly = agg->reduceOnly;
        book_.addResting(agg->side, rro);
        trackResting(agg->id, agg->accountId, agg->stp);
        sink_(OrderAccepted{agg->id, cfg_.id, agg->side, agg->price, out.leaves, true, Quantity{},
                            agg->accountId});
      }
      else if (out.residualCanceled)
      {
        releaseReservationExceptHeld(agg->id);  // held slices stay reserved
        sink_(OrderCanceled{agg->id, cfg_.id, out.residualCancelReason, agg->accountId});
      }
      ref = triggerReference();  // trades may have moved the last-price reference
      if (!ref)
      {
        break;
      }
      stops_.updateTrailing(*ref);
    }
  }

  void onModify(const ModifyOrder& m)
  {
    if (m.symbol != cfg_.id)
    {
      sink_(CancelRejected{m.id, m.symbol, RejectReason::UnknownSymbol, m.accountId, true});
      return;
    }
    if (admissionDenies(m.accountId, AdmissionDeny::DenyAmend))
    {
      ++admissionRejects_;
      sink_(CancelRejected{m.id, m.symbol, RejectReason::AmendNotPermitted, m.accountId, true});
      return;
    }
    // Resolve (reject) any last-look holds referencing this order FIRST: both
    // modify paths re-shape the order and its reservation, and a hold left
    // behind would later settle against a reservation that no longer covers it.
    rejectHoldsFor(m.id);
    const RestingOrder* cur = book_.find(m.id);
    if (cur == nullptr)
    {
      sink_(CancelRejected{m.id, m.symbol, RejectReason::UnknownOrder, m.accountId, true});
      return;
    }
    if (m.newQty.raw() <= 0)
    {
      sink_(CancelRejected{m.id, m.symbol, RejectReason::InvalidQuantity, m.accountId, true});
      return;
    }

    const Side side = cur->side;
    const uint64_t acct = cur->accountId;
    const Quantity curLeaves = cur->leaves;
    const Quantity curHidden = cur->hidden;
    const bool curReduceOnly = cur->reduceOnly;
    const Price curPrice = cur->price;
    const Price newPrice = (m.newPrice.raw() == 0) ? curPrice : m.newPrice;

    // Validate the new price/qty before mutating so a bad modify leaves the
    // original order untouched.
    if (newPrice.raw() <= 0)
    {
      sink_(CancelRejected{m.id, m.symbol, RejectReason::InvalidPrice, acct, true});
      return;
    }
    if (!cfg_.tickSize.isZero() && (newPrice.raw() % cfg_.tickSize.raw()) != 0)
    {
      sink_(CancelRejected{m.id, m.symbol, RejectReason::TickSizeViolation, acct, true});
      return;
    }
    if (!cfg_.minPrice.isZero() && newPrice < cfg_.minPrice)
    {
      sink_(CancelRejected{m.id, m.symbol, RejectReason::InvalidPrice, acct, true});
      return;
    }
    if (!cfg_.maxPrice.isZero() && cfg_.maxPrice < newPrice)
    {
      sink_(CancelRejected{m.id, m.symbol, RejectReason::InvalidPrice, acct, true});
      return;
    }
    if (!cfg_.lotSize.isZero() && (m.newQty.raw() % cfg_.lotSize.raw()) != 0)
    {
      sink_(CancelRejected{m.id, m.symbol, RejectReason::LotSizeViolation, acct, true});
      return;
    }

    // Same price and shrinking: reduce in place, keeping time priority. Release
    // the reservation for the freed quantity (proportional -- works for spot
    // quote/base and perp IM alike) so the trader regains that buying power.
    // Iceberg orders are excluded: their `leaves` is only the displayed peak,
    // not the full remaining (hidden reserve lives in `curHidden`), so neither
    // the proportional release (denominator would be the peak, not the total)
    // nor book_.reduce (leaves the hidden reserve resting) is correct here.
    // They fall through to the re-enter path, which releases the full old
    // reservation and re-reserves at the new quantity.
    if (newPrice == curPrice && m.newQty <= curLeaves && curHidden.isZero())
    {
      book_.reduce(m.id, m.newQty);
      releaseReservationPro(m.id, curLeaves.raw(), m.newQty.raw());
      sink_(OrderModified{m.id, m.symbol, newPrice, m.newQty, true, acct});
      return;
    }

    // Price change or size increase: re-enter at the tail (lost priority). The
    // old reservation is released and buying power is RE-CHECKED at the new
    // price/qty -- a reprice-up must not leave the order under-collateralized.
    book_.cancel(m.id);
    releaseReservation(m.id);
    NewOrder re;
    re.id = m.id;
    re.symbol = cfg_.id;
    re.side = side;
    re.type = OrderType::LIMIT;
    re.price = newPrice;
    re.quantity = m.newQty;
    re.tif = TimeInForce::GTC;
    re.accountId = acct;
    re.reduceOnly = curReduceOnly;  // preserve reduce-only across the modify
    // A modify re-enters matching as a fresh aggressor, so it must carry the
    // self-trade prevention the original order was admitted with. Rebuilding
    // the order from the resting record alone would silently drop it.
    re.stp = stpOf(m.id);

    // Perp risk gate: a modified perp order re-enters matching HERE, not through
    // onNew, so it must run the same reduce-only cap + position-cap checks (spot:
    // perpRiskGate is a no-op). reduceOnly is carried from the resting record onto
    // `re` above, so perpRiskGate re-caps it to the current position -- a modify
    // can neither grow past maxPositionQty nor flip a reduce-only order.
    if (const RejectReason r = perpRiskGate(re); r != RejectReason::None)
    {
      forgetOrder(m.id);  // order was already canceled above -> stays gone
      sink_(OrderRejected{m.id, m.symbol, r, acct});
      return;
    }
    if (!reserveFunds(re))  // cannot fund the modified order -> reject; order is gone
    {
      forgetOrder(m.id);
      sink_(OrderRejected{m.id, m.symbol, RejectReason::InsufficientFunds, acct});
      return;
    }
    const MatchOutcome out =
        matcher_.cross(re, book_, [this]()
                       { return ++tradeSeq_; }, emit_);
    stampFreshHolds();
    if (out.residualRests)
    {
      RestingOrder mro{m.id, acct, newPrice, out.leaves, side};
      mro.reduceOnly = re.reduceOnly;
      book_.addResting(side, mro);
      trackResting(m.id, acct, re.stp);
    }
    sink_(OrderModified{m.id, m.symbol, newPrice, out.leaves, false, acct});
    processTriggers();  // a reprice-into-cross may have moved the last price
  }

  void onCancel(const CancelOrder& c)
  {
    if (c.symbol != cfg_.id)
    {
      sink_(CancelRejected{c.id, c.symbol, RejectReason::UnknownSymbol, c.accountId, false});
      return;
    }
    if (admissionDenies(c.accountId, AdmissionDeny::DenyCancel))
    {
      ++admissionRejects_;
      sink_(CancelRejected{c.id, c.symbol, RejectReason::CancelNotPermitted, c.accountId, false});
      return;
    }
    // Cancel-while-held: deterministically resolve (reject) the order's holds
    // BEFORE removing it. The reject restores held quantity to the book (so the
    // cancel below removes ALL of it and releases the full reservation), and a
    // later accept of the same heldId is impossible (UnknownOrder) -- without
    // this, releaseReservation would strip the held slice and the accept would
    // settle through the unchecked-debit path, breaking conservation.
    rejectHoldsFor(c.id);
    const uint64_t restingAcct = ownerOf(c.id);
    const uint64_t stopAcct = stops_.accountOf(c.id);
    if (book_.cancel(c.id).has_value())
    {
      releaseReservation(c.id);
      forgetOrder(c.id);
      sink_(OrderCanceled{c.id, c.symbol, CancelReason::UserRequested, restingAcct});
    }
    else if (stops_.cancel(c.id))
    {
      // A pending stop holds no reservation, but it does hold an OCO
      // membership: leaving it behind means a later trigger of the sibling
      // looks for an order that no longer exists.
      unlinkOco(c.id);
      forgetOrder(c.id);
      sink_(OrderCanceled{c.id, c.symbol, CancelReason::UserRequested, stopAcct});
    }
    else
    {
      sink_(CancelRejected{c.id, c.symbol, RejectReason::UnknownOrder, c.accountId, false});
    }
  }

  // ---- per-account resting-order tracking (mass-cancel / MMP) ----
  // Owner of a live tracked order (0 if unknown) -- read BEFORE forgetOrder so
  // async cancel events can be routed to the owner's session.
  uint64_t ownerOf(OrderId id) const noexcept
  {
    auto it = orderAccount_.find(id);
    return it == orderAccount_.end() ? 0 : it->second;
  }
  // Self-trade-prevention mode recorded for an order, or None if it asked for
  // none. Reading it back is how a re-entering order keeps the control it was
  // admitted with.
  STPMode stpOf(OrderId id) const
  {
    auto it = orderStp_.find(id);
    return it == orderStp_.end() ? STPMode::None : it->second;
  }

  // Self-trade-prevention scope: the firm group if the account is in one, else
  // the account itself. Read off the matcher's table so the two can never
  // disagree about who counts as the same trader.
  uint64_t stpScope(uint64_t account) const
  {
    const auto& groups = matcher_.stpGroups();
    if (groups.empty())
    {
      return account;
    }
    auto it = groups.find(account);
    return it == groups.end() ? account : it->second;
  }

  // Cancel one leg of a self-matching auction pair, with the same discipline
  // every other engine-side cancel follows: free the reservation, drop the
  // tracking, tell the owner.
  void cancelForStp(OrderId id, uint64_t account)
  {
    book_.cancel(id);
    releaseReservation(id);
    forgetOrder(id);
    sink_(OrderCanceled{id, cfg_.id, CancelReason::SelfTradePrevention, account});
  }

  // Trim one leg by the overlapping quantity without printing. A leg trimmed
  // to nothing is canceled outright, which is also what keeps the uncross loop
  // making progress.
  void decrementForStp(OrderId id, Quantity by)
  {
    const RestingOrder* r = book_.find(id);
    if (r == nullptr)
    {
      return;
    }
    const uint64_t acct = r->accountId;
    const int64_t fromRaw = r->leaves.raw();
    const int64_t toRaw = fromRaw - by.raw();
    if (toRaw <= 0)
    {
      cancelForStp(id, acct);
      return;
    }
    book_.reduce(id, Quantity::fromRaw(toRaw));
    releaseReservationPro(id, fromRaw, toRaw);
  }

  // Every path that puts an order on the book comes through here, and the STP
  // mode is a required argument on purpose: a new rest path cannot compile
  // without saying what self-trade prevention the order carries.
  void trackResting(OrderId id, uint64_t account, STPMode stp)
  {
    orderAccount_[id] = account;
    byAccount_[account].insert(id);
    if (stp != STPMode::None)
    {
      orderStp_[id] = stp;
    }
    else
    {
      orderStp_.erase(id);  // an id can be reused after the previous order left
    }
  }
  void forgetOrder(OrderId id)
  {
    auto it = orderAccount_.find(id);
    if (it == orderAccount_.end())
    {
      return;
    }
    auto ba = byAccount_.find(it->second);
    if (ba != byAccount_.end())
    {
      ba->second.erase(id);
    }
    orderAccount_.erase(it);
    expiry_.erase(id);
    unlinkOco(id);
    pegged_.erase(id);
    orderStp_.erase(id);
  }

  // Remove an order from its OCO group, keeping orderOco_ and ocoMembers_ in
  // sync. The fill path (processOco) erases ocoMembers_ itself; EVERY other exit
  // (cancel / expiry / MMP / halt / liquidation / reject) must route through
  // here, or a departed leg lingers in ocoMembers_ and later cancels a reused
  // OrderId when the surviving sibling resolves (and the group vector leaks).
  // Free the reservation covering the quantity an order just lost. Any path
  // that shrinks a resting order owes this: buying power held against size
  // that no longer rests is the account's money, frozen for nothing.
  void releaseReservationPro(OrderId id, int64_t fromQtyRaw, int64_t toQtyRaw)
  {
    if (ledger_ == nullptr || fromQtyRaw <= 0 || toQtyRaw >= fromQtyRaw)
    {
      return;
    }
    auto it = reserve_.find(id);
    if (it == reserve_.end())
    {
      return;
    }
    const Amount freed = static_cast<Amount>(static_cast<__int128>(it->second.reservedRaw) *
                                             (fromQtyRaw - toQtyRaw) / fromQtyRaw);
    if (freed <= 0)
    {
      return;
    }
    ledger_->release(it->second.account, it->second.asset, freed);
    it->second.reservedRaw -= freed;
  }

  void unlinkOco(OrderId id)
  {
    auto it = orderOco_.find(id);
    if (it == orderOco_.end())
    {
      return;
    }
    if (auto gm = ocoMembers_.find(it->second); gm != ocoMembers_.end())
    {
      auto& v = gm->second;
      v.erase(std::remove(v.begin(), v.end(), id), v.end());
      if (v.empty())
      {
        ocoMembers_.erase(gm);
      }
    }
    orderOco_.erase(it);
  }
  void cancelAllForAccount(uint64_t account, CancelReason reason = CancelReason::UserRequested)
  {
    // Resolve every hold touching this account FIRST (mass-cancel / MMP /
    // liquidation must not leave held fills behind): the account's own held
    // slices are restored and then canceled below; counterparty slices return
    // to the book. Resolving before the id snapshot also catches an order of
    // this account that a restore would re-create.
    rejectHoldsForAccount(account);
    auto it = byAccount_.find(account);
    if (it == byAccount_.end())
    {
      return;
    }
    std::vector<OrderId> ids(it->second.begin(), it->second.end());  // copy: erased in loop
    std::sort(ids.begin(), ids.end());                               // deterministic cancel/event order (layout-independent)
    for (OrderId id : ids)
    {
      if (book_.cancel(id).has_value())
      {
        releaseReservation(id);
        forgetOrder(id);
        sink_(OrderCanceled{id, cfg_.id, reason, account});
      }
    }
  }
  void onMassCancel(const MassCancel& mc)
  {
    if (mc.symbol != 0 && mc.symbol != cfg_.id)
    {
      return;
    }
    cancelAllForAccount(mc.accountId);
  }

  // ---- two-sided market-maker quote (replace prior bid/ask) ----
  void onQuote(const Quote& q)
  {
    if (q.symbol != cfg_.id)
    {
      return;
    }
    if (admissionDenies(q.accountId, AdmissionDeny::DenyQuote))
    {
      ++admissionRejects_;
      sink_(OrderRejected{q.bidId, q.symbol, RejectReason::QuoteNotPermitted, q.accountId});
      return;
    }
    rejectHoldsFor(q.bidId);  // a replaced quote may carry open holds
    rejectHoldsFor(q.askId);
    if (book_.cancel(q.bidId).has_value())
    {
      const uint64_t acct = ownerOf(q.bidId);
      releaseReservation(q.bidId);
      forgetOrder(q.bidId);
      sink_(OrderCanceled{q.bidId, cfg_.id, CancelReason::UserRequested, acct});
    }
    if (book_.cancel(q.askId).has_value())
    {
      const uint64_t acct = ownerOf(q.askId);
      releaseReservation(q.askId);
      forgetOrder(q.askId);
      sink_(OrderCanceled{q.askId, cfg_.id, CancelReason::UserRequested, acct});
    }
    if (q.bidQty.raw() > 0)
    {
      NewOrder b;
      b.id = q.bidId;
      b.symbol = cfg_.id;
      b.side = Side::BUY;
      b.type = OrderType::LIMIT;
      b.price = q.bidPrice;
      b.quantity = q.bidQty;
      b.accountId = q.accountId;
      b.stp = q.stp;
      b.lastLook = q.lastLook;
      onNew(b);
    }
    if (q.askQty.raw() > 0)
    {
      NewOrder a;
      a.id = q.askId;
      a.symbol = cfg_.id;
      a.side = Side::SELL;
      a.type = OrderType::LIMIT;
      a.price = q.askPrice;
      a.quantity = q.askQty;
      a.accountId = q.accountId;
      a.stp = q.stp;
      a.lastLook = q.lastLook;
      onNew(a);
    }
  }

  // ---- market-maker protection: pull all quotes on a fill-rate breach ----
  void onTradeObserved(const Trade& t)
  {
    // Hot path: skip the per-trade hash lookups entirely unless MMP or OCO is
    // actually in use (the common case is neither).
    if (!mmpCfg_.empty())
    {
      mmpAdd(t.makerAccount, t.quantity);
      mmpAdd(t.takerAccount, t.quantity);
    }
    // OCO: a fill on either side wins its group; sibling cancellation is deferred
    // to after matching (processOco) so we never mutate the book mid-match.
    if (!orderOco_.empty())
    {
      if (auto it = orderOco_.find(t.makerId); it != orderOco_.end())
      {
        ocoPending_.emplace_back(it->second, t.makerId);
      }
      if (auto it = orderOco_.find(t.takerId); it != orderOco_.end())
      {
        ocoPending_.emplace_back(it->second, t.takerId);
      }
    }
  }
  void mmpAdd(uint64_t account, Quantity qty)
  {
    auto cfg = mmpCfg_.find(account);
    if (cfg == mmpCfg_.end())
    {
      return;
    }
    auto& w = mmpFills_[account];
    w.fills.emplace_back(now_, qty);
    w.sumRaw += qty.raw();
    while (!w.fills.empty() && w.fills.front().first <= now_ - cfg->second.windowNs)
    {
      w.sumRaw -= w.fills.front().second.raw();
      w.fills.pop_front();
    }
    if (w.sumRaw >= cfg->second.qtyLimit.raw())  // sum >= limit
    {
      bool queued = false;
      for (uint64_t a : mmpBreached_)
      {
        queued |= (a == account);
      }
      if (!queued)
      {
        mmpBreached_.push_back(account);
      }
    }
  }
  void mmpEnforce()
  {
    for (uint64_t acc : mmpBreached_)
    {
      cancelAllForAccount(acc);
      auto& w = mmpFills_[acc];  // re-arm: drop the window and its running sum
      w.fills.clear();
      w.sumRaw = 0;
      sink_(MmpTriggered{acc, cfg_.id});
    }
    mmpBreached_.clear();
  }

  // ---- fees ----
  void emitFees(const Trade& t)
  {
    if (!feesEnabled_)
    {
      return;
    }
    const double notional =
        static_cast<double>(
            notionalRaw(t.price.raw(), t.quantity.raw(), cfg_.priceScale, cfg_.qtyScale)) /
        kMoneyScale;
    sink_(FeeCharged{t.makerId, cfg_.id, Volume::fromDouble(fees_.feeFor(now_.raw(), notional, true)),
                     true, t.makerAccount});
    sink_(FeeCharged{t.takerId, cfg_.id, Volume::fromDouble(fees_.feeFor(now_.raw(), notional, false)),
                     false, t.takerAccount});
  }

  // ---- settlement ledger ----
  struct Reservation
  {
    uint64_t account{};
    AssetId asset{};
    Amount reservedRaw{};
    int64_t limitPriceRaw{};
    Side side{};
  };

  Amount imForRaw(int64_t qtyRaw, int64_t priceRaw) const
  {
    const Amount notional = notionalRaw(priceRaw, qtyRaw, cfg_.priceScale, cfg_.qtyScale);
    return notional * cfg_.initialMarginBps / 10000;
  }

  bool reserveFunds(const NewOrder& o)
  {
    // Permission first, funding second. An external risk owner answers a
    // question the engine cannot ("is this account good for it across every
    // instrument it holds?"), so it has to be asked whether or not this engine
    // also posts collateral -- both money branches below used to return before
    // the hook was ever reached, so binding a ledger silently disabled it.
    if (credit_)
    {
      const CreditDecision d = credit_(CreditRequest{o.id, o.accountId, cfg_.id, o.side, o.type,
                                                     o.price, o.quantity, o.reduceOnly});
      if (!d.allowed)
      {
        creditReason_ = d.reason;  // surfaced by the caller's reject
        return false;
      }
    }
    if (ledger_ != nullptr && cfg_.linearPerp)
    {
      // Derivatives: reserve initial margin in quote collateral (reduce-only
      // reserves nothing -- it frees position margin instead).
      //
      // An unpriced (market / triggered-stop) order is bounded by the price
      // band, and for a linear perp that bound is the band's TOP on BOTH sides.
      // Nothing is delivered here: the exposure is notional, and notional --
      // hence initial margin -- grows with price whether the account is long or
      // short. Bounding a perp SELL at minPrice (which is the correct worst case
      // for a SPOT seller, who delivers base and whose quote proceeds only grow
      // with price -- see the spot branch below) under-reserves it by the whole
      // maxPrice/minPrice ratio, and consumeOrderIM then caps the position's
      // margin at that under-reserved number.
      const int64_t limitRaw = (o.type == OrderType::LIMIT) ? o.price.raw() : cfg_.maxPrice.raw();
      // A market/stop order with no price band (limitRaw == 0) cannot be bounded
      // for margin: reserving 0 IM would let it open a position with no
      // collateral. Reject rather than admit an uncollateralized fill.
      if (!o.reduceOnly && o.type != OrderType::LIMIT && limitRaw <= 0)
      {
        return false;
      }
      const Amount im = o.reduceOnly ? 0 : imForRaw(o.quantity.raw(), limitRaw);
      if (im > 0 && !ledger_->reserve(o.accountId, cfg_.quoteAsset, im))
      {
        return false;
      }
      reserve_[o.id] = Reservation{o.accountId, cfg_.quoteAsset, im, limitRaw, o.side};
      return true;
    }
    if (ledger_ != nullptr)
    {
      AssetId asset;
      Amount amt;
      int64_t limitRaw;
      if (o.side == Side::BUY)
      {
        const Price px = (o.type == OrderType::LIMIT || cfg_.maxPrice.raw() == 0) ? o.price
                                                                                  : cfg_.maxPrice;
        // A market/stop buy with no price band (px == 0) cannot bound its quote
        // spend, so reserving amountOf(0) would gate nothing and let the fill
        // drive the account's reserved balance negative. Reject it instead.
        if (px.raw() <= 0)
        {
          return false;
        }
        asset = cfg_.quoteAsset;
        amt = notionalRaw(px.raw(), o.quantity.raw(), cfg_.priceScale, cfg_.qtyScale);
        limitRaw = px.raw();
      }
      else
      {
        asset = cfg_.baseAsset;
        amt = amountOf(o.quantity);
        limitRaw = o.price.raw();
      }
      if (!ledger_->reserve(o.accountId, asset, amt))
      {
        return false;
      }
      reserve_[o.id] = Reservation{o.accountId, asset, amt, limitRaw, o.side};
      return true;
    }
    return true;
  }

  // ---- journaled balance genesis ----
  // Deposits/withdrawals are sequenced commands, not direct Ledger calls, so
  // the WAL is the single source of truth for balances: a replay from an empty
  // ledger reproduces them. Without a ledger bound they are no-ops.
  void onDeposit(const Deposit& d)
  {
    // A money command against an engine that holds no ledger is answered, not
    // swallowed: silence is indistinguishable from a lost command, and the
    // sender has no other way to learn the funds went nowhere.
    if (ledger_ == nullptr)
    {
      sink_(OrderRejected{0, cfg_.id, RejectReason::NoLedgerBound, d.accountId});
      return;
    }
    if (d.amountRaw <= 0)
    {
      return;
    }
    ledger_->deposit(d.accountId, d.asset, static_cast<Amount>(d.amountRaw));
    // The owner sees the credit land. Deterministic (post-event ledger state),
    // so it is part of the event hash; journal replay re-emits it and the
    // shard's recovery suppression keeps replayed/snapshot-restored copies off
    // the wire.
    emitBalance(d.accountId, d.asset, BalanceReason::Deposit);
  }

  void onWithdraw(const Withdraw& w)
  {
    if (ledger_ == nullptr)
    {
      sink_(OrderRejected{0, cfg_.id, RejectReason::NoLedgerBound, w.accountId});
      return;
    }
    if (w.amountRaw <= 0)
    {
      return;
    }
    // debit is all-or-nothing (checks available >= amount): an uncovered
    // withdraw changes nothing, so conservation and replay stay intact.
    const bool ok = ledger_->debit(w.accountId, w.asset, static_cast<Amount>(w.amountRaw));
    emitBalance(w.accountId, w.asset,
                ok ? BalanceReason::Withdraw : BalanceReason::WithdrawRejected);
  }

  void emitBalance(uint64_t account, AssetId asset, BalanceReason reason)
  {
    sink_(BalanceUpdate{account, asset, static_cast<int64_t>(ledger_->available(account, asset)),
                        static_cast<int64_t>(ledger_->reserved(account, asset)), reason});
  }

  void releaseReservation(OrderId id)
  {
    if (ledger_ == nullptr)
    {
      return;
    }
    auto it = reserve_.find(id);
    if (it == reserve_.end())
    {
      return;
    }
    if (it->second.reservedRaw > 0)
    {
      ledger_->release(it->second.account, it->second.asset, it->second.reservedRaw);
    }
    reserve_.erase(it);
  }

  // Any open last-look hold referencing this order (as maker or taker)?
  bool hasHoldsFor(OrderId id) const
  {
    if (held_.empty())
    {
      return false;
    }
    for (const auto& [hid, h] : held_)
    {
      (void)hid;
      if (h.maker == id || h.taker == id)
      {
        return true;
      }
    }
    return false;
  }

  // releaseReservation, but keep the slice backing this order's open held
  // fills reserved: a canceled residual must not strip the collateral an
  // accept still needs to settle from (the unchecked-debit fallback would
  // credit the counterparty against a possibly failing debit).
  void releaseReservationExceptHeld(OrderId id)
  {
    if (ledger_ == nullptr)
    {
      return;
    }
    Quantity heldQty{};
    for (const auto& [hid, h] : held_)
    {
      (void)hid;
      if (h.taker == id || h.maker == id)
      {
        heldQty += h.qty;
      }
    }
    if (heldQty.isZero())
    {
      releaseReservation(id);
      return;
    }
    auto it = reserve_.find(id);
    if (it == reserve_.end())
    {
      return;
    }
    Amount keep;
    if (cfg_.linearPerp)
    {
      keep = imForRaw(heldQty.raw(), it->second.limitPriceRaw);
    }
    else if (it->second.side == Side::BUY)
    {
      keep = notionalRaw(it->second.limitPriceRaw, heldQty.raw(), cfg_.priceScale, cfg_.qtyScale);
    }
    else
    {
      keep = amountOf(heldQty);
    }
    if (keep > it->second.reservedRaw)
    {
      keep = it->second.reservedRaw;
    }
    const Amount rel = it->second.reservedRaw - keep;
    if (rel > 0)
    {
      ledger_->release(it->second.account, it->second.asset, rel);
      it->second.reservedRaw = keep;
    }
  }

  // After the last hold on an order resolves: if the order is gone from the
  // book (and the stop book), free any leftover reservation and drop tracking.
  // Complements the emit_ wrapper, which defers this cleanup while holds are
  // open (see the complete-with-holds note there).
  void cleanupOrderIfDone(OrderId id)
  {
    if (book_.contains(id) || stops_.contains(id) || hasHoldsFor(id))
    {
      return;
    }
    releaseReservation(id);
    forgetOrder(id);
  }

  // A printed trade could not be settled without creating value, so nothing was
  // moved. Counted and logged rather than event-carried: the counter is a
  // diagnostic (like droppedSnapshotRecords), and adding an event here would
  // change the outbound stream on a path that must stay unreachable.
  void reportUnsettled(const Trade& t, uint64_t account, const char* why)
  {
    ++unsettledTrades_;
    std::fprintf(stderr,
                 "flox-venue: trade %llu on symbol %u NOT settled (%s, account %llu) -- "
                 "no value moved\n",
                 static_cast<unsigned long long>(t.tradeId), static_cast<unsigned>(cfg_.id), why,
                 static_cast<unsigned long long>(account));
  }

  void settleTrade(const Trade& t)
  {
    if (ledger_ == nullptr)
    {
      // A position is EXPOSURE, not cash: it is what reduce-only, the position
      // cap and open interest are computed from, and none of those are money
      // questions. So a ledgerless perp still tracks positions -- only the
      // settlement (PnL, margin, fees) is skipped. Leaving positions_ empty
      // here is what used to make reduce-only reject unconditionally, degrade
      // the position cap to a per-order cap, and publish open interest as a
      // flat zero.
      if (cfg_.linearPerp)
      {
        const bool takerBuys = (t.takerSide == Side::BUY);
        updatePerpPosition(takerBuys ? t.takerAccount : t.makerAccount,
                           takerBuys ? t.takerId : t.makerId, true, t.quantity.raw(),
                           t.price.raw());
        updatePerpPosition(takerBuys ? t.makerAccount : t.takerAccount,
                           takerBuys ? t.makerId : t.takerId, false, t.quantity.raw(),
                           t.price.raw());
      }
      emitFees(t);  // no settlement: fee events only
      return;
    }
    if (cfg_.linearPerp)
    {
      settlePerp(t);
      return;
    }
    const Amount notional =
        notionalRaw(t.price.raw(), t.quantity.raw(), cfg_.priceScale, cfg_.qtyScale);
    const Amount qtyRaw = amountOf(t.quantity);
    const bool takerBuys = (t.takerSide == Side::BUY);
    const OrderId buyerId = takerBuys ? t.takerId : t.makerId;
    const uint64_t buyerAcct = takerBuys ? t.takerAccount : t.makerAccount;
    const OrderId sellerId = takerBuys ? t.makerId : t.takerId;
    const uint64_t sellerAcct = takerBuys ? t.makerAccount : t.takerAccount;

    auto rb = reserve_.find(buyerId);
    auto rs = reserve_.find(sellerId);
    const bool buyerUnreserved = (rb == reserve_.end());
    const bool sellerUnreserved = (rs == reserve_.end());

    // VALUE INTEGRITY. A leg with no reservation has to settle straight out of
    // `available`, and Ledger::debit is all-or-nothing: it can refuse. Crediting
    // the counterparty regardless -- which is what discarding the debit's result
    // amounts to -- CREATES the credited amount out of nothing. So both
    // unreserved legs are funded first, each checked, before anything is
    // credited: if either refuses, the settlement is abandoned as a unit (the
    // first debit, if it happened, is credited back by exactly the same amount),
    // the trade is counted as unsettled and nothing moves. Every path that
    // reaches here is supposed to hold a reservation; this is the floor under
    // any path that ever stops doing so.
    if (buyerUnreserved && !ledger_->debit(buyerAcct, cfg_.quoteAsset, notional))
    {
      reportUnsettled(t, buyerAcct, "buyer cannot fund quote");
      return;
    }
    if (sellerUnreserved && !ledger_->debit(sellerAcct, cfg_.baseAsset, qtyRaw))
    {
      if (buyerUnreserved)
      {
        ledger_->credit(buyerAcct, cfg_.quoteAsset, notional);  // exact undo of the debit above
      }
      reportUnsettled(t, sellerAcct, "seller cannot deliver base");
      return;
    }

    // Past this point the settlement cannot fail: what is left is reserved
    // spending (already ring-fenced) and credits.
    // Buyer: pay quote from reserved (refund over-reservation), receive base.
    if (!buyerUnreserved)
    {
      const Amount limitNotional = notionalRaw(rb->second.limitPriceRaw, t.quantity.raw(),
                                               cfg_.priceScale, cfg_.qtyScale);
      ledger_->spendReserved(buyerAcct, cfg_.quoteAsset, notional);
      if (limitNotional > notional)
      {
        ledger_->release(buyerAcct, cfg_.quoteAsset, limitNotional - notional);
      }
      rb->second.reservedRaw -= limitNotional;
    }
    ledger_->credit(buyerAcct, cfg_.baseAsset, qtyRaw);

    // Seller: deliver base from reserved, receive quote.
    if (!sellerUnreserved)
    {
      ledger_->spendReserved(sellerAcct, cfg_.baseAsset, qtyRaw);
      rs->second.reservedRaw -= qtyRaw;
    }
    ledger_->credit(sellerAcct, cfg_.quoteAsset, notional);

    if (feesEnabled_)
    {
      const double notionalD =
          static_cast<double>(
              notionalRaw(t.price.raw(), t.quantity.raw(), cfg_.priceScale, cfg_.qtyScale)) /
          kMoneyScale;
      chargeFee(t.makerId, t.makerAccount, fees_.feeFor(now_.raw(), notionalD, true), true);
      chargeFee(t.takerId, t.takerAccount, fees_.feeFor(now_.raw(), notionalD, false), false);
    }
  }

  void chargeFee(OrderId id, uint64_t acct, double feeD, bool maker)
  {
    const Amount fee = static_cast<Amount>(Volume::fromDouble(feeD).raw());
    // Signed move: participant -fee, venue +fee (conserves value; fee<0 = rebate).
    ledger_->credit(acct, cfg_.quoteAsset, -fee);
    ledger_->credit(venueAccount_, cfg_.quoteAsset, fee);
    sink_(FeeCharged{id, cfg_.id, Volume::fromDouble(feeD), maker, acct});
  }

  // ---- linear-perp clearing ----
  struct Position
  {
    int64_t qtyRaw{0};    // signed contracts (Quantity raw)
    int64_t entryRaw{0};  // average entry price (Price raw)
    Amount margin{0};     // posted position margin (quote raw), reserved in the ledger
  };

  static int64_t iabs64(int64_t v) { return v < 0 ? -v : v; }

  // Move reserved IM for `qtyRaw` from the order reservation to position margin
  // (stays reserved in the ledger; returns the amount).
  Amount consumeOrderIM(OrderId orderId, int64_t qtyRaw)
  {
    auto it = reserve_.find(orderId);
    if (it == reserve_.end())
    {
      return 0;
    }
    Amount im = imForRaw(qtyRaw, it->second.limitPriceRaw);
    if (im > it->second.reservedRaw)
    {
      im = it->second.reservedRaw;
    }
    it->second.reservedRaw -= im;
    return im;
  }
  // A reducing order's own reserved IM was not needed -> release it to available.
  void releaseOrderIM(OrderId orderId, int64_t qtyRaw, uint64_t acct)
  {
    auto it = reserve_.find(orderId);
    if (it == reserve_.end())
    {
      return;
    }
    Amount im = imForRaw(qtyRaw, it->second.limitPriceRaw);
    if (im > it->second.reservedRaw)
    {
      im = it->second.reservedRaw;
    }
    it->second.reservedRaw -= im;
    if (im > 0)
    {
      ledger_->release(acct, cfg_.quoteAsset, im);
    }
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
        // release position margin for the reduced portion
        const Amount relMargin =
            static_cast<Amount>(static_cast<__int128>(p.margin) * reduceQty / iabs64(p.qtyRaw));
        ledger_->release(acct, cfg_.quoteAsset, relMargin);
        p.margin -= relMargin;
        releaseOrderIM(orderId, reduceQty, acct);
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
      const Amount im = (ledger_ != nullptr) ? consumeOrderIM(orderId, remaining) : 0;
      const int64_t absOld = iabs64(p.qtyRaw);
      const __int128 num = static_cast<__int128>(absOld) * p.entryRaw +
                           static_cast<__int128>(remaining) * priceRaw;
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

  void settlePerp(const Trade& t)
  {
    const bool takerBuys = (t.takerSide == Side::BUY);
    const OrderId buyerId = takerBuys ? t.takerId : t.makerId;
    const uint64_t buyerAcct = takerBuys ? t.takerAccount : t.makerAccount;
    const OrderId sellerId = takerBuys ? t.makerId : t.takerId;
    const uint64_t sellerAcct = takerBuys ? t.makerAccount : t.takerAccount;
    updatePerpPosition(buyerAcct, buyerId, true, t.quantity.raw(), t.price.raw());
    updatePerpPosition(sellerAcct, sellerId, false, t.quantity.raw(), t.price.raw());
    if (feesEnabled_)
    {
      const double notionalD =
          static_cast<double>(
              notionalRaw(t.price.raw(), t.quantity.raw(), cfg_.priceScale, cfg_.qtyScale)) /
          kMoneyScale;
      chargeFee(t.makerId, t.makerAccount, fees_.feeFor(now_.raw(), notionalD, true), true);
      chargeFee(t.takerId, t.takerAccount, fees_.feeFor(now_.raw(), notionalD, false), false);
    }
  }

  // Close a position on someone else's decision. The engine sees one symbol;
  // a portfolio-margin model sees the whole basket and is the only party that
  // can judge an account solvent or not across instruments. Same settlement
  // path as the engine's own sweep, so both produce identical events and the
  // replay cannot tell them apart.
  void onForceClose(const ForceClosePosition& fc)
  {
    if (ledger_ == nullptr)
    {
      sink_(OrderRejected{0, cfg_.id, RejectReason::NoLedgerBound, fc.accountId});
      return;
    }
    if (!cfg_.linearPerp || !hasMark_)
    {
      sink_(OrderRejected{0, cfg_.id, RejectReason::UnknownOrder, fc.accountId});
      return;
    }
    if (positions_.find(fc.accountId) == positions_.end())
    {
      return;  // nothing open: a no-op, not an error
    }
    forceClose(fc.accountId, markPrice_);
  }

  // Maintenance-margin sweep: liquidate every position whose equity (posted
  // margin + unrealized PnL) has fallen below the maintenance requirement.
  // Skipped entirely when an external risk owner drives liquidation: two
  // systems closing the same position from different numbers is worse than
  // either doing it alone.
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
    for (const auto& [acct, p] : positions_)
    {
      const Amount uPnl = unrealizedPnlRaw(acct, mark);
      const Amount notional =
          notionalRaw(mark.raw(), iabs64(p.qtyRaw), cfg_.priceScale, cfg_.qtyScale);
      const Amount mmReq = notional * cfg_.maintenanceMarginBps / 10000;
      // A negative wallet (funding/fees charged to `available` with no free
      // collateral to absorb them) drags the maintenance-equity check: otherwise
      // funding could push a max-leverage payer's wallet unboundedly negative
      // with no liquidation (silent bad debt). A healthy (>=0) wallet stays
      // isolated from the position, so positive balances never prevent an
      // otherwise-due liquidation -- the drag only ever tightens the check.
      const Amount wallet = ledger_->available(acct, cfg_.quoteAsset);
      const Amount walletDrag = wallet < 0 ? wallet : 0;
      if (p.margin + uPnl + walletDrag < mmReq)
      {
        toLiq.push_back(acct);
      }
    }
    // Deterministic order: forceClose emits Liquidation events (folded into the
    // determinism hash) and, with ADL on, shared counterparties make the close
    // order matter -- it must not depend on positions_ (unordered_map) layout.
    std::sort(toLiq.begin(), toLiq.end());
    for (uint64_t a : toLiq)
    {
      forceClose(a, mark);
    }
  }

  // Force-close a position at the mark price: realize PnL through the clearing
  // pool, return posted margin, and let the insurance fund (venue account) cover
  // any negative-equity (bankruptcy) deficit.
  void forceClose(uint64_t acct, Price mark)
  {
    auto it = positions_.find(acct);
    if (it == positions_.end())
    {
      return;
    }
    const Position p = it->second;
    positions_.erase(it);
    // Cancel the account's other resting orders first: their initial margin is
    // locked in `reserved` and is the account's own collateral. Freeing it back
    // to `available` before the bankruptcy test below ensures a total-equity-
    // solvent account covers its own shortfall instead of the insurance fund
    // paying out to it (a mis-socialization invisible to conservation-of-total).
    cancelAllForAccount(acct, CancelReason::Liquidation);
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
      autoDeleverageEngine(sign, deficit, mark);  // claw the deficit back from winners
    }
  }

  // Auto-deleverage the isolated-margin book: recover a bankruptcy deficit from
  // the most profitable opposite-side positions (close at mark, haircut their
  // gain into the insurance fund) instead of socializing it. Same model as the
  // portfolio path (cross_margin.h); every ledger op stays balanced.
  void autoDeleverageEngine(int64_t bankruptSign, Amount deficit, Price mark)
  {
    if (deficit <= 0)
    {
      return;
    }
    struct Cand
    {
      uint64_t acct;
      Amount uPnl;
    };
    std::vector<Cand> cands;
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
    // Most profitable first; `acct` breaks ties deterministically so the chosen
    // ADL victim (an emitted Liquidation, folded into the determinism hash) does
    // not depend on positions_ iteration order or std::sort's instability.
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
      // credit the venue only what is actually confiscated -- otherwise the
      // unconditional credit against a no-op debit mints money. Uncovered
      // remainder falls to the insurance fund (normal ADL waterfall).
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

  // ---- checkpoint helpers ----
  static uint64_t mixAmount(uint64_t h, Amount a) noexcept
  {
    h = mix(h, static_cast<uint64_t>(static_cast<unsigned __int128>(a)));
    h = mix(h, static_cast<uint64_t>(static_cast<unsigned __int128>(a) >> 64));
    return h;
  }

  template <class Map>
  static std::vector<typename Map::key_type> sortedKeys(const Map& m)
  {
    std::vector<typename Map::key_type> keys;
    keys.reserve(m.size());
    for (const auto& [k, v] : m)
    {
      (void)v;
      keys.push_back(k);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
  }

  SeqNanos expiryOf(OrderId id) const
  {
    auto it = expiry_.find(id);
    return it == expiry_.end() ? SeqNanos{} : it->second;
  }
  uint64_t ocoOf(OrderId id) const
  {
    auto it = orderOco_.find(id);
    return it == orderOco_.end() ? 0 : it->second;
  }

  // Pending conditional orders with their current triggers, sorted by order id
  // (the stop book's internal container order is not state: firing and cancel
  // are id-deterministic, so the canonical order for hash/serialization is id).
  std::vector<std::pair<NewOrder, Price>> sortedStops() const
  {
    std::vector<std::pair<NewOrder, Price>> v;
    stops_.forEachPending([&](const NewOrder& o, Price trigger)
                          { v.emplace_back(o, trigger); });
    std::sort(v.begin(), v.end(),
              [](const auto& a, const auto& b)
              { return a.first.id < b.first.id; });
    return v;
  }

  void linkOco(OrderId id, uint64_t group)
  {
    orderOco_[id] = group;
    ocoMembers_[group].push_back(id);
  }

  bool applyRestoreOrder(const RestoreOrder& r)
  {
    if (book_.contains(r.id) || stops_.contains(r.id))
    {
      return false;
    }
    // Corruption tripwire: a continuous-trading book without last look is
    // uncrossed by invariant, so a crossing restore marks the file corrupt.
    // Two legal exceptions: a pre-open (auction) book accumulates crossed
    // (the AdminCmd{BeginPreOpen} record earlier in the file has set
    // auctionMode_ by the time orders restore), and a last-look venue can
    // legitimately hold a crossed book -- a rejected hold restores the maker
    // at its original price on top of a residual that rested through it (see
    // restoreMakerHeld). There the SnapshotEnd stateHash remains the
    // corruption check.
    if (!auctionMode_ && cfg_.lastLookWindowNs.count() == 0)
    {
      if (r.side == Side::BUY)
      {
        if (auto ba = book_.bestAsk(); ba && !(r.price < *ba))
        {
          return false;
        }
      }
      else
      {
        if (auto bb = book_.bestBid(); bb && !(*bb < r.price))
        {
          return false;
        }
      }
    }
    RestingOrder ro{r.id, r.accountId, r.price, r.leaves, r.side};
    ro.hidden = r.hidden;
    ro.peak = r.peak;
    ro.lastLook = r.lastLook;
    ro.reduceOnly = r.reduceOnly;
    // Straight to the tail of its level, NO matching pass: the canonical write
    // order (levels best-first, FIFO within) makes tail-appends reproduce the
    // exact live book layout.
    book_.addResting(r.side, ro);
    trackResting(r.id, r.accountId, STPMode::None);  // set by the RestoreOrderStp that follows
    if (static_cast<bool>(r.expiryNs))
    {
      expiry_[r.id] = r.expiryNs;
    }
    if (r.ocoGroup > 0)
    {
      linkOco(r.id, r.ocoGroup);
    }
    // Buying power is NOT re-derived here: the order's exact reservation
    // arrives as its own RestoreReservation record (live amounts are
    // history-dependent -- partial fills, held slices, STP interactions).
    return true;
  }

  bool applyRestoreStop(const RestoreStop& r)
  {
    if (!isConditional(r.order.type) || stops_.contains(r.order.id) ||
        book_.contains(r.order.id))
    {
      return false;
    }
    if (r.order.ocoGroup > 0)
    {
      linkOco(r.order.id, r.order.ocoGroup);  // a parked stop keeps its OCO link
    }
    // No processTriggers: a restore is not a market event; an in-the-money
    // stop at write time would already have fired live.
    stops_.add(r.order, r.trigger, r.order.type == OrderType::TRAILING_STOP);
    return true;
  }

  bool applyRestoreHeld(const RestoreHeld& r)
  {
    if (held_.count(r.heldId) != 0 || r.qty.raw() <= 0)
    {
      return false;
    }
    Held h{r.heldId, r.taker, r.takerAccount, r.takerSide, r.maker,
           r.makerAccount, r.price, r.qty, r.deadline};
    h.takerTif = r.takerTif;
    h.takerType = r.takerType;
    h.takerPrice = r.takerPrice;
    h.takerExpiryNs = r.takerExpiryNs;
    h.makerReduceOnly = r.makerReduceOnly;
    h.takerReduceOnly = r.takerReduceOnly;
    h.refAtHoldRaw = r.refAtHoldRaw;
    held_[r.heldId] = h;
    heldOpen_.store(held_.size(), std::memory_order_relaxed);
    // Tracking follows the recorded live truth rather than being re-derived: a
    // held maker stays tracked even fully off the book (see createHeld), and
    // the flag also carries snapshots written before the matcher's own removals
    // learned to resolve holds first. Idempotent when the maker also rests
    // (RestoreOrder tracked it). The taker is tracked only while resting.
    // Reservations backing the legs arrive as RestoreReservation records --
    // nothing is re-derived here.
    if (r.makerTracked)
    {
      trackResting(h.maker, h.makerAccount, stpOf(h.maker));
    }
    return true;
  }

  // Restore one buying-power reservation entry EXACTLY as recorded. With
  // exact RestoreBalance splits in the snapshot the ledger's reserved side is
  // already in place, so only the engine-side table is rebuilt; a v1
  // (Deposit-total) snapshot instead moves the amount available -> reserved
  // through the ledger, and a total that cannot back its recorded reservation
  // marks the snapshot corrupt.
  bool applyRestoreReservation(const RestoreReservation& r)
  {
    if (reserve_.count(r.id) != 0)
    {
      return false;
    }
    if (ledger_ == nullptr)
    {
      // The snapshot describes a venue that held money; this engine does not.
      // Accepting it would leave reserve_ empty while stateHash folds it in,
      // and the generation would later be discarded as "corrupt" -- a wrong
      // diagnosis of a sound file. Refuse here, where the reason is knowable.
      if (r.reservedRaw > 0)
      {
        std::fprintf(stderr,
                     "venue: snapshot carries reservations but no ledger is bound "
                     "(order %llu) -- restore refused\n",
                     static_cast<unsigned long long>(r.id));
        return false;
      }
      return true;  // nothing reserved: nothing to honour
    }
    if (!exactBalanceRestore_ && r.reservedRaw > 0 &&
        !ledger_->reserve(r.account, r.asset, r.reservedRaw))
    {
      return false;
    }
    reserve_[r.id] = Reservation{r.account, r.asset, r.reservedRaw, r.limitPriceRaw, r.side};
    return true;
  }

  bool applyRestorePosition(const RestorePosition& r)
  {
    if (r.qtyRaw == 0 || positions_.count(r.account) != 0)
    {
      return false;
    }
    if (ledger_ != nullptr && !exactBalanceRestore_ && r.marginRaw > 0 &&
        !ledger_->reserve(r.account, cfg_.quoteAsset, r.marginRaw))
    {
      return false;  // deposited total cannot back the posted margin -> corrupt
    }
    positions_[r.account] = Position{r.qtyRaw, r.entryRaw, r.marginRaw};
    return true;
  }

  bool applySnapshotEnd(const SnapshotEnd& e)
  {
    tradeSeq_ = e.tradeSeq;
    heldSeq_ = e.heldSeq;
    timeCounter_ = e.timeCounter;
    now_ = SeqNanos::fromRaw(e.nowNs);  // snapshot wire -> sequencer domain
    lastPrice_ = Price::fromRaw(e.lastPriceRaw);
    hasLast_ = e.hasLast;
    markPrice_ = Price::fromRaw(e.markPriceRaw);
    hasMark_ = e.hasMark;
    haltUntil_ = SeqNanos::fromRaw(e.haltUntilNs);
    // The restored flags are the state the feed must start from: re-sync the
    // transition memo so the next real transition is measured against the
    // recovered state, not against the one the config records replayed.
    lastStatus_ = tradingStatus();
    lastStatusUntil_ = lastStatus_ == TradingStatus::LuldPause ? haltUntil_.raw() : 0;
    statusPublished_ = true;
    // Full verification: the reconstructed state must hash to what the writer
    // measured. A mismatch (torn/corrupted/semantically-drifted snapshot)
    // rejects the generation.
    return stateHash() == e.stateHash;
  }

  // ---- last look ----
  struct Held
  {
    uint64_t id{};
    OrderId taker{};
    uint64_t takerAccount{};
    Side takerSide{};
    OrderId maker{};
    uint64_t makerAccount{};
    Price price{};
    Quantity qty{};
    SeqNanos deadline{};
    // The reference when the hold was taken. Last look is about the move
    // DURING the window, so the move has to be measured from here -- measuring
    // from the quoted price instead reports the distance between a quote and
    // the last print, which is a stale quote, not a move.
    int64_t refAtHoldRaw{0};
    // Captured at hold time so a reject can route the taker residual by its
    // TIF and rebuild either leg if it was fully held out of the book.
    TimeInForce takerTif{TimeInForce::GTC};
    OrderType takerType{OrderType::LIMIT};
    Price takerPrice{};
    SeqNanos takerExpiryNs{};
    bool makerReduceOnly{false};
    // Captured so a checkpoint can re-reserve the taker leg's held backing
    // exactly (a perp reduce-only taker reserves nothing).
    bool takerReduceOnly{false};
  };
  void createHeld(const RestingOrder& maker, Quantity fill, const NewOrder& taker)
  {
    const uint64_t id = ++heldSeq_;
    Held h{id,
           taker.id,
           taker.accountId,
           taker.side,
           maker.id,
           maker.accountId,
           maker.price,
           fill,
           now_ + cfg_.lastLookWindowNs,
           // Stamped once the matching pass finishes, not here. See
           // stampFreshHolds().
           0};
    // The taker is an aggressor now, but its residual may rest once the hold
    // resolves -- and a resting order's STP mode is what an auction reads.
    // Capture it here, where the mode is still in hand.
    if (taker.stp != STPMode::None)
    {
      orderStp_[taker.id] = taker.stp;
    }
    h.takerTif = taker.tif;
    h.takerType = taker.type;
    h.takerPrice = taker.price;
    h.takerExpiryNs = taker.expiryNs;
    h.makerReduceOnly = maker.reduceOnly;
    h.takerReduceOnly = taker.reduceOnly;
    held_[id] = h;
    freshHolds_.push_back(id);
    heldOpen_.store(held_.size(), std::memory_order_relaxed);
    // Called BEFORE the matcher reserves the qty out of the book, so the
    // maker's post-hold displayed size is computed here the same way a normal
    // fill would (partial peak -> remainder shown; full peak -> iceberg refill).
    const Quantity displayAfter =
        (fill < maker.leaves) ? (maker.leaves - fill)
                              : ((maker.peak < maker.hidden) ? maker.peak : maker.hidden);
    sink_(FillHeld{id, cfg_.id, maker.id, taker.id, maker.price, fill, displayAfter,
                   maker.accountId, taker.accountId});
    // NOTE: the maker stays tracked (orderAccount_/byAccount_) even when the
    // hold empties its displayed size and fillBest removes it from the book --
    // the id is still live (a reject restores it) and mass-cancel paths must
    // still find it.
  }
  // Release one leg's buying-power reservation for a held quantity that will NOT
  // trade (last-look reject / timeout). Mirrors the per-fill decrement settleTrade
  // would have applied, but returns the funds to `available` instead of spending
  // them -- so both parties are made whole for a fill that never happened, and no
  // reservation is stranded in `reserved` forever. Cleans up a fully-released,
  // no-longer-resting order's reservation + tracking maps.
  void releaseHeldLeg(OrderId id, Quantity qty)
  {
    if (ledger_ == nullptr)
    {
      return;
    }
    auto it = reserve_.find(id);
    if (it == reserve_.end())
    {
      return;
    }
    Amount rel;
    if (cfg_.linearPerp)
    {
      rel = imForRaw(qty.raw(), it->second.limitPriceRaw);
    }
    else if (it->second.side == Side::BUY)
    {
      rel = notionalRaw(it->second.limitPriceRaw, qty.raw(), cfg_.priceScale, cfg_.qtyScale);
    }
    else
    {
      rel = amountOf(qty);
    }
    if (rel > it->second.reservedRaw)
    {
      rel = it->second.reservedRaw;
    }
    if (rel > 0)
    {
      ledger_->release(it->second.account, it->second.asset, rel);
    }
    it->second.reservedRaw -= rel;
    if (it->second.reservedRaw <= 0 && !book_.contains(id))
    {
      reserve_.erase(it);
      forgetOrder(id);
    }
  }
  void resolveHeld(typename std::unordered_map<uint64_t, Held>::iterator it, bool accept)
  {
    const Held h = it->second;
    held_.erase(it);
    heldOpen_.store(held_.size(), std::memory_order_relaxed);
    // An accept settles a fill that was risk-checked when the hold was created,
    // possibly a whole window ago. Re-measure it against the position as it is
    // now: a perp fill that would open or flip a reduce-only leg (with no margin
    // behind it) or carry an account past the position cap must not print just
    // because the maker said yes late. The hold is rejected instead -- the
    // honest outcome, since the venue cannot part-accept a hold: liquidity is
    // restored to both legs exactly as a maker reject would.
    if (accept && !holdStillAllowed(h))
    {
      ++riskRejectedHolds_;
      accept = false;
    }
    // Symmetric price tolerance, applied by the venue on magnitude alone.
    const int64_t moveRaw = referenceMoveSinceHold(h);
    if (cfg_.lastLookToleranceRaw > 0)
    {
      const int64_t mag = moveRaw < 0 ? -moveRaw : moveRaw;
      if (mag > cfg_.lastLookToleranceRaw)
      {
        ++toleranceRejectedHolds_;
        accept = false;
      }
    }
    recordHoldOutcome(h, moveRaw, accept);
    if (accept)
    {
      emit_(Trade{++tradeSeq_, cfg_.id, h.price, h.qty, h.maker, h.taker, h.takerSide,
                  h.makerAccount, h.takerAccount});
      const RestingOrder* mk = book_.find(h.maker);
      const Quantity makerLeaves = mk ? Quantity::fromRaw(mk->leaves.raw() + mk->hidden.raw()) : Quantity{};
      const Quantity makerDisp = mk ? mk->leaves : Quantity{};  // displayed peak for public feed
      emit_(OrderExecuted{h.maker, cfg_.id, h.qty, makerLeaves, false, makerLeaves.isZero(), h.price,
                          makerDisp, h.makerAccount});
      emit_(OrderExecuted{h.taker, cfg_.id, h.qty, Quantity{}, true, false, h.price, Quantity{},
                          h.takerAccount});
    }
    else
    {
      // Reject / timeout: the held qty does not trade, and liquidity must not
      // be destroyed. The maker's displayed qty returns to its price level (at
      // the TAIL, as-if re-entered -- see docs/venue/matching.md); its
      // reservation was never touched and keeps backing it. The taker residual
      // follows its TIF: GTC/GTD rests, IOC/FOK/MARKET is canceled with the
      // matching residual reason (that leg's buying power is released). The
      // cancel paths resolve holds BEFORE removing an order, so a leg that is
      // absent from the book here was fully held out of it -- never "gone".
      restoreMakerHeld(h);
      restoreTakerHeld(h);
      sink_(FillRejected{h.id, cfg_.id, h.taker, h.maker, h.price, h.qty, h.takerAccount,
                         h.makerAccount});
    }
    // Whichever way the hold resolved: if this was the last hold on a leg and
    // that leg no longer rests, free its leftover reservation and tracking
    // (deferred from the emit_ wrapper while holds were open).
    cleanupOrderIfDone(h.taker);
    cleanupOrderIfDone(h.maker);
  }

  // Where the market is, for judging a held fill: the mid of the book when both
  // sides are quoted, and the last trade only when they are not.
  //
  // The two are not interchangeable here. A trade prints half a spread off the
  // mid, on whichever side the aggressor took, so consecutive prints move by
  // the spread even in a market that has not moved at all. Over a hold window
  // measured in milliseconds that is most of what the last price does, and a
  // tolerance or a conduct statistic built on it is reading the spread.
  //
  // It also decides whether the statistic has any power. A maker prices off its
  // own view of the market, not off this venue's tape; measuring its behaviour
  // against a series it never looked at classifies its refusals at random. In
  // this deployment that is the normal case rather than the exception -- the
  // liquidity provider aggregates several venues and this one is a fraction of
  // what it sees. The mid is the closest thing here to what it is actually
  // looking at.
  //
  // Zero when neither is available, which is the only honest answer before
  // anything has traded or been quoted: it means unmeasurable, not unmoved.
  // The reference a hold is judged from has to describe the book as it stands
  // FOR THE DURATION of the hold, which is not the book that existed the
  // instant before it opened.
  //
  // Opening a hold reserves the maker's quantity out of the book, so the side
  // the aggressor hit loses its touch and the mid steps away from the
  // aggressor. Stamping before that and comparing after makes the hold's own
  // mechanism look like a market move -- always in the same direction, since a
  // buyer always removes an ask. In an example run with buy-only probe flow
  // this put 1,204 holds in the adverse bucket against 556 favourable, on a
  // market whose moves were symmetric by construction, and it flattened the
  // conduct statistic it was feeding.
  //
  // So the stamp waits until the matching pass is over and the book has
  // settled. Both ends of the comparison then describe the same book.
  void stampFreshHolds()
  {
    if (freshHolds_.empty())
    {
      return;
    }
    const int64_t ref = referenceRaw();
    for (uint64_t id : freshHolds_)
    {
      auto it = held_.find(id);
      if (it != held_.end())
      {
        it->second.refAtHoldRaw = ref;
      }
    }
    freshHolds_.clear();
  }

  int64_t referenceRaw() const
  {
    const auto bb = book_.bestBid();
    const auto ba = book_.bestAsk();
    if (bb && ba)
    {
      return (bb->raw() + ba->raw()) / 2;
    }
    return hasLast_ ? lastPrice_.raw() : 0;
  }

  // How far the reference has moved since the hold was taken, signed so that a
  // positive value means it moved AGAINST the maker: it sold and the price rose,
  // or it bought and the price fell. Zero when there is no reference to compare
  // against.
  int64_t referenceMoveSinceHold(const Held& h) const
  {
    const int64_t nowRaw = referenceRaw();
    if (nowRaw == 0 || h.refAtHoldRaw == 0)
    {
      return 0;
    }
    const int64_t delta = nowRaw - h.refAtHoldRaw;
    // takerSide is the aggressor's. A taker buying leaves the maker short, so a
    // rising price hurts the maker.
    return h.takerSide == Side::BUY ? delta : -delta;
  }

  void recordHoldOutcome(const Held& h, int64_t moveRaw, bool accepted)
  {
    LastLookStats& st = lastLookStats_[h.makerAccount];
    ++st.held;
    if (accepted)
    {
      ++st.accepted;
    }
    else
    {
      ++st.rejected;
    }
    if (moveRaw > 0)
    {
      ++st.adverse;
      if (!accepted)
      {
        ++st.rejectedAdverse;
      }
    }
    else if (moveRaw < 0)
    {
      ++st.favourable;
      if (!accepted)
      {
        ++st.rejectedFavourable;
      }
    }
  }

  // Would settling this hold in full still pass the perp risk limits? Both legs
  // are measured on the live position (see fillLimit); spot and ledgerless
  // engines have no positions, so nothing constrains them.
  bool holdStillAllowed(const Held& h) const
  {
    if (!cfg_.linearPerp || ledger_ == nullptr)
    {
      return true;
    }
    const Side makerSide = (h.takerSide == Side::BUY) ? Side::SELL : Side::BUY;
    CancelReason ignored = CancelReason::ReduceOnlyNotReducing;
    if (legFillLimit(h.makerAccount, makerSide, h.makerReduceOnly, h.qty.raw(), ignored) <
        h.qty.raw())
    {
      return false;
    }
    return legFillLimit(h.takerAccount, h.takerSide, h.takerReduceOnly, h.qty.raw(), ignored) >=
           h.qty.raw();
  }

  // Return a rejected hold's qty to the maker: back onto its price level at the
  // tail (as-if re-entered). If the hold consumed the whole displayed size the
  // order left the book -- rebuild it from the hold record.
  void restoreMakerHeld(const Held& h)
  {
    if (auto ro = book_.cancel(h.maker); ro.has_value())
    {
      ro->leaves += h.qty;  // the returned slice was displayed when it was held
      book_.addResting(ro->side, *ro);
      sink_(OrderModified{h.maker, cfg_.id, ro->price, ro->leaves, false, h.makerAccount});
    }
    else
    {
      const Side makerSide = (h.takerSide == Side::BUY) ? Side::SELL : Side::BUY;
      RestingOrder rebuilt{h.maker, h.makerAccount, h.price, h.qty, makerSide};
      rebuilt.lastLook = true;
      rebuilt.reduceOnly = h.makerReduceOnly;
      book_.addResting(makerSide, rebuilt);
      // Still tracked in orderAccount_/byAccount_: a fully-held maker is never
      // forgotten while its hold is open (see createHeld).
      sink_(OrderModified{h.maker, cfg_.id, h.price, h.qty, false, h.makerAccount});
    }
  }

  // Return a rejected hold's qty to the taker per its TIF.
  void restoreTakerHeld(const Held& h)
  {
    const bool rests = h.takerType == OrderType::LIMIT &&
                       (h.takerTif == TimeInForce::GTC || h.takerTif == TimeInForce::GTD);
    if (rests)
    {
      // Reservation keeps backing the restored resting quantity.
      if (auto ro = book_.cancel(h.taker); ro.has_value())
      {
        ro->leaves += h.qty;  // combine with the already-resting remainder, tail requeue
        book_.addResting(ro->side, *ro);
        sink_(OrderModified{h.taker, cfg_.id, ro->price, ro->leaves, false, h.takerAccount});
      }
      else
      {
        RestingOrder rebuilt{h.taker, h.takerAccount, h.takerPrice, h.qty, h.takerSide};
        book_.addResting(h.takerSide, rebuilt);
        trackResting(h.taker, h.takerAccount, stpOf(h.taker));
        if (h.takerTif == TimeInForce::GTD && static_cast<bool>(h.takerExpiryNs))
        {
          expiry_[h.taker] = h.takerExpiryNs;
        }
        sink_(OrderAccepted{h.taker, cfg_.id, h.takerSide, h.takerPrice, h.qty, true, h.qty,
                            h.takerAccount});
      }
      return;
    }
    // IOC / FOK / MARKET: the residual never rests -- release the taker's held
    // buying power and cancel it with the reason its TIF would have produced.
    releaseHeldLeg(h.taker, h.qty);
    const CancelReason reason = (h.takerType == OrderType::MARKET) ? CancelReason::MarketResidual
                                : (h.takerTif == TimeInForce::FOK)
                                    ? CancelReason::FillOrKillResidual
                                    : CancelReason::ImmediateOrCancelResidual;
    sink_(OrderCanceled{h.taker, cfg_.id, reason, h.takerAccount});
  }

  // Deterministically resolve (reject) every open hold that references `id` as
  // maker or taker. MUST run before any path that permanently removes the order
  // or reshapes its reservation (cancel/modify/quote-replace/expiry/OCO/peg):
  // a hold left behind would let a later accept settle with no backing
  // reservation (unchecked debit -> conservation breach).
  void rejectHoldsFor(OrderId id)
  {
    if (held_.empty())
    {
      return;
    }
    std::vector<uint64_t> due;
    for (const auto& [hid, h] : held_)
    {
      if (h.maker == id || h.taker == id)
      {
        due.push_back(hid);
      }
    }
    std::sort(due.begin(), due.end());  // deterministic resolution/event order
    for (uint64_t hid : due)
    {
      if (auto it = held_.find(hid); it != held_.end())
      {
        resolveHeld(it, false);
      }
    }
  }

  // Account-scope variant for mass-cancel / MMP / liquidation.
  void rejectHoldsForAccount(uint64_t account)
  {
    if (held_.empty())
    {
      return;
    }
    std::vector<uint64_t> due;
    for (const auto& [hid, h] : held_)
    {
      if (h.makerAccount == account || h.takerAccount == account)
      {
        due.push_back(hid);
      }
    }
    std::sort(due.begin(), due.end());
    for (uint64_t hid : due)
    {
      if (auto it = held_.find(hid); it != held_.end())
      {
        resolveHeld(it, false);
      }
    }
  }

  void rejectAllHolds()
  {
    if (held_.empty())
    {
      return;
    }
    std::vector<uint64_t> due;
    due.reserve(held_.size());
    for (const auto& [hid, h] : held_)
    {
      (void)h;
      due.push_back(hid);
    }
    std::sort(due.begin(), due.end());
    for (uint64_t hid : due)
    {
      if (auto it = held_.find(hid); it != held_.end())
      {
        resolveHeld(it, false);
      }
    }
  }

  void onLastLookDecision(const LastLookDecision& d)
  {
    auto it = held_.find(d.heldId);
    if (it == held_.end())
    {
      sink_(OrderRejected{d.heldId, cfg_.id, RejectReason::UnknownOrder, d.accountId});
      return;
    }
    // Ownership: only the maker whose quote is held may decide its fate.
    if (d.accountId != it->second.makerAccount)
    {
      sink_(OrderRejected{d.heldId, cfg_.id, RejectReason::NotOrderOwner, d.accountId});
      return;
    }
    resolveHeld(it, d.accept);
  }
  // GTD: cancel resting orders whose expiry time has passed (deterministic by
  // sequencer-ts). Runs on every submit before the command is processed.
  void expireOrders()
  {
    if (expiry_.empty())
    {
      return;
    }
    std::vector<OrderId> due;
    for (const auto& [id, exp] : expiry_)
    {
      if (now_ >= exp)
      {
        due.push_back(id);
      }
    }
    std::sort(due.begin(), due.end());  // deterministic expiry/event order (layout-independent)
    for (OrderId id : due)
    {
      expiry_.erase(id);
      rejectHoldsFor(id);                // expiry removes the order: resolve holds first
      if (book_.cancel(id).has_value())  // still resting -> expire it
      {
        const uint64_t acct = ownerOf(id);
        releaseReservation(id);
        forgetOrder(id);
        sink_(OrderCanceled{id, cfg_.id, CancelReason::Expired, acct});
      }
      else if (stops_.cancel(id))  // never triggered -> expire the conditional
      {
        const uint64_t acct = ownerOf(id);
        unlinkOco(id);
        forgetOrder(id);
        sink_(OrderCanceled{id, cfg_.id, CancelReason::Expired, acct});
      }
    }
  }

  // Peg price for `side` tracking `ref` (+ signed offset), tick-aligned, clamped
  // to not cross the opposite touch and to stay inside the price band.
  int64_t pegTargetRaw(Side side, PegRef ref, int64_t offsetRaw) const
  {
    const auto bb = book_.bestBid();
    const auto ba = book_.bestAsk();
    const int64_t last =
        hasLast_ ? lastPrice_.raw() : ((cfg_.minPrice.raw() + cfg_.maxPrice.raw()) / 2);
    int64_t refRaw;
    switch (ref)
    {
      case PegRef::Bid:
        refRaw = bb ? bb->raw() : (ba ? ba->raw() : last);
        break;
      case PegRef::Ask:
        refRaw = ba ? ba->raw() : (bb ? bb->raw() : last);
        break;
      case PegRef::Mid:
        refRaw = (bb && ba) ? (bb->raw() + ba->raw()) / 2 : (bb ? bb->raw() : (ba ? ba->raw() : last));
        break;
      default:
        return 0;
    }
    int64_t target = refRaw + offsetRaw;
    const int64_t tick = cfg_.tickSize.raw();
    if (tick > 0)
    {
      target = target / tick * tick;  // align down to tick
    }
    if (side == Side::BUY && ba && target >= ba->raw())
    {
      target = ba->raw() - tick;  // never cross
    }
    if (side == Side::SELL && bb && target <= bb->raw())
    {
      target = bb->raw() + tick;
    }
    if (cfg_.minPrice.raw() > 0 && target < cfg_.minPrice.raw())
    {
      target = cfg_.minPrice.raw();
    }
    if (cfg_.maxPrice.raw() > 0 && target > cfg_.maxPrice.raw())
    {
      target = cfg_.maxPrice.raw();
    }
    return target;
  }

  // Re-price pegged orders to track the book at each submit boundary. Repricing
  // re-queues the order (time priority resets) and, for real-money settlement,
  // re-reserves buying power at the new price (cancels the peg if unaffordable).
  void repeg()
  {
    if (pegged_.empty())
    {
      return;
    }
    std::vector<OrderId> ids;
    ids.reserve(pegged_.size());
    for (const auto& [id, p] : pegged_)
    {
      ids.push_back(id);
    }
    // Deterministic order: a peg reprice reads the book that PRIOR pegs in this
    // pass already mutated, so the processing order is state-affecting. Sort by id
    // so the outcome does not depend on pegged_ (unordered_map) layout -- same
    // layout-independence standard as the ADL/liquidation paths.
    std::sort(ids.begin(), ids.end());
    for (OrderId id : ids)
    {
      auto pit = pegged_.find(id);
      if (pit == pegged_.end())
      {
        continue;
      }
      const Peg pg = pit->second;
      // A peg reprice releases and re-reserves buying power at the new price;
      // an open hold against the old price/reservation would settle against a
      // reservation that no longer covers it -- resolve holds first.
      rejectHoldsFor(id);
      // Remove the order from the book BEFORE computing its peg target: otherwise
      // pegTargetRaw reads best-bid/ask/mid INCLUDING this order's own resting
      // quantity, so a peg that is the touch references itself and ratchets one
      // tick toward the opposite touch on every submit (creep + priority churn +
      // OrderModified spam) even when the real market never moved. Cancelling
      // first mirrors the creation path, where the order isn't resting yet.
      auto ro = book_.cancel(id);
      if (!ro)
      {
        pegged_.erase(id);
        continue;  // already filled / gone
      }
      const int64_t target = pegTargetRaw(pg.side, pg.ref, pg.offsetRaw);
      if (ro->price.raw() == target)
      {
        book_.addResting(ro->side, *ro);  // unchanged -> put it back
        continue;
      }
      // The reprice is re-funded whether or not a ledger is bound: with no
      // ledger reserveFunds still consults the setCreditCheck hook, and
      // skipping the whole block here was the one path where a repriced peg
      // escaped a check that submit and stop-trigger both apply.
      {
        releaseReservation(id);  // no-op without a ledger
        NewOrder synth;
        synth.id = id;
        synth.symbol = cfg_.id;
        synth.side = ro->side;
        synth.type = OrderType::LIMIT;
        synth.price = Price::fromRaw(target);
        synth.quantity = Quantity::fromRaw(ro->leaves.raw() + ro->hidden.raw());
        synth.accountId = ro->accountId;
        if (!reserveFunds(synth))  // cannot fund the reprice -> drop the peg
        {
          forgetOrder(id);
          pegged_.erase(id);
          sink_(OrderCanceled{id, cfg_.id, CancelReason::UserRequested, ro->accountId});
          continue;
        }
      }
      RestingOrder nr = *ro;
      nr.price = Price::fromRaw(target);
      book_.addResting(nr.side, nr);
      sink_(OrderModified{id, cfg_.id, Price::fromRaw(target), nr.leaves, false, nr.accountId});
    }
  }

  // OCO: after matching, cancel the losing siblings of every group that had a
  // fill this submit. The filled ("winner") order is left alone.
  void processOco()
  {
    if (ocoPending_.empty())
    {
      return;
    }
    for (const auto& [group, winner] : ocoPending_)
    {
      auto git = ocoMembers_.find(group);
      if (git == ocoMembers_.end())
      {
        continue;  // already resolved this submit
      }
      std::vector<OrderId> members = git->second;  // copy: cancel mutates maps
      // Deterministic sibling order by id: group membership is a SET (the
      // insertion order is not state -- a checkpoint restore rebuilds it in
      // canonical book order), so the cancel/event order must not depend on it.
      std::sort(members.begin(), members.end());
      ocoMembers_.erase(git);
      for (OrderId id : members)
      {
        orderOco_.erase(id);
        if (id != winner)
        {
          cancelOcoSibling(id);
        }
      }
    }
    ocoPending_.clear();
  }
  void cancelOcoSibling(OrderId id)
  {
    rejectHoldsFor(id);  // the sibling leaves for good: resolve its holds first
    const uint64_t restingAcct = ownerOf(id);
    const uint64_t stopAcct = stops_.accountOf(id);
    if (book_.cancel(id).has_value())
    {
      releaseReservation(id);
      forgetOrder(id);
      sink_(OrderCanceled{id, cfg_.id, CancelReason::OcoTriggered, restingAcct});
    }
    else if (stops_.cancel(id))
    {
      sink_(OrderCanceled{id, cfg_.id, CancelReason::OcoTriggered, stopAcct});
    }
  }

  void expireHolds()
  {
    if (held_.empty())
    {
      return;
    }
    std::vector<uint64_t> due;
    for (const auto& [id, h] : held_)
    {
      if (now_ >= h.deadline)
      {
        due.push_back(id);
      }
    }
    // Deterministic order: a timeout-accept assigns ++tradeSeq_, so the resolution
    // order feeds the event stream -- must not depend on held_ (unordered_map) layout.
    std::sort(due.begin(), due.end());
    for (uint64_t id : due)
    {
      auto it = held_.find(id);
      if (it != held_.end())
      {
        resolveHeld(it, cfg_.lastLookAcceptOnTimeout);
      }
    }
  }

  SymbolConfig cfg_;
  EventSink sink_;
  EventSink emit_;  // sink_ wrapper that tracks lastPrice_ from trades
  Matcher<Book> matcher_;
  Book book_;
  StopBook stops_;
  Price lastPrice_{};
  bool hasLast_{false};
  std::vector<uint64_t> freshHolds_;  // stamped at the end of the matching pass
  Price markPrice_{};
  bool hasMark_{false};
  uint64_t tradeSeq_{0};

  SeqNanos now_{};
  int64_t timeCounter_{0};
  std::unordered_map<OrderId, uint64_t> orderAccount_;
  std::unordered_map<uint64_t, std::unordered_set<OrderId>> byAccount_;
  std::unordered_map<OrderId, SeqNanos> expiry_;                   // GTD: orderId -> expiry, sequencer time
  std::unordered_map<OrderId, uint64_t> orderOco_;                 // orderId -> OCO group
  std::unordered_map<uint64_t, std::vector<OrderId>> ocoMembers_;  // group -> member orderIds
  std::vector<std::pair<uint64_t, OrderId>> ocoPending_;           // (group, winner) collected while matching
  struct Peg
  {
    Side side;
    PegRef ref;
    int64_t offsetRaw;
  };
  std::unordered_map<OrderId, Peg> pegged_;  // orderId -> peg spec (re-priced each submit)
  // orderId -> self-trade-prevention mode, for orders that asked for one. Only
  // the auction uncross and a modify re-entry read it: continuous matching
  // takes the mode off the aggressor. Sparse -- STPMode::None is absent.
  std::unordered_map<OrderId, STPMode> orderStp_;
  // account -> admission profile. Empty table and absent entries both mean
  // "everything permitted", so an engine that was never given profiles behaves
  // exactly as before.
  std::unordered_map<uint64_t, AdmissionProfile> admission_;
  bool delisted_{false};          // withdrawn from trading; outranks halt / session / auction
  uint64_t admissionRejects_{0};  // observability: a counterparty sending what it may not

  flox::FeeSchedule fees_;
  bool feesEnabled_{false};

  struct MmpCfg
  {
    Quantity qtyLimit{};
    DurationNs windowNs{};
  };
  std::unordered_map<uint64_t, MmpCfg> mmpCfg_;
  // Sliding fill window per account with an incrementally maintained sum, so a
  // breach check is O(1) amortised rather than an O(n) rescan of the deque on
  // every fill (an active MM inside the window would otherwise be O(n^2)).
  struct MmpWindow
  {
    std::deque<std::pair<SeqNanos, Quantity>> fills;
    int64_t sumRaw{0};  // running sum of fills.second.raw()
  };
  std::unordered_map<uint64_t, MmpWindow> mmpFills_;
  std::vector<uint64_t> mmpBreached_;
  CreditCheck credit_;
  // Reason from the last refused credit check, so the reject the client sees
  // says why ("portfolio margin", say) instead of a flat InsufficientFunds.
  mutable RejectReason creditReason_{RejectReason::InsufficientFunds};
  // Diagnostic only, like the pro-rata skip counters: conduct measurement, not
  // matching state, so it stays out of the state hash and the snapshot.
  std::unordered_map<uint64_t, LastLookStats> lastLookStats_;
  uint64_t toleranceRejectedHolds_{0};

  std::unordered_map<uint64_t, Held> held_;
  uint64_t heldSeq_{0};
  // Mirror of held_.size() readable from other threads (the shard's idle
  // sweeper); the engine itself never reads it for logic.
  std::atomic<uint64_t> heldOpen_{0};

  // clientOrderId dedup index, per account. Window = the engine session
  // (uptime); rotation/compaction is a future checkpoint concern -- see
  // docs/venue/matching.md. Rebuilt naturally by journal replay.
  std::unordered_map<uint64_t, std::unordered_set<uint64_t>> clientOrderIds_;

  // Snapshot-only records seen (and dropped) on the live submit path.
  uint64_t droppedSnapshotRecords_{0};

  // Clearing-integrity counters (diagnostics, not hashed state): trades left
  // unsettled rather than settled by creating value, and last-look accepts
  // refused at decision time by a perp risk limit.
  uint64_t unsettledTrades_{0};
  uint64_t riskRejectedHolds_{0};

  // Recovery mode flag: this snapshot carried exact RestoreBalance splits, so
  // RestoreReservation / RestorePosition must not move ledger money (v1
  // Deposit-total snapshots leave it false and keep the re-reservation path).
  bool exactBalanceRestore_{false};

  Ledger* ledger_{nullptr};
  uint64_t venueAccount_{0};
  std::unordered_map<OrderId, Reservation> reserve_;
  std::unordered_map<uint64_t, Position> positions_;  // perp positions per account
  bool auctionMode_{false};
  SeqNanos haltUntil_{};
  // Session state, deliberately separate from cfg_.halted: a closed session and
  // an operator halt are different facts with different reject reasons, and a
  // close must not clear a halt underneath it. Hashed and checkpointed.
  bool closed_{false};

  // Last trading state published, so the feed carries transitions only. Not
  // hashed and not snapshotted: it is a de-duplication memo of what went OUT,
  // not engine state -- the state itself is (halted, haltUntil_, auctionMode_,
  // closed_), which stateHash already covers and a snapshot already restores.
  TradingStatus lastStatus_{TradingStatus::Trading};
  int64_t lastStatusUntil_{0};
  bool statusPublished_{false};

  // Last funding rate applied, kFundingRateScale (published, never used in
  // matching), and the live funding calendar set by SetFundingSchedule
  // (0 = none: nextFundingNs() falls back to the config derivation). All three
  // are hashed and carried by the checkpoint as RestoreFunding -- a restored
  // engine publishes the rate and the boundary the venue will actually settle
  // on, not a zero and a formula.
  int64_t fundingRateRaw_{0};
  DurationNs fundingIntervalNs_{};
  SeqNanos nextFundingNs_{};
};

}  // namespace flox::venue
