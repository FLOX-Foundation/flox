/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

// MatchingEngine<Book>: construction, command dispatch and the engine's own surface.
//
// Included only from flox-venue/matching_engine.h, which declares every member
// defined here. Including it directly gives a fragment with no class to attach
// to, so the include is refused rather than left to fail on the first method.
#ifndef FLOX_VENUE_MATCHING_ENGINE_INL
#error "flox-venue/engine/dispatch.inl is a fragment of flox-venue/matching_engine.h; include that instead"
#endif

namespace flox::venue
{

template <class Book>
MatchingEngine<Book>::MatchingEngine(SymbolConfig cfg, EventSink sink, Book book,
                                     MatchPolicy policy)
    : cfg_(cfg),
      sink_(std::move(sink)),
      matcher_(policy),
      book_(std::move(book)),
      // Clearing delegates back the three things it cannot reach: the order
      // reservation a fill's initial margin moves out of, the release of an
      // unneeded reservation on a reducing fill, and the resting orders of an
      // account about to be liquidated. Captureless lambdas, so these are
      // plain function pointers -- no vtable on the fill path.
      clearing_(cfg_, sink_,
                engine::Clearing::Hooks{
                    this,
                    [](void* c, OrderId id, int64_t qtyRaw)
                    { return static_cast<MatchingEngine*>(c)->consumeOrderIM(id, qtyRaw); },
                    [](void* c, OrderId id, int64_t qtyRaw, uint64_t acct)
                    { static_cast<MatchingEngine*>(c)->releaseOrderIM(id, qtyRaw, acct); },
                    [](void* c, uint64_t acct)
                    {
                      static_cast<MatchingEngine*>(c)->cancelAllForAccount(
                          acct, CancelReason::Liquidation);
                    }})
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
    matcher_.setFillLimitDryHook([this](const RestingOrder& maker, const NewOrder& taker,
                                        Quantity want, const PositionDeltas& deltas)
                                 { return fillLimitDry(maker, taker, want, deltas); });
  }
}

template <class Book>
void MatchingEngine<Book>::submit(const InboundCommand& cmd)
{
  submit(cmd, SeqNanos::fromRaw(++timeCounter_));
}

// Timestamped submit (sequencer-stamped). Drives last-look expiry and MMP
// windows deterministically.
// The ingestion boundary is the one legitimate crossing into sequencer time:
// the caller hands a raw tick (wall-derived at capture, journal-derived on
// replay) and it becomes SeqNanos here, in exactly one place.
template <class Book>
void MatchingEngine<Book>::submit(const InboundCommand& cmd, int64_t tsRawNs)
{
  submit(cmd, SeqNanos::fromRaw(tsRawNs));
}

template <class Book>
void MatchingEngine<Book>::submit(const InboundCommand& cmd, SeqNanos tsNs)
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
  else if (const auto* ap = std::get_if<AdjustPosition>(&cmd))
  {
    if (ap->symbol == cfg_.id)
    {
      onAdjustPosition(*ap);
    }
  }
  else if (std::get_if<TimeTick>(&cmd) != nullptr)
  {
    // Pure time sweep: expireHolds/expireOrders already ran above. Sequenced
    // and journaled so a replay reproduces the same timeouts (see tick()).
  }
  // ListInstrument is consumed above the engine (InstrumentRegistry / router);
  // an existing engine has nothing to do with its own listing record.
  //
  // Anything else with no branch above is not rejected, logged or counted --
  // it is dropped, and the venue carries on as if it had never been sent.
  // Snapshot-only records are supposed to land here; a new LIVE command is
  // not, and nothing but this says so.
  static_assert(std::variant_size_v<InboundCommand> == 35,
                "new InboundCommand alternative: give it a branch in submit(), or confirm it "
                "is snapshot-only and handled in applySnapshotRecord");
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
template <class Book>
void MatchingEngine<Book>::tick(int64_t nowRawNs)
{
  const SeqNanos nowNs = SeqNanos::fromRaw(nowRawNs);
  if (nowNs > now_)
  {
    now_ = nowNs;
  }
  expireHolds();
  expireOrders();
}

