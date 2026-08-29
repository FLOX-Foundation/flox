/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/messages.h"

#include "flox/book/book_update.h"
#include "flox/book/events/book_update_event.h"
#include "flox/book/nlevel_order_book.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

namespace flox::venue
{

enum class MdType : uint8_t
{
  AddOrder,
  Trade,
  Executed,
  Cancel,
  Replace,
  Triggered,
  // Appended types. The order-level types above map onto SBE template ids
  // 1..6; these do NOT continue that arithmetic (7..13 are the recovery and
  // session verbs), so the codec maps them explicitly.
  TradingStatus,      // halt / LULD pause / auction phase transition
  DerivativesUpdate,  // mark price, funding rate + next funding time, open interest
};

struct MdMessage
{
  MdType type{};
  uint64_t seq{};
  SymbolId symbol{};
  OrderId id{};  // subject order; for Trade this is the taker
  Side side{};
  Price price{};      // DerivativesUpdate: the mark price
  Quantity qty{};     // AddOrder: shown qty; Trade: exec qty; Executed/Replace: leaves;
                      // DerivativesUpdate: open interest
  OrderId makerId{};  // Trade only
  uint64_t epoch{};   // publisher lifetime id; changes on restart (consumer re-snapshots)

  // ---- time (every type, every path) ----
  // When the ENGINE produced the event: the sequencer-ts of the command that
  // caused it (MatchingEngine::engineTimeNs), handed to the publisher by the
  // caller. It is journaled input, so a replay of the same journal produces the
  // same value -- time bars, latency budgets and a faithful tape are all built
  // on this one, and determinism checks may hash it.
  int64_t engineTsNs{};
  // When the PUBLISHER sent this copy: a wall-clock read at send time. It is
  // deliberately NOT reproducible -- a resend of the same message carries a
  // later value than the original -- so it must never enter an event hash or a
  // determinism comparison. Its use is measuring the venue's own outbound
  // latency (sendTsNs - engineTsNs) and the wire delay a consumer sees.
  int64_t sendTsNs{};

  // ---- TradingStatus only ----
  flox::venue::TradingStatus status{};
  TradingStatusReason reason{};
  int64_t untilNs{};  // pause deadline in sequencer time (0 = state has no deadline)

  // ---- DerivativesUpdate only (mark = price, open interest = qty) ----
  int64_t fundingRateRaw{};  // kFundingRateScale, per funding interval
  int64_t nextFundingNs{};   // next funding boundary (0 = no funding schedule)
};

// Consistent (snapshot, lastSeq) pair from MarketDataPublisher::snapshotAtomic:
// apply `orders`, then resume the incremental feed at lastSeq+1 under `epoch`.
//
// The book alone is not a startable state for a late joiner: an instrument that
// is halted, paused or in a pre-open auction looks exactly like a quiet one,
// and a perp position cannot be valued without a mark. So the snapshot carries
// the CURRENT trading status and the current derivatives layer alongside the
// orders -- the same messages the incremental feed publishes on a transition,
// with seq=0 like the rest of the body.
struct MdSnapshot
{
  uint64_t epoch{};
  uint64_t lastSeq{};
  std::vector<MdMessage> orders;  // AddOrder body messages (seq=0, epoch set)
  bool hasStatus{false};
  MdMessage status{};  // current TradingStatus (valid when hasStatus)
  bool hasDerivatives{false};
  MdMessage derivatives{};  // current DerivativesUpdate (valid when hasDerivatives)
};

// One publisher per symbol. Each emitted message carries (symbol, epoch, seq):
// seq is contiguous from 1 within one publisher lifetime, epoch is a random
// value minted at construction. Sequence numbers are deliberately NOT
// persisted across restarts -- the book is rebuildable from matching state, so
// a restart mints a new epoch and starts over at seq=1; a consumer that sees
// the epoch change knows every prior seq is void and re-snapshots via the
// recovery channel (md_recovery.h). Without the epoch a restarted feed would
// look like an endless stream of duplicates to a surviving consumer.
//
// onEvent runs on the matching thread; snapshotAtomic/resendFrom are called
// from recovery-server connection threads. The internal mutex makes the
// (snapshot, lastSeq) pair and the resend ring consistent with the live feed.
template <size_t Levels = (1u << 16)>
class MarketDataPublisher
{
 public:
  using MdSink = std::function<void(const MdMessage&)>;

