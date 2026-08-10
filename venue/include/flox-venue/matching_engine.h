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
#include "flox-venue/matcher.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/messages.h"
#include "flox-venue/stop_book.h"
#include "flox/book/resting_order.h"

#include "flox/backtest/fee_schedule.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace flox::venue
{

// Reference price a conditional order (stop / take-profit / trailing) triggers
// against. Spot: last trade price. Derivatives: usually the mark price.
enum class TriggerRef : uint8_t
{
  Last = 0,
  Mark = 1,
};

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
  int64_t lastLookWindowNs{0};          // 0 = last look disabled venue-wide
  bool lastLookAcceptOnTimeout{false};  // window elapses with no decision -> accept vs reject
  AssetId baseAsset{0};                 // e.g. BTC in BTC-USD (settled to the seller/buyer)
  AssetId quoteAsset{1};                // e.g. USD in BTC-USD
  int32_t luldBps{0};                   // limit-up/limit-down band around the reference (0 = off)
  int64_t luldHaltNs{0};                // trading pause on a band breach
  bool linearPerp{false};               // derivatives: linear perpetual (margin, no asset delivery)
  int32_t initialMarginBps{0};          // IM as bps of notional (1000 = 10% = 10x leverage)
  int32_t maintenanceMarginBps{0};      // MM; position liquidated when equity < MM (0 = off)
  bool autoDeleverage{false};           // ADL: recover a bankruptcy deficit from winners before insurance
  Quantity maxPositionQty{};            // 0 = unchecked (max |position| per account, perp risk cap)
  uint32_t maxOpenOrders{0};            // 0 = unchecked (max live resting orders per account)

  // Per-symbol fixed-point scale, same semantics as core SymbolInfo. Default
  // 1e8 = the compile-time Price/Quantity scale. Money always settles at
  // kMoneyScale regardless. Must satisfy scalesValid().
  int64_t priceScale{Price::Scale};
  int64_t qtyScale{Quantity::Scale};
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
        if (x->complete)
        {
          releaseReservation(x->id);  // free any over-reserved remainder
          forgetOrder(x->id);
        }
      }
      else if (const auto* c = std::get_if<OrderCanceled>(&e))
      {
        releaseReservation(c->id);
        forgetOrder(c->id);
      }
      sink_(e);
      if (const auto* t = std::get_if<Trade>(&e))
      {
        settleTrade(*t);  // move base/quote + fees; emits FeeCharged via sink_
      }
    };

    // Last look: the matcher hands each held fill here.
    matcher_.setLastLookHook([this](const RestingOrder& maker, Quantity fill, const NewOrder& taker)
                             { createHeld(maker, fill, taker); });
  }

  void submit(const InboundCommand& cmd) { submit(cmd, ++timeCounter_); }

  // Timestamped submit (sequencer-stamped). Drives last-look expiry and MMP
  // windows deterministically.
  void submit(const InboundCommand& cmd, int64_t tsNs)
  {
    now_ = tsNs;
    if (cfg_.halted && haltUntil_ > 0 && now_ >= haltUntil_)
    {
      cfg_.halted = false;  // timed LULD volatility pause elapsed
      haltUntil_ = 0;
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
    processOco();
    repeg();
    mmpEnforce();
  }

  void setFeeSchedule(flox::FeeSchedule fees)
  {
    fees_ = std::move(fees);
    feesEnabled_ = true;
  }

  // Market-maker protection: if `qtyLimit` is filled for `account` within
  // `windowNs`, all its resting orders are pulled.
  void setMmp(uint64_t account, Quantity qtyLimit, int64_t windowNs)
  {
    mmpCfg_[account] = MmpCfg{qtyLimit, windowNs};
  }

  // Pre-trade credit / buying-power gate. Returns true if the account may place
  // the order. A real deployment binds this to the account/balance service.
  using CreditCheck = std::function<bool(uint64_t account, Side, Price, Quantity)>;
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
    if (ledger_ == nullptr)
    {
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
  }

  // External mark-price update (derivatives). Drives mark-referenced stops and
  // maintenance-margin liquidations.
  void setMarkPrice(Price mark)
  {
    markPrice_ = mark;
    hasMark_ = true;
    processTriggers();
    checkLiquidations(mark);
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
  void setHalted(bool halted) noexcept { cfg_.halted = halted; }  // control-plane hook

  // Live risk-limit adjustment (control-plane): operators tighten these during
  // volatility without a restart. Only the risk knobs are mutable -- structural
  // fields (symbol id, tick size, assets, linearPerp) stay fixed. Applies to
  // subsequent orders; existing resting orders are unaffected.
  void setLuldBps(int32_t bps) noexcept { cfg_.luldBps = bps; }
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
  // prevention fires across all of a firm's accounts, not just the same account.
  void setStpGroup(uint64_t account, uint64_t group) { matcher_.setStpGroup(account, group); }

  // Operator emergency stop: halt the symbol (reject new orders) AND pull the
  // entire resting book -- every live limit order and pending conditional --
  // releasing reservations. Used on a fat-finger event or system anomaly.
  void haltAndCancelAll()
  {
    cfg_.halted = true;
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
        releaseReservation(id);
        forgetOrder(id);
        sink_(OrderCanceled{id, cfg_.id, CancelReason::VenueHalt});
      }
    }
    for (OrderId id : stops_.ids())
    {
      if (stops_.cancel(id))
      {
        releaseReservation(id);
        sink_(OrderCanceled{id, cfg_.id, CancelReason::VenueHalt});
      }
    }
  }

  // ---- session / auctions ----
  // Pre-open: orders accumulate without matching (a crossed book is allowed).
  void beginPreOpen() noexcept { auctionMode_ = true; }

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
    }
  }

  // Resume a halted symbol through a re-opening auction: clear the halt (stop
  // rejecting) and enter pre-open accumulation. Orders build a (possibly crossed)
  // book without matching until the operator calls openContinuous(), which
  // uncrosses at the single volume-maximizing price and switches to continuous.
  // This is how venues reopen after a halt -- never straight into continuous.
  void resumeWithAuction() noexcept
  {
    cfg_.halted = false;
    haltUntil_ = 0;
    auctionMode_ = true;
  }
  // Run the (opening / closing) uncross auction: match everything at the single
  // volume-maximizing price, then resume continuous trading.
  void openContinuous()
  {
    runAuction();
    auctionMode_ = false;
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
      const Quantity fill = (bid->leaves < ask->leaves) ? bid->leaves : ask->leaves;
      const OrderId bidId = bid->id;
      const OrderId askId = ask->id;
      const uint64_t bAcct = bid->accountId;
      const uint64_t aAcct = ask->accountId;
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
      emit_(OrderExecuted{bidId, cfg_.id, fill, bl, false, bl.isZero(), P, bDisp});
      emit_(OrderExecuted{askId, cfg_.id, fill, al, false, al.isZero(), P, aDisp});
    }
  }

 private:
  RejectReason validate(const NewOrder& o) const
  {
    if (o.symbol != cfg_.id)
    {
      return RejectReason::UnknownSymbol;
    }
    if (cfg_.halted)
    {
      return RejectReason::Halted;
    }
    if (o.quantity.raw() <= 0)
    {
      return RejectReason::InvalidQuantity;
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
    return RejectReason::None;
  }

  // Per-order perp risk gates shared by onNew and the trigger path (a triggered
  // stop re-enters matching directly, NOT through onNew, so it must run these too
  // -- otherwise a reduce-only stop opens an uncollateralized position and a
  // plain stop bypasses the position cap). Caps a reduce-only order to the
  // opposing position (0 reducible -> reject: nothing to reduce) and rejects a
  // fill that would breach maxPositionQty. Mutates o.quantity for the cap.
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

  void onNew(NewOrder o)
  {
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
        sink_(OrderRejected{o.id, o.symbol, RejectReason::TooManyOpenOrders});
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
      sink_(OrderRejected{o.id, o.symbol, r});
      return;
    }
    if (const RejectReason r = validate(o); r != RejectReason::None)
    {
      sink_(OrderRejected{o.id, o.symbol, r});
      return;
    }
    if (!reserveFunds(o))
    {
      sink_(OrderRejected{o.id, o.symbol, RejectReason::InsufficientFunds});
      return;  // pre-trade buying-power (ledger reservation or credit hook)
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
      trackResting(o.id, o.accountId);
      sink_(OrderAccepted{o.id, o.symbol, o.side, restPx, o.quantity, true});
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
        sink_(OrderRejected{o.id, o.symbol, RejectReason::LuldBreach});
        tripLuldHalt();
        return;
      }
    }

    const MatchOutcome out =
        matcher_.cross(o, book_, [this]()
                       { return ++tradeSeq_; }, emit_);

    if (out.reject != RejectReason::None)
    {
      releaseReservation(o.id);  // post-only-would-cross / FOK-unfulfillable: free the reserve
      sink_(OrderRejected{o.id, o.symbol, out.reject});
      return;
    }
    if (out.residualRests)
    {
      RestingOrder ro{o.id, o.accountId, o.price, out.leaves, o.side};
      ro.lastLook = o.lastLook;
      ro.reduceOnly = o.reduceOnly;  // carried so a later modify preserves it
      if (o.visibleQuantity.raw() > 0 && o.visibleQuantity < out.leaves)
      {
        ro.peak = o.visibleQuantity;    // iceberg: show a peak, hide the rest
        ro.leaves = o.visibleQuantity;  // displayed
        ro.hidden = out.leaves - o.visibleQuantity;
      }
      book_.addResting(o.side, ro);
      trackResting(o.id, o.accountId);
      if (o.tif == TimeInForce::GTD && o.expiryNs > 0)
      {
        expiry_[o.id] = o.expiryNs;
      }
      if (o.peg != PegRef::None)
      {
        pegged_[o.id] = {o.side, o.peg, o.pegOffsetRaw};
      }
      // Public feed shows only the displayed peak (ro.leaves); the hidden iceberg
      // reserve (out.leaves - ro.leaves) is not leaked. Non-iceberg: they match.
      sink_(OrderAccepted{o.id, o.symbol, o.side, o.price, out.leaves, true, ro.leaves});
    }
    else if (out.residualCanceled)
    {
      // The unfilled residual (IOC/FOK/MARKET/STP-newest) never rests, so its
      // buying-power reservation must be released here -- this raw-sink path does
      // not run the emit_ wrapper's release-on-cancel.
      releaseReservation(o.id);
      sink_(OrderCanceled{o.id, o.symbol, out.residualCancelReason});
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
      sink_(OrderRejected{o.id, o.symbol, RejectReason::UnknownSymbol});
      return false;
    }
    if (cfg_.halted)
    {
      sink_(OrderRejected{o.id, o.symbol, RejectReason::Halted});
      return false;
    }
    if (o.quantity.raw() <= 0)
    {
      sink_(OrderRejected{o.id, o.symbol, RejectReason::InvalidQuantity});
      return false;
    }
    const bool trailing = (o.type == OrderType::TRAILING_STOP);
    if (!trailing && o.triggerPrice.raw() <= 0)
    {
      sink_(OrderRejected{o.id, o.symbol, RejectReason::InvalidPrice});
      return false;
    }
    if (trailing && o.trailingOffset.raw() <= 0)
    {
      sink_(OrderRejected{o.id, o.symbol, RejectReason::InvalidPrice});
      return false;
    }
    if (isLimitStop(o.type) && o.price.raw() <= 0)
    {
      sink_(OrderRejected{o.id, o.symbol, RejectReason::InvalidPrice});
      return false;
    }
    if (book_.contains(o.id) || stops_.contains(o.id))
    {
      sink_(OrderRejected{o.id, o.symbol, RejectReason::DuplicateOrderId});
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
    sink_(OrderAccepted{o.id, o.symbol, o.side, o.triggerPrice, o.quantity, false});  // pending, not on book
    processTriggers();                                                                // may already be in-the-money
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
    auto ref = triggerReference();
    if (!ref)
    {
      return;
    }
    stops_.updateTrailing(*ref);
    while (auto agg = stops_.popTriggered(*ref))
    {
      sink_(OrderTriggered{agg->id, cfg_.id, *ref});
      // Perp risk gates: a triggered stop re-enters matching HERE, not through
      // onNew, so it must run the same reduce-only cap + position-limit checks --
      // else a reduce-only stop opens an uncollateralized (im=0) position and a
      // plain stop bypasses maxPositionQty. Reject (and free nothing -- not yet
      // reserved) if the reduce-only cap leaves nothing or the cap is breached.
      if (const RejectReason r = perpRiskGate(*agg); r != RejectReason::None)
      {
        sink_(OrderRejected{agg->id, cfg_.id, r});
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
        sink_(OrderRejected{agg->id, cfg_.id, RejectReason::InsufficientFunds});
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
      if (out.reject != RejectReason::None)
      {
        releaseReservation(agg->id);
        sink_(OrderRejected{agg->id, cfg_.id, out.reject});
      }
      else if (out.residualRests)
      {
        RestingOrder rro{agg->id, agg->accountId, agg->price, out.leaves, agg->side};
        rro.reduceOnly = agg->reduceOnly;
        book_.addResting(agg->side, rro);
        trackResting(agg->id, agg->accountId);
        sink_(OrderAccepted{agg->id, cfg_.id, agg->side, agg->price, out.leaves, true});
      }
      else if (out.residualCanceled)
      {
        releaseReservation(agg->id);
        sink_(OrderCanceled{agg->id, cfg_.id, out.residualCancelReason});
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
      sink_(OrderRejected{m.id, m.symbol, RejectReason::UnknownSymbol});
      return;
    }
    const RestingOrder* cur = book_.find(m.id);
    if (cur == nullptr)
    {
      sink_(OrderRejected{m.id, m.symbol, RejectReason::UnknownOrder});
      return;
    }
    if (m.newQty.raw() <= 0)
    {
      sink_(OrderRejected{m.id, m.symbol, RejectReason::InvalidQuantity});
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
      sink_(OrderRejected{m.id, m.symbol, RejectReason::InvalidPrice});
      return;
    }
    if (!cfg_.tickSize.isZero() && (newPrice.raw() % cfg_.tickSize.raw()) != 0)
    {
      sink_(OrderRejected{m.id, m.symbol, RejectReason::TickSizeViolation});
      return;
    }
    if (!cfg_.minPrice.isZero() && newPrice < cfg_.minPrice)
    {
      sink_(OrderRejected{m.id, m.symbol, RejectReason::InvalidPrice});
      return;
    }
    if (!cfg_.maxPrice.isZero() && cfg_.maxPrice < newPrice)
    {
      sink_(OrderRejected{m.id, m.symbol, RejectReason::InvalidPrice});
      return;
    }
    if (!cfg_.lotSize.isZero() && (m.newQty.raw() % cfg_.lotSize.raw()) != 0)
    {
      sink_(OrderRejected{m.id, m.symbol, RejectReason::LotSizeViolation});
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
      if (ledger_ != nullptr && m.newQty < curLeaves)
      {
        if (auto it = reserve_.find(m.id); it != reserve_.end() && curLeaves.raw() > 0)
        {
          const Amount freed = static_cast<Amount>(static_cast<__int128>(it->second.reservedRaw) *
                                                   (curLeaves.raw() - m.newQty.raw()) / curLeaves.raw());
          ledger_->release(it->second.account, it->second.asset, freed);
          it->second.reservedRaw -= freed;
        }
      }
      sink_(OrderModified{m.id, m.symbol, newPrice, m.newQty, true});
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

    // Perp risk gate: a modified perp order re-enters matching HERE, not through
    // onNew, so it must run the same reduce-only cap + position-cap checks (spot:
    // perpRiskGate is a no-op). reduceOnly is carried from the resting record onto
    // `re` above, so perpRiskGate re-caps it to the current position -- a modify
    // can neither grow past maxPositionQty nor flip a reduce-only order.
    if (const RejectReason r = perpRiskGate(re); r != RejectReason::None)
    {
      forgetOrder(m.id);  // order was already canceled above -> stays gone
      sink_(OrderRejected{m.id, m.symbol, r});
      return;
    }
    if (!reserveFunds(re))  // cannot fund the modified order -> reject; order is gone
    {
      forgetOrder(m.id);
      sink_(OrderRejected{m.id, m.symbol, RejectReason::InsufficientFunds});
      return;
    }
    const MatchOutcome out =
        matcher_.cross(re, book_, [this]()
                       { return ++tradeSeq_; }, emit_);
    if (out.residualRests)
    {
      RestingOrder mro{m.id, acct, newPrice, out.leaves, side};
      mro.reduceOnly = re.reduceOnly;
      book_.addResting(side, mro);
      trackResting(m.id, acct);
    }
    sink_(OrderModified{m.id, m.symbol, newPrice, out.leaves, false});
    processTriggers();  // a reprice-into-cross may have moved the last price
  }

  void onCancel(const CancelOrder& c)
  {
    if (c.symbol != cfg_.id)
    {
      sink_(OrderRejected{c.id, c.symbol, RejectReason::UnknownSymbol});
      return;
    }
    if (book_.cancel(c.id).has_value())
    {
      releaseReservation(c.id);
      forgetOrder(c.id);
      sink_(OrderCanceled{c.id, c.symbol, CancelReason::UserRequested});
    }
    else if (stops_.cancel(c.id))
    {
      sink_(OrderCanceled{c.id, c.symbol, CancelReason::UserRequested});
    }
    else
    {
      sink_(OrderRejected{c.id, c.symbol, RejectReason::UnknownOrder});
    }
  }

  // ---- per-account resting-order tracking (mass-cancel / MMP) ----
  void trackResting(OrderId id, uint64_t account)
  {
    orderAccount_[id] = account;
    byAccount_[account].insert(id);
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
  }

  // Remove an order from its OCO group, keeping orderOco_ and ocoMembers_ in
  // sync. The fill path (processOco) erases ocoMembers_ itself; EVERY other exit
  // (cancel / expiry / MMP / halt / liquidation / reject) must route through
  // here, or a departed leg lingers in ocoMembers_ and later cancels a reused
  // OrderId when the surviving sibling resolves (and the group vector leaks).
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
        sink_(OrderCanceled{id, cfg_.id, reason});
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
    if (book_.cancel(q.bidId).has_value())
    {
      releaseReservation(q.bidId);
      forgetOrder(q.bidId);
      sink_(OrderCanceled{q.bidId, cfg_.id, CancelReason::UserRequested});
    }
    if (book_.cancel(q.askId).has_value())
    {
      releaseReservation(q.askId);
      forgetOrder(q.askId);
      sink_(OrderCanceled{q.askId, cfg_.id, CancelReason::UserRequested});
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
    sink_(FeeCharged{t.makerId, cfg_.id, Volume::fromDouble(fees_.feeFor(now_, notional, true)), true});
    sink_(FeeCharged{t.takerId, cfg_.id, Volume::fromDouble(fees_.feeFor(now_, notional, false)), false});
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
    if (ledger_ != nullptr && cfg_.linearPerp)
    {
      // Derivatives: reserve initial margin in quote collateral (reduce-only
      // reserves nothing -- it frees position margin instead).
      const int64_t limitRaw =
          (o.type == OrderType::LIMIT) ? o.price.raw()
                                       : (o.side == Side::BUY ? cfg_.maxPrice.raw() : cfg_.minPrice.raw());
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
    if (credit_)
    {
      return credit_(o.accountId, o.side, o.price, o.quantity);
    }
    return true;
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

  void settleTrade(const Trade& t)
  {
    if (ledger_ == nullptr)
    {
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

    // Buyer: pay quote from reserved (refund over-reservation), receive base.
    if (auto rb = reserve_.find(buyerId); rb != reserve_.end())
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
    else
    {
      ledger_->debit(buyerAcct, cfg_.quoteAsset, notional);
    }
    ledger_->credit(buyerAcct, cfg_.baseAsset, qtyRaw);

    // Seller: deliver base from reserved, receive quote.
    if (auto rs = reserve_.find(sellerId); rs != reserve_.end())
    {
      ledger_->spendReserved(sellerAcct, cfg_.baseAsset, qtyRaw);
      rs->second.reservedRaw -= qtyRaw;
    }
    else
    {
      ledger_->debit(sellerAcct, cfg_.baseAsset, qtyRaw);
    }
    ledger_->credit(sellerAcct, cfg_.quoteAsset, notional);

    if (feesEnabled_)
    {
      const double notionalD =
          static_cast<double>(
              notionalRaw(t.price.raw(), t.quantity.raw(), cfg_.priceScale, cfg_.qtyScale)) /
          kMoneyScale;
      chargeFee(t.makerId, t.makerAccount, fees_.feeFor(now_, notionalD, true), true);
      chargeFee(t.takerId, t.takerAccount, fees_.feeFor(now_, notionalD, false), false);
    }
  }

  void chargeFee(OrderId id, uint64_t acct, double feeD, bool maker)
  {
    const Amount fee = static_cast<Amount>(Volume::fromDouble(feeD).raw());
    // Signed move: participant -fee, venue +fee (conserves value; fee<0 = rebate).
    ledger_->credit(acct, cfg_.quoteAsset, -fee);
    ledger_->credit(venueAccount_, cfg_.quoteAsset, fee);
    sink_(FeeCharged{id, cfg_.id, Volume::fromDouble(feeD), maker});
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
      p.qtyRaw += fillSign * reduceQty;  // toward zero
      remaining -= reduceQty;
      if (p.qtyRaw == 0)
      {
        p.entryRaw = 0;
      }
    }

    if (remaining > 0)
    {
      const Amount im = consumeOrderIM(orderId, remaining);
      const int64_t absOld = iabs64(p.qtyRaw);
      const __int128 num = static_cast<__int128>(absOld) * p.entryRaw +
                           static_cast<__int128>(remaining) * priceRaw;
      p.entryRaw = static_cast<int64_t>(num / (absOld + remaining));
      p.qtyRaw += fillSign * remaining;
      p.margin += im;
    }

    if (p.qtyRaw == 0)
    {
      if (p.margin > 0)
      {
        ledger_->release(acct, cfg_.quoteAsset, p.margin);
        p.margin = 0;
      }
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
      chargeFee(t.makerId, t.makerAccount, fees_.feeFor(now_, notionalD, true), true);
      chargeFee(t.takerId, t.takerAccount, fees_.feeFor(now_, notionalD, false), false);
    }
  }

  // Maintenance-margin sweep: liquidate every position whose equity (posted
  // margin + unrealized PnL) has fallen below the maintenance requirement.
  void checkLiquidations(Price mark)
  {
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
    int64_t deadline{};
  };
  void createHeld(const RestingOrder& maker, Quantity fill, const NewOrder& taker)
  {
    const uint64_t id = ++heldSeq_;
    held_[id] = Held{id, taker.id, taker.accountId, taker.side, maker.id,
                     maker.accountId, maker.price, fill, now_ + cfg_.lastLookWindowNs};
    sink_(FillHeld{id, cfg_.id, maker.id, taker.id, maker.price, fill});
    if (!book_.contains(maker.id))
    {
      forgetOrder(maker.id);
    }
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
    if (accept)
    {
      emit_(Trade{++tradeSeq_, cfg_.id, h.price, h.qty, h.maker, h.taker, h.takerSide,
                  h.makerAccount, h.takerAccount});
      const RestingOrder* mk = book_.find(h.maker);
      const Quantity makerLeaves = mk ? Quantity::fromRaw(mk->leaves.raw() + mk->hidden.raw()) : Quantity{};
      const Quantity makerDisp = mk ? mk->leaves : Quantity{};  // displayed peak for public feed
      emit_(OrderExecuted{h.maker, cfg_.id, h.qty, makerLeaves, false, makerLeaves.isZero(), h.price,
                          makerDisp});
      emit_(OrderExecuted{h.taker, cfg_.id, h.qty, Quantity{}, true, false, h.price, Quantity{}});
    }
    else
    {
      // Reject / timeout: the held qty does not trade. Return both parties' held
      // buying power to available (else it is stranded in `reserved` forever and
      // the taker is never made whole for the fill that did not happen).
      releaseHeldLeg(h.taker, h.qty);
      releaseHeldLeg(h.maker, h.qty);
      sink_(FillRejected{h.id, cfg_.id, h.taker});
    }
  }
  void onLastLookDecision(const LastLookDecision& d)
  {
    auto it = held_.find(d.heldId);
    if (it == held_.end())
    {
      sink_(OrderRejected{d.heldId, cfg_.id, RejectReason::UnknownOrder});
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
      if (book_.cancel(id).has_value())  // still resting -> expire it
      {
        releaseReservation(id);
        forgetOrder(id);
        sink_(OrderCanceled{id, cfg_.id, CancelReason::Expired});
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
      if (ledger_ != nullptr)
      {
        releaseReservation(id);
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
          sink_(OrderCanceled{id, cfg_.id, CancelReason::UserRequested});
          continue;
        }
      }
      RestingOrder nr = *ro;
      nr.price = Price::fromRaw(target);
      book_.addResting(nr.side, nr);
      sink_(OrderModified{id, cfg_.id, Price::fromRaw(target), nr.leaves, false});
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
      const std::vector<OrderId> members = git->second;  // copy: cancel mutates maps
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
    if (book_.cancel(id).has_value())
    {
      releaseReservation(id);
      forgetOrder(id);
      sink_(OrderCanceled{id, cfg_.id, CancelReason::OcoTriggered});
    }
    else if (stops_.cancel(id))
    {
      sink_(OrderCanceled{id, cfg_.id, CancelReason::OcoTriggered});
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
  Price markPrice_{};
  bool hasMark_{false};
  uint64_t tradeSeq_{0};

  int64_t now_{0};
  int64_t timeCounter_{0};
  std::unordered_map<OrderId, uint64_t> orderAccount_;
  std::unordered_map<uint64_t, std::unordered_set<OrderId>> byAccount_;
  std::unordered_map<OrderId, int64_t> expiry_;                    // GTD: orderId -> expiry sequencer-ts
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

  flox::FeeSchedule fees_;
  bool feesEnabled_{false};

  struct MmpCfg
  {
    Quantity qtyLimit{};
    int64_t windowNs{};
  };
  std::unordered_map<uint64_t, MmpCfg> mmpCfg_;
  // Sliding fill window per account with an incrementally maintained sum, so a
  // breach check is O(1) amortised rather than an O(n) rescan of the deque on
  // every fill (an active MM inside the window would otherwise be O(n^2)).
  struct MmpWindow
  {
    std::deque<std::pair<int64_t, Quantity>> fills;
    int64_t sumRaw{0};  // running sum of fills.second.raw()
  };
  std::unordered_map<uint64_t, MmpWindow> mmpFills_;
  std::vector<uint64_t> mmpBreached_;
  CreditCheck credit_;

  std::unordered_map<uint64_t, Held> held_;
  uint64_t heldSeq_{0};

  Ledger* ledger_{nullptr};
  uint64_t venueAccount_{0};
  std::unordered_map<OrderId, Reservation> reserve_;
  std::unordered_map<uint64_t, Position> positions_;  // perp positions per account
  bool auctionMode_{false};
  int64_t haltUntil_{0};
};

}  // namespace flox::venue