// Live resting orders tracked on this symbol (observability gauge).
template <class Book>
uint64_t MatchingEngine<Book>::restingOrderCount() const noexcept
{
  return orderAccount_.size();
}

// Sequencer-ts of the last command the engine applied. This is the ONLY
// honest "when" an outbound event has: the events themselves carry no time,
// and a wall-clock read taken downstream would not reproduce on replay. A
// publisher that stamps market data with it (market_data.h) produces the same
// timestamps on a journal replay as it did live.
template <class Book>
SeqNanos MatchingEngine<Book>::engineTime() const noexcept
{
  return now_;
}

template <class Book>
int64_t MatchingEngine<Book>::engineTimeNs() const noexcept
{
  return now_.raw();
}

template <class Book>
const Book& MatchingEngine<Book>::book() const noexcept
{
  return book_;
}

template <class Book>
uint64_t MatchingEngine<Book>::tradesGenerated() const noexcept
{
  return tradeSeq_;
}

template <class Book>
const SymbolConfig& MatchingEngine<Book>::config() const noexcept
{
  return cfg_;
}

// Live risk-limit adjustment (control-plane): operators tighten these during
// volatility without a restart. Only the risk knobs are mutable -- structural
// fields (symbol id, tick size, assets, linearPerp) stay fixed. Applies to
// subsequent orders; existing resting orders are unaffected.
// Apply only the limits the record claims. The direct setters below still
// exist for pre-start wiring; on a running engine this is the route that
// survives a restart and reproduces on a replica.
template <class Book>
void MatchingEngine<Book>::applyRiskLimits(const SetRiskLimits& r) noexcept
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
template <class Book>
SetRiskLimits MatchingEngine<Book>::riskLimits() const noexcept
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

template <class Book>
void MatchingEngine<Book>::setLuldBps(int32_t bps) noexcept
{
  cfg_.luldBps = bps;
}

template <class Book>
void MatchingEngine<Book>::setTriggerRef(TriggerRef ref) noexcept
{
  cfg_.triggerRef = ref;
}

template <class Book>
void MatchingEngine<Book>::setPriceBand(Price minPrice, Price maxPrice) noexcept
{
  cfg_.minPrice = minPrice;
  cfg_.maxPrice = maxPrice;
}

template <class Book>
void MatchingEngine<Book>::setFatFinger(Quantity maxOrderQty, Volume maxOrderNotional) noexcept
{
  cfg_.maxOrderQty = maxOrderQty;
  cfg_.maxOrderNotional = maxOrderNotional;
}

template <class Book>
void MatchingEngine<Book>::setPositionLimit(Quantity maxPositionQty) noexcept
{
  cfg_.maxPositionQty = maxPositionQty;
}

template <class Book>
void MatchingEngine<Book>::setMaxOpenOrders(uint32_t n) noexcept
{
  cfg_.maxOpenOrders = n;
}

template <class Book>
void MatchingEngine<Book>::setMarginBps(int32_t initialBps, int32_t maintenanceBps) noexcept
{
  cfg_.initialMarginBps = initialBps;
  cfg_.maintenanceMarginBps = maintenanceBps;
}

// Pro-rata participants excluded by the fill-time risk limits.
template <class Book>
uint64_t MatchingEngine<Book>::skippedRiskProRata() const noexcept
{
  return matcher_.skippedRiskProRata();
}

// All-or-none accounting (see Matcher::fillOrKillPrechecks).
template <class Book>
uint64_t MatchingEngine<Book>::fillOrKillPrechecks() const noexcept
{
  return matcher_.fillOrKillPrechecks();
}

template <class Book>
uint64_t MatchingEngine<Book>::fillOrKillRiskConstrained() const noexcept
{
  return matcher_.fillOrKillRiskConstrained();
}

template <class Book>
uint64_t MatchingEngine<Book>::fillOrKillRejected() const noexcept
{
  return matcher_.fillOrKillRejected();
}

}  // namespace flox::venue