  // epoch 0 -> mint one; pass a fixed epoch only in tests. resendCapacity
  // bounds the ring of recent increments the recovery channel can replay.
  MarketDataPublisher(MdSink sink, Price tickSize, SymbolId symbol, uint64_t epoch = 0,
                      size_t resendCapacity = 1024)
      : sink_(std::move(sink)),
        symbol_(symbol),
        book_(tickSize),
        epoch_(epoch != 0 ? epoch : makeEpoch()),
        resendCapacity_(resendCapacity)
  {
  }

  // `engineTsNs` is the sequencer-ts the engine applied the causing command at
  // (MatchingEngine::engineTimeNs()). The publisher does NOT invent one: an
  // OutboundEvent carries no time of its own, and a clock read here would make
  // the feed unreproducible on replay -- so the caller, which knows the engine's
  // time, passes it in and every message inherits it verbatim.
  void onEvent(const OutboundEvent& e, int64_t engineTsNs)
  {
    std::lock_guard<std::mutex> lk(m_);
    engineTs_ = engineTsNs;
    if (const auto* x = std::get_if<OrderAccepted>(&e))
    {
      onAccepted(*x);
    }
    else if (const auto* x = std::get_if<Trade>(&e))
    {
      onTrade(*x);
    }
    else if (const auto* x = std::get_if<OrderExecuted>(&e))
    {
      onExecuted(*x);
    }
    else if (const auto* x = std::get_if<OrderCanceled>(&e))
    {
      onCanceled(*x);
    }
    else if (const auto* x = std::get_if<OrderModified>(&e))
    {
      onModified(*x);
    }
    else if (const auto* x = std::get_if<OrderTriggered>(&e))
    {
      onTriggered(*x);
    }
    else if (const auto* x = std::get_if<FillHeld>(&e))
    {
      onFillHeld(*x);
    }
    else if (const auto* x = std::get_if<TradingStatusChanged>(&e))
    {
      onTradingStatus(*x);
    }
    else if (const auto* x = std::get_if<DerivativesUpdated>(&e))
    {
      onDerivatives(*x);
    }
    // FillRejected itself has no direct market impact: when a rejected hold
    // returns quantity to the book the engine emits OrderModified (combine /
    // rebuild at the tail) or OrderAccepted (taker residual rests), which the
    // handlers above apply -- so after any hold/accept/reject sequence the
    // published depth equals the matching book.
    // OrderRejected: no market impact
  }

  // flox NLevelOrderBook: bestBid/bestAsk, bidAtPrice/askAtPrice, consumeAsks/
  // consumeBids (VWAP sweep), isCrossed, spread, mid.
  const flox::NLevelOrderBook<Levels>& book() const noexcept { return book_; }
  uint64_t seq() const
  {
    std::lock_guard<std::mutex> lk(m_);
    return seq_;
  }
  uint64_t epoch() const noexcept { return epoch_; }  // fixed for the publisher lifetime
  SymbolId symbol() const noexcept { return symbol_; }

  // Full book snapshot as AddOrder messages (in-process convenience form of
  // snapshotAtomic; same body, without the (epoch, lastSeq) framing).
  std::vector<MdMessage> snapshot() const { return snapshotAtomic().orders; }

  // Atomic (snapshot, lastSeq) pair, consistent with the emitted stream: no
  // increment with seq <= lastSeq is missing from the snapshot and none with
  // seq > lastSeq is included. A late joiner applies `orders`, then resumes
  // the incremental feed at lastSeq+1.
  MdSnapshot snapshotAtomic() const
  {
    std::lock_guard<std::mutex> lk(m_);
    MdSnapshot s;
    s.epoch = epoch_;
    s.lastSeq = seq_;
    s.orders.reserve(orders_.size());
    const int64_t sendTs = stampSendLocked();
    for (const auto& [id, r] : orders_)
    {
      MdMessage m{MdType::AddOrder, 0, r.symbol, id, r.side, r.price, r.leaves, 0, epoch_};
      // The order's engine time is the one it was accepted at -- the snapshot
      // states when each resting order arrived, not when the snapshot was taken.
      m.engineTsNs = r.engineTsNs;
      m.sendTsNs = sendTs;
      s.orders.push_back(m);
    }
    // Current state, so a late joiner starts halted when the instrument is
    // halted and can value a position from the first message it applies.
    if (statusValid_)
    {
      s.hasStatus = true;
      s.status = status_;
      s.status.seq = 0;
      s.status.sendTsNs = sendTs;
    }
    if (derivativesValid_)
    {
      s.hasDerivatives = true;
      s.derivatives = derivatives_;
      s.derivatives.seq = 0;
      s.derivatives.sendTsNs = sendTs;
    }
    return s;
  }

