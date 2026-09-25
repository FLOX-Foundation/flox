/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/engine/clearing.h"
#include "flox-venue/engine/clordid_window.h"
#include "flox-venue/engine/credit.h"
#include "flox-venue/engine/expiry.h"
#include "flox-venue/engine/fees.h"
#include "flox-venue/engine/integrity.h"
#include "flox-venue/engine/last_look.h"
#include "flox-venue/engine/mmp.h"
#include "flox-venue/engine/oco.h"
#include "flox-venue/engine/pegs.h"
#include "flox-venue/engine/publications.h"
#include "flox-venue/engine/quote.h"
#include "flox-venue/engine/session.h"
#include "flox-venue/engine/sorted_keys.h"
#include "flox-venue/engine/state_hash_tags.h"
#include "flox-venue/engine/stp.h"
#include "flox-venue/event_hash.h"
#include "flox-venue/journal.h"
#include "flox-venue/ledger.h"
#include "flox-venue/matcher.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/messages.h"
#include "flox-venue/stop_book.h"
#include "flox-venue/symbol_config.h"
#include "flox/book/resting_order.h"

#include "flox/clearing/fee_schedule.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace flox::venue
{

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

  // The pre-trade credit gate's types live with the component that asks the
  // question (engine/credit.h). Named here too because that is how every
  // caller spells them and how the written-down surface pins them.
  using CreditRequest = flox::venue::CreditRequest;
  using CreditDecision = flox::venue::CreditDecision;
  using CreditCheck = flox::venue::CreditCheck;

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
    // T059: RestingOrder::cumQty as it stands now, so a reconnect resync's
    // synthesized OrderAccepted (session_verbs.h) reports the real running
    // total instead of always 0.
    Quantity cumQty{};
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
  // Pre-start wiring only; on a running engine submit the SetAccountRiskLimits
  // command, which is sequenced, journaled and replayed (W26-T064).
  void setAccountRiskLimits(const SetAccountRiskLimits& r) { credit_.setAccountLimits(r); }
  const engine::Credit::AccountLimits* accountRiskLimits(uint64_t account) const noexcept
  {
    return credit_.accountLimits(account);
  }
  // A SNAPSHOT, by value: this is the /metrics thread's accessor (see
  // engine/snapshot_lock.h and docs/venue/perimeter.md).
  std::unordered_map<uint64_t, AdmissionProfile> admissionProfiles() const;
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
  // A SNAPSHOT, by value: this is the /metrics thread's accessor (see
  // engine/snapshot_lock.h and docs/venue/perimeter.md).
  std::unordered_map<uint64_t, LastLookStats> lastLookStats() const;
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
  RejectReason instrumentStateRefusal() const;
  RejectReason validateConditional(const NewOrder& o) const;
  RejectReason validate(const NewOrder& o) const;
  RejectReason admissionGate(const NewOrder& o) const;
  bool admissionDenies(uint64_t account, uint8_t bit) const;
  RejectReason perpRiskGate(NewOrder& o);
  // The one door onto the book. Every path that leaves an order resting goes
  // through here, so no path can forget to ask whether the book took it:
  // RejectReason::None means it is resting, anything else means it is on no
  // book at all and the caller owes its owner a report. A bounded book
  // (LadderBook) refuses an out-of-band price and an exhausted pool; the
  // reference book never refuses.
  RejectReason restOnBook(Side side, const RestingOrder& ro);
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

  // ---- session ----
  // engine/session.inl
  //
  // One row of engine/session.h's transition table, plus the two things the
  // session cannot do for itself: reaching the feed, and reaching the book.
  void applySession(engine::SessionEvent e, SeqNanos deadline = SeqNanos{});
  void publishStatus(TradingStatusReason reason);
  void emitStatus(TradingStatus status, TradingStatusReason reason, int64_t untilNs);
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
  void cancelAllForAccount(uint64_t account, CancelReason reason = CancelReason::UserRequested);
  void onMassCancel(const MassCancel& mc);

  // ---- two-sided market-maker quote (replace prior bid/ask) ----
  // engine/quote_mmp.inl
  void onQuote(const Quote& q);
  // The same set of levels in one command. Walks the quote path once per rung,
  // so it is K Quotes by construction rather than by resemblance.
  void onQuoteLadder(const QuoteLadder& l);
  void applyQuote(const Quote& q, bool clOrdIdChecked);

  // ---- market-maker protection: pull all quotes on a fill-rate breach ----
  void onTradeObserved(const Trade& t);
  void mmpAdd(uint64_t account, Quantity qty);
  void mmpEnforce();

  // ---- settlement ledger ----
  // One reservation entry, named where it always was so the checkpoint and
  // the restore path keep spelling it the same way. The table itself, and
  // every rule about what goes into it, belong to engine::Credit.
  using Reservation = engine::Credit::Reservation;

  // Thin forwards into engine::Credit, kept as members because the paths that
  // call them (clearing, settlement, last look, cancel) ask about buying power
  // without needing to know where it is kept. Each is one call; nothing here
  // decides anything.
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
  void settleTrade(const Trade& t);

  // ---- linear-perp clearing ----
  //
  // The positions, the funding calendar and everything that moves money on
  // them live in engine::Clearing (clearing_, below). What is left here is
  // the part that is not clearing: the order reservations a fill's initial
  // margin comes out of, the fee charge on a perp print, and the published
  // methods, which stay on the engine as thin delegates.

  // engine/clearing.inl
  static int64_t iabs64(int64_t v);
  Amount consumeOrderIM(OrderId orderId, int64_t qtyRaw);
  void releaseOrderIM(OrderId orderId, int64_t qtyRaw, uint64_t acct);
  void onAdjustPosition(const AdjustPosition& a);

  void settlePerp(const Trade& t);
  void onForceClose(const ForceClosePosition& fc);

  // ---- checkpoint helpers ----
  // engine/checkpoint.inl
  static uint64_t mixAmount(uint64_t h, Amount a) noexcept;

  template <class Map>
  static std::vector<typename Map::key_type> sortedKeys(const Map& m);

  // The remainder the checkpoint cannot delegate: the book, the stop book,
  // the instrument's own config records and the bound ledger belong to no
  // engine/*.h component, so the engine still walks them itself. A pair per
  // direction, because the remainder is not contiguous in the file -- the
  // config records lead it, the book sits in the middle, the balances close
  // it, and the order IS the format.
  using BalanceRow = std::tuple<uint64_t, AssetId, Amount, Amount>;
  std::vector<BalanceRow> sortedBalances() const;
  uint64_t hashBookAndStops(uint64_t h) const;
  uint64_t hashBalances(uint64_t h) const;
  void writeConfigSection(Journal& out, int64_t ts) const;
  void writeBookAndStops(Journal& out, int64_t ts) const;
  void writeBalances(Journal& out, int64_t ts) const;

  // engine/checkpoint_restore.inl
  SeqNanos expiryOf(OrderId id) const;
  std::vector<std::pair<NewOrder, Price>> sortedStops() const;
  bool applySnapshotBegin(const SnapshotBegin& b);
  std::optional<bool> applyComponentRestore(const InboundCommand& cmd);
  bool applyRestoreOrder(const RestoreOrder& r);
  bool applyRestoreStop(const RestoreStop& r);
  bool applyRestoreBalance(const RestoreBalance& r);
  bool applyRestoreHeld(const RestoreHeld& r);
  bool applyRestoreReservation(const RestoreReservation& r);
  bool applyRestorePosition(const RestorePosition& r);
  bool applySnapshotEnd(const SnapshotEnd& e);

  // ---- last look ----
  // The holds themselves live in engine::LastLook (engine/last_look.h), which
  // is NOT a template: a hold reaches the book three times and every one of
  // those is on the decision path, which runs at maker latency rather than at
  // matching latency. Held is that component's record, named here so the
  // checkpoint and the restore path keep spelling it the way they did.
  using Held = engine::Held;

  // engine/last_look.inl
  // This engine's model of engine::LastLookHost: the three book operations,
  // the event sink in its two flavours, and the collaborations a resolution
  // triggers. Duck-typed rather than derived -- the concept says why.
  class LastLookHost
  {
   public:
    explicit LastLookHost(MatchingEngine& e) noexcept : e_(e) {}

    const RestingOrder* findResting(OrderId id) const;
    std::optional<RestingOrder> takeResting(OrderId id);
    bool reinsertTail(Side side, const RestingOrder& o);
    void publish(const OutboundEvent& ev);
    void publishTracked(const OutboundEvent& ev);
    engine::LastLookConfig lastLookConfig() const;
    int64_t referenceRaw() const;
    bool holdStillAllowed(const Held& h) const;
    uint64_t nextTradeSeq();
    void releaseHeldLeg(OrderId id, Quantity qty);
    void cleanupOrderIfDone(OrderId id);
    void rememberStp(OrderId id, STPMode stp);
    void adoptRestingTaker(const Held& h);

   private:
    MatchingEngine& e_;
  };

  void createHeld(const RestingOrder& maker, Quantity fill, const NewOrder& taker,
                  Quantity takerCumSoFar);
  void releaseHeldLeg(OrderId id, Quantity qty);
  void stampFreshHolds();
  int64_t referenceRaw() const;
  bool holdStillAllowed(const Held& h) const;
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

  Price markPrice_{};

  bool hasMark_{false};

  uint64_t tradeSeq_{0};

  SeqNanos now_{};

  int64_t timeCounter_{0};

  // The outbound stream and the per-account resting-order index (orderAccount_,
  // byAccount_ and orderStp_ used to sit right here). Holds a reference to
  // sink_, so it must be declared after it -- anywhere after it will do, and
  // this is where the members it took over were.
  engine::Publications pub_;

  // GTD deadlines: which resting or conditional orders are due, and when.
  ExpiryBook expiry_;

  // One-cancels-the-other: group membership in both directions, and the
  // groups a fill decided this submit. Exactly the three containers that used
  // to sit here, in the same order, so the class is laid out as it was.
  engine::OcoBook oco_;

  // Peg specs and the price each one should track (re-priced each submit).
  PegBook pegs_;

  // Self-trade-prevention modes of resting orders, and the auction verdict.
  // Only the auction uncross and a modify re-entry read the modes: continuous
  // matching takes the mode off the aggressor inside the matcher.
  StpState stp_;

  // The fee schedule and what a print costs under it. Configuration, like the
  // credit hook: installed by the embedder, never journaled or hashed.
  engine::Fees fees_;

  // Market-maker protection: per-account fill windows and the breach list the
  // submit boundary drains.
  MmpState mmp_;

  // Who may send an order and what it costs to have one live: the admission
  // table, the external credit hook and the buying-power reservations. Held by
  // value and called directly from the hot path, so every gate inlines exactly
  // as it did when these were loose members of this class.
  engine::Credit credit_;

  // Holds nothing but a reference, so it may be declared here and still be
  // handed `*this` -- and a snapshot clone's host names the clone, because
  // each engine constructs its own.
  LastLookHost lastLookHost_{*this};

  engine::LastLook lastLook_;

  // clientOrderId dedup index, per account, in two rotating generations (see
  // engine/clordid_window.h). Rebuilt naturally by journal replay.
  ClOrdIdWindow clOrdIds_;

  // The two refusals that are not supposed to happen: snapshot-only records
  // seen on the live submit path, and trades left unsettled rather than
  // settled by creating value. Diagnostics, not hashed state. The last-look
  // sibling of the second -- accepts refused at decision time by a perp risk
  // limit -- is counted inside engine::LastLook.
  engine::Integrity integrity_;

  // Recovery mode flag: this snapshot carried exact RestoreBalance splits, so
  // RestoreReservation / RestorePosition must not move ledger money (v1
  // Deposit-total snapshots leave it false and keep the re-reservation path).
  bool exactBalanceRestore_{false};

  Ledger* ledger_{nullptr};

  uint64_t venueAccount_{0};

  // Positions, the funding calendar and the money that moves on them. Bound
  // to this engine's cfg_ and sink_ (both declared above it, so the binding
  // is valid) and to the three pieces of engine-side work clearing delegates
  // back. Its ledger pointer is a mirror of ledger_/venueAccount_, set in the
  // one place either is set: setLedger.
  engine::Clearing clearing_;

  // Trading status, halt, the session boundary, delisting and the auction
  // phase: the state, the automaton that moves it, and the memo of what was
  // last published. A non-template component (it touches no book), so the
  // automaton exists once however many book types the engine is instantiated
  // with -- see engine/session.h.
  engine::Session session_;
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
