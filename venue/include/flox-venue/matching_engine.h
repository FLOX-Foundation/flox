/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/engine/clordid_window.h"
#include "flox-venue/engine/expiry.h"
#include "flox-venue/engine/mmp.h"
#include "flox-venue/engine/pegs.h"
#include "flox-venue/engine/sorted_keys.h"
#include "flox-venue/engine/stp.h"
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
  DurationNs lastLookWindowNs{};  // 0 = last look disabled venue-wide
  // How long a client order id stays reserved against reuse. 0 = forever,
  // which is what the venue always did and what it still does unless an
  // operator says otherwise. Past the window an old id is accepted again --
  // exchanges scope client order id uniqueness to the trading day, so that is
  // the honest bound, and it has to be stated rather than discovered.
  int64_t clOrdIdWindowNs{0};
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
//
// This file is the class: nested types, data members, and a declaration for
// every method. The DEFINITIONS live one section per file in engine/*.inl,
// included at the bottom of this header; the `// engine/<name>.inl` markers
// below say which file each run of declarations is defined in. The split is
// layout, not architecture -- the class, its members and its behaviour are
// what they were when all of it was one file.
template <class Book = MatchingBook>
class MatchingEngine
{
 public:
  // engine/dispatch.inl
  MatchingEngine(SymbolConfig cfg, EventSink sink, Book book = Book{},
                 MatchPolicy policy = MatchPolicy::PriceTimeFifo);

  void submit(const InboundCommand& cmd);
  void submit(const InboundCommand& cmd, int64_t tsRawNs);
  void submit(const InboundCommand& cmd, SeqNanos tsNs);
  void tick(int64_t nowRawNs);

  // engine/last_look.inl
  uint64_t openHolds() const noexcept;
  bool hasHold(uint64_t heldId) const;

  template <class Fn>
  void forEachHold(Fn&& fn) const;

  // engine/ledger_fees.inl
  void setFeeSchedule(flox::FeeSchedule fees);

  // engine/quote_mmp.inl
  void setMmp(uint64_t account, Quantity qtyLimit, DurationNs windowNs);

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

  // engine/validate.inl
  void setCreditCheck(CreditCheck c);

  // engine/ledger_fees.inl
  void setLedger(Ledger* ledger, uint64_t venueAccount = 0);

  // engine/clearing.inl
  int64_t positionQty(uint64_t account) const;
  Price positionEntry(uint64_t account) const;

  // engine/dispatch.inl
  uint64_t restingOrderCount() const noexcept;
  SeqNanos engineTime() const noexcept;
  int64_t engineTimeNs() const noexcept;

  // engine/clearing.inl
  Quantity openInterest() const;
  SeqNanos nextFundingNs() const noexcept;
  int64_t fundingIntervalNs() const noexcept;
  int64_t fundingRateRaw() const noexcept;
  void setFundingSchedule(DurationNs intervalNs, SeqNanos nextFundingNs);

  // engine/session.inl
  TradingStatus tradingStatus() const noexcept;
  bool sessionClosed() const noexcept;
  bool delisted() const noexcept;

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

  // engine/ledger_fees.inl
  AccountSnapshot snapshotAccount(uint64_t acct) const;
  Amount totalPositionMargin() const;

  // engine/clearing.inl
  void applyFunding(double rate, Price mark);
  void setMarkPrice(Price mark);
  Amount unrealizedPnlRaw(uint64_t account, Price mark) const;

  // engine/dispatch.inl
  const Book& book() const noexcept;
  uint64_t tradesGenerated() const noexcept;
  const SymbolConfig& config() const noexcept;

  // engine/session.inl
  void setHalted(bool halted);

  // engine/dispatch.inl
  void applyRiskLimits(const SetRiskLimits& r) noexcept;
  SetRiskLimits riskLimits() const noexcept;
  void setLuldBps(int32_t bps) noexcept;
  void setTriggerRef(TriggerRef ref) noexcept;
  void setPriceBand(Price minPrice, Price maxPrice) noexcept;
  void setFatFinger(Quantity maxOrderQty, Volume maxOrderNotional) noexcept;
  void setPositionLimit(Quantity maxPositionQty) noexcept;
  void setMaxOpenOrders(uint32_t n) noexcept;
  void setMarginBps(int32_t initialBps, int32_t maintenanceBps) noexcept;