  // Replay increments with seq >= fromSeq from the bounded ring. nullopt means
  // fromSeq is older than the retained tail (or 0): the caller must serve a
  // snapshot instead. fromSeq past the head returns an empty replay.
  std::optional<std::vector<MdMessage>> resendFrom(uint64_t fromSeq) const
  {
    std::lock_guard<std::mutex> lk(m_);
    if (fromSeq > seq_)
    {
      return std::vector<MdMessage>{};
    }
    if (ring_.empty() || fromSeq < ring_.front().seq)
    {
      return std::nullopt;
    }
    std::vector<MdMessage> out;
    const int64_t sendTs = stampSendLocked();
    for (const auto& m : ring_)
    {
      if (m.seq >= fromSeq)
      {
        out.push_back(m);
        // A replayed copy is being sent NOW: engineTsNs is the event's own time
        // and never changes, sendTsNs is this transmission's. That asymmetry is
        // the point -- it is what lets a consumer tell a replay from live flow.
        out.back().sendTsNs = sendTs;
      }
    }
    return out;
  }

 private:
  // Wall clock for sendTsNs, latched non-decreasing: a step backwards in the
  // system clock must not make an already-sent message look newer than one that
  // follows it. Callers hold m_.
  int64_t stampSendLocked() const
  {
    const int64_t now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    lastSend_ = now > lastSend_ ? now : lastSend_;
    return lastSend_;
  }

  static uint64_t makeEpoch()
  {
    std::random_device rd;
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    const uint64_t e = ((static_cast<uint64_t>(rd()) << 32) | rd()) ^ now;
    return e != 0 ? e : 1;
  }
  struct Resting
  {
    SymbolId symbol{};
    Side side{};
    Price price{};
    Quantity leaves{};
    int64_t engineTsNs{};  // engine time the order was accepted at (snapshot bodies carry it)
  };

  Quantity atPrice(Side s, Price p) const
  {
    return s == Side::BUY ? book_.bidAtPrice(p) : book_.askAtPrice(p);
  }

  // Apply a single absolute-qty level delta to the flox depth book.
  void applyLevel(Side s, Price p, Quantity newAggregate)
  {
    ev_.clear();
    ev_.update.type = flox::BookUpdateType::DELTA;
    ev_.update.symbol = symbol_;
    if (s == Side::BUY)
    {
      ev_.update.bids.emplace_back(p, newAggregate);
    }
    else
    {
      ev_.update.asks.emplace_back(p, newAggregate);
    }
    book_.applyBookUpdate(ev_);
  }

  void emit(MdMessage m)
  {
    m.seq = ++seq_;
    m.epoch = epoch_;
    m.engineTsNs = engineTs_;
    m.sendTsNs = stampSendLocked();
    ring_.push_back(m);
    if (ring_.size() > resendCapacity_)
    {
      ring_.pop_front();
    }
    sink_(m);
  }

  void onTradingStatus(const TradingStatusChanged& s)
  {
    MdMessage m{};
    m.type = MdType::TradingStatus;
    m.symbol = s.symbol;
    m.status = s.status;
    m.reason = s.reason;
    m.untilNs = s.untilNs;
    emit(m);
    // Remember it AFTER emit stamped seq/epoch/time, so the snapshot copy is
    // the message the incremental feed actually published.
    status_ = ring_.back();
    statusValid_ = true;
  }

  void onDerivatives(const DerivativesUpdated& d)
  {
    MdMessage m{};
    m.type = MdType::DerivativesUpdate;
    m.symbol = d.symbol;
    m.price = d.mark;
    m.qty = d.openInterest;
    m.fundingRateRaw = d.fundingRateRaw;
    m.nextFundingNs = d.nextFundingNs.raw();  // MD wire: raw ns
    emit(m);
    derivatives_ = ring_.back();
    derivativesValid_ = true;
  }

  void onAccepted(const OrderAccepted& a)
  {
    if (!a.restingOnBook)
    {
      return;  // pending stop: not on the visible book
    }
    // Public depth shows only the displayed size; an iceberg's hidden reserve is
    // never leaked (displayQty carries the peak, 0 -> non-iceberg, use leavesQty).
    const Quantity shown = a.displayQty.raw() > 0 ? a.displayQty : a.leavesQty;
    orders_[a.id] = Resting{a.symbol, a.side, a.price, shown, engineTs_};
    applyLevel(a.side, a.price, atPrice(a.side, a.price) + shown);
    emit(MdMessage{MdType::AddOrder, 0, a.symbol, a.id, a.side, a.price, shown, 0});
  }