  // engine/publications.inl
  void setStpGroup(uint64_t account, uint64_t group);

  // engine/validate.inl
  void setAdmissionProfile(uint64_t account, const AdmissionProfile& p);
  const std::unordered_map<uint64_t, AdmissionProfile>& admissionProfiles() const noexcept;
  uint64_t admissionRejects() const noexcept;

  // engine/last_look.inl
  uint64_t skippedLastLookProRata() const noexcept;

  // engine/session.inl
  void haltAndCancelAll();
  void cancelEntireBook(CancelReason reason);

  // ---- session ----
  void closeSession();
  void openSession();
  void delist();
  void relist();

  // ---- session / auctions ----
  void beginPreOpen();
  void onAdmin(AdminAction a);
  void resumeWithAuction();
  void openContinuous();
  void runAuction();

  // ---- checkpoint (journal-format snapshot) ----
  // engine/checkpoint.inl
  uint64_t stateHash() const;
  uint64_t configHash() const;
  void writeSnapshot(Journal& out) const;

  // engine/checkpoint_restore.inl
  bool applySnapshotRecord(const InboundCommand& cmd, int64_t tsNs);

  // engine/checkpoint.inl
  uint64_t droppedSnapshotRecords() const noexcept;

  // engine/ledger_fees.inl
  uint64_t unsettledTrades() const noexcept;

  // engine/last_look.inl
  uint64_t riskRejectedHolds() const noexcept;
  const std::unordered_map<uint64_t, LastLookStats>& lastLookStats() const noexcept;
  uint64_t toleranceRejectedHolds() const noexcept;

  // engine/dispatch.inl
  uint64_t skippedRiskProRata() const noexcept;
  uint64_t fillOrKillPrechecks() const noexcept;
  uint64_t fillOrKillRiskConstrained() const noexcept;
  uint64_t fillOrKillRejected() const noexcept;

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

  // engine/checkpoint.inl
  SnapshotClone cloneForSnapshot(Book emptyBook = Book{}) const;

  // engine/ledger_fees.inl
  Ledger* ledger() const noexcept;
  uint64_t venueAccount() const noexcept;

 private:
  // engine/validate.inl
  RejectReason validateConditional(const NewOrder& o) const;
  RejectReason validate(const NewOrder& o) const;
  RejectReason admissionGate(const NewOrder& o) const;
  bool admissionDenies(uint64_t account, uint8_t bit) const;
  RejectReason perpRiskGate(NewOrder& o);
  int64_t restingReduceOnlyRaw(uint64_t account, Side side, OrderId exclude) const;

  int64_t legFillLimit(uint64_t account, Side side, bool reduceOnly, int64_t want,
                       CancelReason& reason, int64_t posDeltaRaw = 0) const;

  FillLimit fillLimit(const RestingOrder& maker, const NewOrder& taker, Quantity want) const;

  FillLimit fillLimitDry(const RestingOrder& maker, const NewOrder& taker, Quantity want,
                         const PositionDeltas& deltas) const;

  FillLimit pairFillLimit(uint64_t makerAcct, Side makerSide, bool makerReduceOnly,
                          uint64_t takerAcct, Side takerSide, bool takerReduceOnly, Quantity want,
                          int64_t makerPosDeltaRaw = 0, int64_t takerPosDeltaRaw = 0) const;

  bool clOrdIdDuplicate(uint64_t account, uint64_t clOrdId);
  void onNew(NewOrder o, bool clOrdIdChecked = false);
  bool luldBand(int64_t& loRaw, int64_t& hiRaw) const;
  void tripLuldHalt();

  // ---- instrument-wide publications ----
  // engine/publications.inl
  void publishStatus(TradingStatusReason reason);
  void emitStatus(TradingStatus status, TradingStatusReason reason, int64_t untilNs);
  void advanceFundingSchedule();
  void publishDerivatives(Price mark);

  // engine/orders.inl
  bool onStop(const NewOrder& o);
  std::optional<Price> triggerReference() const;
  void processTriggers();
  void onModify(const ModifyOrder& m);
  void onCancel(const CancelOrder& c);
  bool ownershipRefused(OrderId id, uint64_t actor) const;

  // ---- per-account resting-order tracking (mass-cancel / MMP) ----
  // engine/publications.inl
  uint64_t ownerOf(OrderId id) const noexcept;
  STPMode stpOf(OrderId id) const;
  uint64_t stpScope(uint64_t account) const;
  void cancelForStp(OrderId id, uint64_t account);
  void decrementForStp(OrderId id, Quantity by);
  void trackResting(OrderId id, uint64_t account, STPMode stp);
  void forgetOrder(OrderId id);
  void releaseReservationPro(OrderId id, int64_t fromQtyRaw, int64_t toQtyRaw);
  void unlinkOco(OrderId id);
  void cancelAllForAccount(uint64_t account, CancelReason reason = CancelReason::UserRequested);
  void onMassCancel(const MassCancel& mc);

  // ---- two-sided market-maker quote (replace prior bid/ask) ----
  // engine/quote_mmp.inl
  void onQuote(const Quote& q);

  // ---- market-maker protection: pull all quotes on a fill-rate breach ----
  void onTradeObserved(const Trade& t);
  void mmpAdd(uint64_t account, Quantity qty);
  void mmpEnforce();

  // ---- fees ----
  // engine/ledger_fees.inl
  void emitFees(const Trade& t);

  // ---- settlement ledger ----
  struct Reservation
  {
    uint64_t account{};
    AssetId asset{};
    Amount reservedRaw{};
    int64_t limitPriceRaw{};
    Side side{};
  };
  Amount imForRaw(int64_t qtyRaw, int64_t priceRaw) const;
  bool reserveFunds(const NewOrder& o);

  // ---- journaled balance genesis ----
  void onDeposit(const Deposit& d);
  void onWithdraw(const Withdraw& w);
  void emitBalance(uint64_t account, AssetId asset, BalanceReason reason);
  void releaseReservation(OrderId id);
  bool hasHoldsFor(OrderId id) const;
  void releaseReservationExceptHeld(OrderId id);
  void cleanupOrderIfDone(OrderId id);
  void reportUnsettled(const Trade& t, uint64_t account, const char* why);
  void settleTrade(const Trade& t);
  void chargeFee(OrderId id, uint64_t acct, double feeD, bool maker);

  // ---- linear-perp clearing ----
  struct Position
  {
    int64_t qtyRaw{0};    // signed contracts (Quantity raw)
    int64_t entryRaw{0};  // average entry price (Price raw)
    Amount margin{0};     // posted position margin (quote raw), reserved in the ledger
  };

  // engine/clearing.inl
  static int64_t iabs64(int64_t v);
  Amount consumeOrderIM(OrderId orderId, int64_t qtyRaw);
  void releaseOrderIM(OrderId orderId, int64_t qtyRaw, uint64_t acct);
  void onAdjustPosition(const AdjustPosition& a);

  void updatePerpPosition(uint64_t acct, OrderId orderId, bool fillBuy, int64_t qtyRaw,
                          int64_t priceRaw);

  void settlePerp(const Trade& t);
  void onForceClose(const ForceClosePosition& fc);
  void checkLiquidations(Price mark);
  void forceClose(uint64_t acct, Price mark);
  void autoDeleverageEngine(int64_t bankruptSign, Amount deficit, Price mark);

  // ---- checkpoint helpers ----
  // engine/checkpoint.inl
  static uint64_t mixAmount(uint64_t h, Amount a) noexcept;

  template <class Map>
  static std::vector<typename Map::key_type> sortedKeys(const Map& m);