  void onTrade(const Trade& t)
  {
    // Depth reduction is done via the maker's OrderExecuted; the print itself
    // does not touch depth (avoids double counting).
    emit(MdMessage{MdType::Trade, 0, t.symbol, t.takerId, t.takerSide, t.price, t.quantity,
                   t.makerId});
  }

  void onExecuted(const OrderExecuted& x)
  {
    auto it = orders_.find(x.id);
    if (it == orders_.end())
    {
      return;  // taker leg (not resting) -- ignore
    }
    Resting& r = it->second;
    // Use the DISPLAYED remaining, never the whole (peak+hidden) leavesQty -- else
    // an iceberg's hidden reserve leaks onto the public feed and a partial peak
    // fill (leavesQty > displayed) would fail to reduce depth / underflow the
    // level. r.leaves tracks the shown size; drive it from displayLeaves.
    const Quantity shownAfter = x.displayLeaves;
    const int64_t levelDelta = shownAfter.raw() - r.leaves.raw();
    if (levelDelta != 0)
    {
      applyLevel(r.side, r.price, Quantity::fromRaw(atPrice(r.side, r.price).raw() + levelDelta));
    }
    r.leaves = shownAfter;
    emit(MdMessage{MdType::Executed, 0, r.symbol, x.id, r.side, r.price, shownAfter, 0});
    if (x.complete)
    {
      orders_.erase(it);
    }
  }

  void onCanceled(const OrderCanceled& c)
  {
    auto it = orders_.find(c.id);
    if (it == orders_.end())
    {
      return;  // pending stop / unknown
    }
    const Resting r = it->second;
    applyLevel(r.side, r.price, atPrice(r.side, r.price) - r.leaves);
    orders_.erase(it);
    emit(MdMessage{MdType::Cancel, 0, r.symbol, c.id, r.side, r.price, r.leaves, 0});
  }

  void onModified(const OrderModified& m)
  {
    auto it = orders_.find(m.id);
    if (it == orders_.end())
    {
      return;
    }
    Resting& r = it->second;
    applyLevel(r.side, r.price, atPrice(r.side, r.price) - r.leaves);  // remove old
    r.price = m.price;
    r.leaves = m.leavesQty;
    applyLevel(r.side, r.price, atPrice(r.side, r.price) + m.leavesQty);  // add new
    emit(MdMessage{MdType::Replace, 0, r.symbol, m.id, r.side, m.price, m.leavesQty, 0});
  }

  void onTriggered(const OrderTriggered& t)
  {
    emit(MdMessage{MdType::Triggered, 0, t.symbol, t.id, Side::BUY, t.refPrice, Quantity{}, 0});
  }

  // Last-look hold: the held qty is reserved OUT of the matching book, so the
  // public level must shrink now -- otherwise the feed advertises size that is
  // not there (a phantom that would live forever after a reject). The entry is
  // kept even at zero displayed size: the id is still live (a reject restores
  // via OrderModified, an accept completes via OrderExecuted).
  void onFillHeld(const FillHeld& f)
  {
    auto it = orders_.find(f.makerId);
    if (it == orders_.end())
    {
      return;
    }
    Resting& r = it->second;
    const Quantity shownAfter = f.makerDisplayAfter;
    const int64_t levelDelta = shownAfter.raw() - r.leaves.raw();
    if (levelDelta != 0)
    {
      applyLevel(r.side, r.price, Quantity::fromRaw(atPrice(r.side, r.price).raw() + levelDelta));
    }
    r.leaves = shownAfter;
    emit(MdMessage{MdType::Executed, 0, r.symbol, f.makerId, r.side, r.price, shownAfter, 0});
  }

  MdSink sink_;
  SymbolId symbol_{};
  flox::NLevelOrderBook<Levels> book_;
  std::pmr::memory_resource* res_ = std::pmr::new_delete_resource();
  flox::BookUpdateEvent ev_{res_};
  std::unordered_map<OrderId, Resting> orders_;
  uint64_t seq_{0};
  uint64_t epoch_{};
  size_t resendCapacity_{};
  std::deque<MdMessage> ring_;   // recent increments (seq embedded) for resendFrom
  int64_t engineTs_{0};          // engine time of the event being processed
  mutable int64_t lastSend_{0};  // latched wall clock, so sendTsNs never goes backwards
  // Last published instrument state, replayed into every snapshot so a late
  // joiner starts with the real status and mark instead of a blank one.
  MdMessage status_{};
  bool statusValid_{false};
  MdMessage derivatives_{};
  bool derivativesValid_{false};
  mutable std::mutex m_;  // matching thread vs recovery-server threads
};

}  // namespace flox::venue