  // engine/checkpoint_restore.inl
  SeqNanos expiryOf(OrderId id) const;
  uint64_t ocoOf(OrderId id) const;
  std::vector<std::pair<NewOrder, Price>> sortedStops() const;
  void linkOco(OrderId id, uint64_t group);
  bool applyRestoreOrder(const RestoreOrder& r);
  bool applyRestoreStop(const RestoreStop& r);
  bool applyRestoreHeld(const RestoreHeld& r);
  bool applyRestoreReservation(const RestoreReservation& r);
  bool applyRestorePosition(const RestorePosition& r);
  bool applySnapshotEnd(const SnapshotEnd& e);

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
    // Captured for the same reason as the rest of this block: a reject rebuilds
    // either leg, and the report that follows has to name the order the way its
    // submitter does.
    uint64_t makerClientOrderId{0};
    uint64_t takerClientOrderId{0};
  };

  // engine/last_look.inl
  void createHeld(const RestingOrder& maker, Quantity fill, const NewOrder& taker);
  void releaseHeldLeg(OrderId id, Quantity qty);
  void resolveHeld(typename std::unordered_map<uint64_t, Held>::iterator it, bool accept);
  void stampFreshHolds();
  int64_t referenceRaw() const;
  int64_t referenceMoveSinceHold(const Held& h) const;
  void recordHoldOutcome(const Held& h, int64_t moveRaw, bool accepted);
  bool holdStillAllowed(const Held& h) const;
  void restoreMakerHeld(const Held& h);
  void restoreTakerHeld(const Held& h);
  void rejectHoldsFor(OrderId id);
  void rejectHoldsForAccount(uint64_t account);
  void rejectAllHolds();
  void onLastLookDecision(const LastLookDecision& d);

  // engine/expiry_pegs.inl
  void expireOrders();
  int64_t pegTargetRaw(Side side, PegRef ref, int64_t offsetRaw) const;
  void repeg();
  void processOco();
  void cancelOcoSibling(OrderId id);

  // engine/last_look.inl
  void expireHolds();

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

  // GTD deadlines: which resting or conditional orders are due, and when.
  ExpiryBook expiry_;

  std::unordered_map<OrderId, uint64_t> orderOco_;  // orderId -> OCO group

  std::unordered_map<uint64_t, std::vector<OrderId>> ocoMembers_;  // group -> member orderIds

  std::vector<std::pair<uint64_t, OrderId>> ocoPending_;  // (group, winner) collected while matching

  // Peg specs and the price each one should track (re-priced each submit).
  PegBook pegs_;

  // Self-trade-prevention modes of resting orders, and the auction verdict.
  // Only the auction uncross and a modify re-entry read the modes: continuous
  // matching takes the mode off the aggressor inside the matcher.
  StpState stp_;

  // account -> admission profile. Empty table and absent entries both mean
  // "everything permitted", so an engine that was never given profiles behaves
  // exactly as before.
  std::unordered_map<uint64_t, AdmissionProfile> admission_;

  bool delisted_{false};  // withdrawn from trading; outranks halt / session / auction

  uint64_t admissionRejects_{0};  // observability: a counterparty sending what it may not

  flox::FeeSchedule fees_;

  bool feesEnabled_{false};

  // Market-maker protection: per-account fill windows and the breach list the
  // submit boundary drains.
  MmpState mmp_;

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

  // clientOrderId dedup index, per account, in two rotating generations (see
  // engine/clordid_window.h). Rebuilt naturally by journal replay.
  ClOrdIdWindow clOrdIds_;

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

// The definitions of everything declared above, one file per section. They are
// included here rather than written inline so that a change to one section is a
// diff in one file; each of them reopens namespace flox::venue.
#define FLOX_VENUE_MATCHING_ENGINE_INL
#include "flox-venue/engine/checkpoint.inl"
#include "flox-venue/engine/checkpoint_restore.inl"
#include "flox-venue/engine/clearing.inl"
#include "flox-venue/engine/dispatch.inl"
#include "flox-venue/engine/expiry_pegs.inl"
#include "flox-venue/engine/last_look.inl"
#include "flox-venue/engine/ledger_fees.inl"
#include "flox-venue/engine/orders.inl"
#include "flox-venue/engine/publications.inl"
#include "flox-venue/engine/quote_mmp.inl"
#include "flox-venue/engine/session.inl"
#include "flox-venue/engine/validate.inl"

#undef FLOX_VENUE_MATCHING_ENGINE_INL
