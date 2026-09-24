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
#include "flox-venue/journal.h"
#include "flox-venue/messages.h"

#include "flox/book/resting_order.h"

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace flox::venue::engine
{

// One open last-look hold: a fill that has been taken out of the book but not
// yet printed, waiting for the maker to confirm or refuse it.
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
  // T059: each leg's CONFIRMED cumulative fill as of the moment this hold
  // opened -- the maker's RestingOrder::cumQty and the taker's running total
  // within its own crossing sweep, both read BEFORE this hold's own qty is
  // reserved out of the book (create()). FillHeld/FillRejected report the
  // taker's value as FIX 14 (CumQty); both values are also what a reject
  // that rebuilds a leg fully off the book restores onto it, since the
  // book's own real-fill bookkeeping (fillBest/consumeById) optimistically
  // bumps RestingOrder::cumQty at hold-creation time, before the maker's
  // decision is known.
  Quantity makerCumQtyAtHold{};
  Quantity takerCumQtyAtHold{};
};

// The venue knobs a hold reads. Fetched from the host on use rather than
// copied when the hold opens: they are live config, and a hold outlives the
// command that opened it.
struct LastLookConfig
{
  SymbolId symbol{};
  DurationNs window{};          // 0 = last look disabled venue-wide
  int64_t toleranceRaw{0};      // 0 = the venue imposes none of its own
  bool acceptOnTimeout{false};  // what an unanswered hold becomes
};

// The seam between the holds and the venue around them: three book operations
// (lift an order off its level, put one back at the TAIL, read a maker as it
// rests), the event sink in its two flavours, and the handful of facts and
// registrations a resolution triggers.
//
// It is a CONCEPT and not an abstract class on purpose. The seam was written
// both ways and measured over 100k hold/resolve cycles: the abstract-class
// version costs about a dozen indirect calls per cycle, which came to +3.3%
// to +4.7% on the accept path -- past the 3% this decomposition allows
// itself. Duck-typed, every call inlines and the difference disappears. The
// price is that the host arrives as an argument rather than a member, which
// is why LastLook stays a plain class holding plain state.
template <class H>
concept LastLookHost =
    requires(H& h, const H& ch, OrderId id, Side side, const RestingOrder& order,
             const OutboundEvent& ev, const Held& held, Quantity q, STPMode stp) {
      // The maker as it rests now; null when the hold took its whole
      // displayed size and the order left the book.
      { ch.findResting(id) } -> std::same_as<const RestingOrder*>;
      // Lift an order off its level whole (nullopt: it is not there).
      { h.takeResting(id) } -> std::same_as<std::optional<RestingOrder>>;
      // Put one back, at the TAIL of its level -- as if freshly entered. The
      // refused quantity loses its queue position; see docs/venue/matching.md.
      // False: the book would not take it (no room, or a price with no level),
      // and the caller owes its owner a cancel rather than a silent loss.
      { h.reinsertTail(side, order) } -> std::same_as<bool>;
      // Reports about holds and about the legs a reject restores.
      h.publish(ev);
      // The sink the engine wraps to track its own last price -- a printed
      // trade and the executions that belong to it go through this one.
      h.publishTracked(ev);
      { ch.lastLookConfig() } -> std::same_as<LastLookConfig>;
      // Where the market is, for judging a held fill: the mid when both sides
      // are quoted, the last print when they are not, 0 when neither exists.
      { ch.referenceRaw() } -> std::same_as<int64_t>;
      // Would settling this hold in full still pass the perp risk limits?
      { ch.holdStillAllowed(held) } -> std::same_as<bool>;
      { h.nextTradeSeq() } -> std::same_as<uint64_t>;
      // Give back the buying power reserved for a held quantity that will not
      // trade.
      h.releaseHeldLeg(id, q);
      // After the last hold on a leg resolves: free what is left of its
      // reservation and drop its tracking, if it no longer rests anywhere.
      h.cleanupOrderIfDone(id);
      // Remember an aggressor's STP mode: its residual may rest once the hold
      // resolves, and a resting order's mode is what an auction reads.
      h.rememberStp(id, stp);
      // A refused taker residual rests again under its own id: re-register
      // its per-account tracking and, for a GTD order, its deadline.
      h.adoptRestingTaker(held);
    };

// Holds, their decisions and their outcomes -- the whole last-look section of
// the matching engine, and nothing else.
//
// It is NOT a template, and neither is the state it holds. A hold reaches the
// resting book three times and every one of those is on the decision path,
// which runs at maker latency rather than at matching latency -- so the venue
// around it arrives through LastLookHost, satisfied by the engine's template
// layer (see engine/last_look.inl) and, in the unit test, by fifty lines of
// std::vector.
class LastLook
{
 public:
  using Config = LastLookConfig;

  LastLook() = default;

  LastLook(const LastLook&) = delete;
  LastLook& operator=(const LastLook&) = delete;

  // ---- what the outside may ask ----------------------------------------

  // Approximate cross-thread gauge for the idle sweeper.
  uint64_t openCount() const noexcept { return open_.load(std::memory_order_relaxed); }

  // Unlike openCount(), which reads an atomic mirror, this is backed directly
  // by the (unsynchronized) hold table: call only from the engine's thread.
  bool has(uint64_t heldId) const { return held_.contains(heldId); }

  // Enumerate every open hold as fn(const Held&), in unspecified order.
  template <class Fn>
  void forEach(Fn&& fn) const
  {
    // order: not observable -- forEach reports an identity SET (a caller
    // restoring its own holds checks membership, not sequence)
    for (const auto& [hid, h] : held_)
    {
      (void)hid;
      fn(h);
    }
  }

  // Any open hold naming this order as maker or taker? An id under a live
  // hold is still live even when the order is fully out of the book.
  bool referencesOrder(OrderId id) const
  {
    if (held_.empty())
    {
      return false;
    }
    // order: not observable -- a predicate scan, first match wins
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

  // How much of this order's quantity is sitting in open holds -- the slice
  // whose reservation must survive a cancel of the rest.
  Quantity heldQtyFor(OrderId id) const
  {
    Quantity total{};
    // order: not observable -- Quantity sum of the held slices
    for (const auto& [hid, h] : held_)
    {
      (void)hid;
      if (h.maker == id || h.taker == id)
      {
        total += h.qty;
      }
    }
    return total;
  }

  // Per-maker last-look conduct.
  //
  // The reject RATE on its own says nothing: a maker with a wide tolerance and
  // a maker cherry-picking its fills can post the same number. What separates
  // them is which way the price had moved when they refused. A maker applying a
  // symmetric rule refuses roughly as often when the move favoured it as when
  // it did not; one taking the free option refuses almost only when it was
  // losing. That single split is what makes the behaviour visible without
  // anyone having to see the maker's code.
  const std::unordered_map<uint64_t, LastLookStats>& stats() const noexcept { return stats_; }

  // Last-look accepts turned into rejects because the fill would have breached
  // a perp risk limit by the time the maker answered (see resolve()).
  uint64_t riskRejected() const noexcept { return riskRejected_; }

  // Holds refused by the venue's own tolerance rather than by the maker.
  uint64_t toleranceRejected() const noexcept { return toleranceRejected_; }

  // ---- the hold lifecycle ----------------------------------------------

  template <LastLookHost Host>
  void create(Host& host, const RestingOrder& maker, Quantity fill, const NewOrder& taker,
              SeqNanos now, Quantity takerCumSoFar = {})
  {
    const Config cfg = host.lastLookConfig();
    const uint64_t id = ++seq_;
    Held h{id,
           taker.id,
           taker.accountId,
           taker.side,
           maker.id,
           maker.accountId,
           maker.price,
           fill,
           now + cfg.window,
           // Stamped once the matching pass finishes, not here. See
           // stampFresh().
           0};
    // The taker is an aggressor now, but its residual may rest once the hold
    // resolves -- and a resting order's STP mode is what an auction reads.
    // Capture it here, where the mode is still in hand.
    if (taker.stp != STPMode::None)
    {
      host.rememberStp(taker.id, taker.stp);
    }
    h.takerTif = taker.tif;
    h.takerType = taker.type;
    h.takerPrice = taker.price;
    h.takerExpiryNs = taker.expiryNs;
    h.makerReduceOnly = maker.reduceOnly;
    h.makerClientOrderId = maker.clientOrderId;
    h.takerClientOrderId = taker.clientOrderId;
    h.takerReduceOnly = taker.reduceOnly;
    // T059: each leg's confirmed cumQty as of right now -- maker.cumQty is
    // the RestingOrder's running total BEFORE this fill (the book has not
    // mutated it yet; that happens in the caller, after this call returns),
    // and takerCumSoFar is the caller's own running total for this sweep.
    h.makerCumQtyAtHold = maker.cumQty;
    h.takerCumQtyAtHold = takerCumSoFar;
    held_[id] = h;
    fresh_.push_back(id);
    open_.store(held_.size(), std::memory_order_relaxed);
    // Called BEFORE the matcher reserves the qty out of the book, so the
    // maker's post-hold displayed size is computed here the same way a normal
    // fill would (partial peak -> remainder shown; full peak -> iceberg refill).
    const Quantity displayAfter =
        (fill < maker.leaves) ? (maker.leaves - fill)
                              : ((maker.peak < maker.hidden) ? maker.peak : maker.hidden);
    host.publish(FillHeld{id, cfg.symbol, maker.id, taker.id, maker.price, fill, displayAfter,
                          maker.accountId, taker.accountId, h.takerSide, taker.clientOrderId,
                          h.takerCumQtyAtHold});
    // NOTE: the maker stays tracked (orderAccount_/byAccount_) even when the
    // hold empties its displayed size and fillBest removes it from the book --
    // the id is still live (a reject restores it) and mass-cancel paths must
    // still find it.
  }

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
  template <LastLookHost Host>
  void stampFresh(Host& host)
  {
    if (fresh_.empty())
    {
      return;
    }
    const int64_t ref = host.referenceRaw();
    for (uint64_t id : fresh_)
    {
      auto it = held_.find(id);
      if (it != held_.end())
      {
        it->second.refAtHoldRaw = ref;
      }
    }
    fresh_.clear();
  }

  template <LastLookHost Host>
  void onDecision(Host& host, const LastLookDecision& d)
  {
    auto it = held_.find(d.heldId);
    if (it == held_.end())
    {
      host.publish(
          OrderRejected{d.heldId, host.lastLookConfig().symbol, RejectReason::UnknownOrder,
                        d.accountId});
      return;
    }
    // Ownership: only the maker whose quote is held may decide its fate.
    if (d.accountId != it->second.makerAccount)
    {
      host.publish(
          OrderRejected{d.heldId, host.lastLookConfig().symbol, RejectReason::NotOrderOwner,
                        d.accountId});
      return;
    }
    resolve(host, it, d.accept);
  }

  template <LastLookHost Host>
  void expire(Host& host, SeqNanos now)
  {
    if (held_.empty())
    {
      return;
    }
    std::vector<uint64_t> due;
    // order: the due holds are id-sorted below -- a timeout-accept assigns
    // the next trade sequence, so the resolution order feeds the event stream
    for (const auto& [id, h] : held_)
    {
      if (now >= h.deadline)
      {
        due.push_back(id);
      }
    }
    // Deterministic order: a timeout-accept assigns the next trade sequence, so
    // the resolution order feeds the event stream -- must not depend on the
    // hold table's (unordered_map) layout.
    std::sort(due.begin(), due.end());
    const bool accept = host.lastLookConfig().acceptOnTimeout;
    for (uint64_t id : due)
    {
      auto it = held_.find(id);
      if (it != held_.end())
      {
        resolve(host, it, accept);
      }
    }
  }

  // Deterministically resolve (reject) every open hold that references `id` as
  // maker or taker. MUST run before any path that permanently removes the order
  // or reshapes its reservation (cancel/modify/quote-replace/expiry/OCO/peg):
  // a hold left behind would let a later accept settle with no backing
  // reservation (unchecked debit -> conservation breach).
  template <LastLookHost Host>
  void rejectFor(Host& host, OrderId id)
  {
    if (held_.empty())
    {
      return;
    }
    std::vector<uint64_t> due;
    // order: the due holds are id-sorted below, before any is resolved
    for (const auto& [hid, h] : held_)
    {
      if (h.maker == id || h.taker == id)
      {
        due.push_back(hid);
      }
    }
    rejectAllOf(host, due);
  }

  // Account-scope variant for mass-cancel / MMP / liquidation.
  template <LastLookHost Host>
  void rejectForAccount(Host& host, uint64_t account)
  {
    if (held_.empty())
    {
      return;
    }
    std::vector<uint64_t> due;
    // order: the due holds are id-sorted below, before any is resolved
    for (const auto& [hid, h] : held_)
    {
      if (h.makerAccount == account || h.takerAccount == account)
      {
        due.push_back(hid);
      }
    }
    rejectAllOf(host, due);
  }

  template <LastLookHost Host>
  void rejectAll(Host& host)
  {
    if (held_.empty())
    {
      return;
    }
    std::vector<uint64_t> due;
    due.reserve(held_.size());
    // order: the due holds are id-sorted below, before any is resolved
    for (const auto& [hid, h] : held_)
    {
      (void)h;
      due.push_back(hid);
    }
    rejectAllOf(host, due);
  }

  // ---- checkpoint -------------------------------------------------------

  uint64_t seq() const noexcept { return seq_; }
  void setSeq(uint64_t v) noexcept { seq_ = v; }

  // Hold ids in ascending order: what the state hash and the snapshot walk.
  std::vector<uint64_t> sortedIds() const
  {
    std::vector<uint64_t> ids;
    ids.reserve(held_.size());
    // order: sorted below, before the vector is handed out
    for (const auto& [hid, h] : held_)
    {
      (void)h;
      ids.push_back(hid);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
  }

  const Held& at(uint64_t heldId) const { return held_.at(heldId); }

  void insertRestored(const Held& h)
  {
    held_[h.id] = h;
    open_.store(held_.size(), std::memory_order_relaxed);
  }

  // The hold table as the snapshot clone needs it: the holds themselves and
  // the id sequence, and nothing else. The statistics and the reject counters
  // are diagnostics of the LIVE engine -- they are neither hashed nor written,
  // so the clone starts them at zero exactly as it did before.
  void copyHoldsFrom(const LastLook& other)
  {
    held_ = other.held_;
    seq_ = other.seq_;
    open_.store(held_.size(), std::memory_order_relaxed);
  }

  // The holds in the state hash, in id order. `tracked(orderId)` answers the
  // one thing a hold carries that this component cannot see: whether the
  // maker is still in the engine's resting-order index (a held maker stays
  // tracked even fully off the book -- see createHeld and
  // RestoreHeld::makerTracked). Duck-typed like the Host, so the hold table
  // stays free of the publication side.
  template <class Tracked>
  uint64_t hashInto(uint64_t h, Tracked&& tracked) const
  {
    for (uint64_t hid : sortedIds())
    {
      const Held& x = held_.at(hid);
      h = mix(h, hash_tags::kHold);
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
      // The reference the hold was stamped against. It is written and
      // restored, and it decides both the tolerance reject and the conduct
      // split, so a value that drifted or came back wrong changes how the
      // hold resolves -- the digest has to see it. Only when set, the same
      // "zero == absent" rule the ids below follow: a hold opened with
      // neither side quoted and nothing printed carries 0 and hashes as it
      // did before.
      if (x.refAtHoldRaw != 0)
      {
        h = mix(h, static_cast<uint64_t>(x.refAtHoldRaw));
      }
      if (x.makerClientOrderId != 0)
      {
        h = mix(h, x.makerClientOrderId);  // only when set: a hold without one hashes as before
      }
      if (x.takerClientOrderId != 0)
      {
        h = mix(h, x.takerClientOrderId);
      }
      // T059: only when set, same guard as the client order ids above -- a
      // hold on a leg that never filled before it opened hashes as before.
      if (!x.makerCumQtyAtHold.isZero())
      {
        h = mix(h, static_cast<uint64_t>(x.makerCumQtyAtHold.raw()));
      }
      if (!x.takerCumQtyAtHold.isZero())
      {
        h = mix(h, static_cast<uint64_t>(x.takerCumQtyAtHold.raw()));
      }
      h = mix(h, tracked(x.maker) ? 1U : 0U);
    }
    return h;
  }

  // One RestoreHeld per open hold, id order -- the same traversal the hash
  // folds. `tracked` as above: the recorded live truth, so recovery does not
  // have to re-derive it.
  template <class Tracked>
  void writeSnapshot(Journal& out, int64_t ts, Tracked&& tracked) const
  {
    for (uint64_t hid : sortedIds())
    {
      const Held& x = held_.at(hid);
      RestoreHeld r{x.id, x.taker, x.takerAccount, x.takerSide, {}, x.maker, x.makerAccount, x.price, x.qty, x.deadline, x.takerTif, x.takerType, {}, x.takerPrice, x.takerExpiryNs, x.makerReduceOnly, x.takerReduceOnly};
      r.makerClientOrderId = x.makerClientOrderId;
      r.takerClientOrderId = x.takerClientOrderId;
      r.makerCumQtyAtHold = x.makerCumQtyAtHold;
      r.takerCumQtyAtHold = x.takerCumQtyAtHold;
      r.makerTracked = tracked(x.maker);
      r.refAtHoldRaw = x.refAtHoldRaw;
      out.append(InboundCommand{r}, ts);
    }
  }

 private:
  template <LastLookHost Host>
  void resolve(Host& host, std::unordered_map<uint64_t, Held>::iterator it, bool accept)
  {
    const Config cfg = host.lastLookConfig();
    const Held h = it->second;
    held_.erase(it);
    open_.store(held_.size(), std::memory_order_relaxed);
    // An accept settles a fill that was risk-checked when the hold was created,
    // possibly a whole window ago. Re-measure it against the position as it is
    // now: a perp fill that would open or flip a reduce-only leg (with no margin
    // behind it) or carry an account past the position cap must not print just
    // because the maker said yes late. The hold is rejected instead -- the
    // honest outcome, since the venue cannot part-accept a hold: liquidity is
    // restored to both legs exactly as a maker reject would.
    if (accept && !host.holdStillAllowed(h))
    {
      ++riskRejected_;
      accept = false;
    }
    // Symmetric price tolerance, applied by the venue on magnitude alone.
    const int64_t moveRaw = moveSinceHold(host, h);
    if (cfg.toleranceRaw > 0)
    {
      const int64_t mag = moveRaw < 0 ? -moveRaw : moveRaw;
      if (mag > cfg.toleranceRaw)
      {
        ++toleranceRejected_;
        accept = false;
      }
    }
    record(h, moveRaw, accept);
    if (accept)
    {
      host.publishTracked(Trade{host.nextTradeSeq(), cfg.symbol, h.price, h.qty, h.maker,
                                h.taker, h.takerSide, h.makerAccount, h.takerAccount});
      const RestingOrder* mk = host.findResting(h.maker);
      const Quantity makerLeaves =
          mk ? Quantity::fromRaw(mk->leaves.raw() + mk->hidden.raw()) : Quantity{};
      const Quantity makerDisp = mk ? mk->leaves : Quantity{};  // displayed peak, public feed
      // T059: an accept confirms this fill, so the book's own optimistic
      // cumQty bump at hold-creation time (fillBest, in matcher.h) is now
      // correct -- read it straight off the still-resting order. If the
      // maker left the book entirely, fall back to the snapshot taken when
      // the hold opened plus this now-confirmed fill.
      const Quantity makerCumAfter = mk ? mk->cumQty : (h.makerCumQtyAtHold + h.qty);
      host.publishTracked(OrderExecuted{h.maker, cfg.symbol, h.qty, makerLeaves, false,
                                        makerLeaves.isZero(), h.price, makerDisp, h.makerAccount,
                                        h.makerClientOrderId, makerCumAfter});
      // The taker leg is done exactly when nothing of its order is still
      // outstanding: not resting (a GTC/GTD residual may already sit in the
      // book, untouched by this hold -- see restoreTaker/onNew's
      // residualRests) and not sitting in another open hold (one sweep can
      // hold against more than one last-look maker; each resolves on its own
      // maker's schedule). `it` above already erased THIS hold, so
      // heldQtyFor sums only the others. Hardcoding leaves=0/complete=true
      // here would fire the terminal report before a sibling hold or a
      // resting residual actually clears; hardcoding them false/0, as this
      // used to, never fires it at all -- see T062.
      const RestingOrder* tk = host.findResting(h.taker);
      const Quantity takerRestLeaves =
          tk ? Quantity::fromRaw(tk->leaves.raw() + tk->hidden.raw()) : Quantity{};
      const Quantity takerLeaves =
          Quantity::fromRaw(takerRestLeaves.raw() + heldQtyFor(h.taker).raw());
      const Quantity takerDisp = tk ? tk->leaves : Quantity{};  // displayed peak, public feed
      // T059: confirmed-before-this-hold (takerCumQtyAtHold) + this fill,
      // now confirmed (h.qty) + whatever a still-resting residual of the
      // SAME order id has filled separately since it started resting
      // (tk->cumQty; 0 when nothing rests). Does not see a SIBLING hold on
      // this taker that already resolved+accepted between this hold's
      // creation and now -- neither the sibling's fill nor this one flows
      // through the other's book entry, and nothing tracks a non-resting
      // order's running total; the same class of honesty limit T058 already
      // documented for the IOC-residual cancel case below.
      const Quantity takerCumAfter =
          h.takerCumQtyAtHold + h.qty + (tk ? tk->cumQty : Quantity{});
      host.publishTracked(OrderExecuted{h.taker, cfg.symbol, h.qty, takerLeaves, true,
                                        takerLeaves.isZero(), h.price, takerDisp, h.takerAccount,
                                        h.takerClientOrderId, takerCumAfter});
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
      restoreMaker(host, h, cfg.symbol);
      restoreTaker(host, h, cfg.symbol);
      host.publish(FillRejected{h.id, cfg.symbol, h.taker, h.maker, h.price, h.qty,
                                h.takerAccount, h.makerAccount, h.takerClientOrderId,
                                h.takerCumQtyAtHold});
    }
    // Whichever way the hold resolved: if this was the last hold on a leg and
    // that leg no longer rests, free its leftover reservation and tracking
    // (deferred from the emit_ wrapper while holds were open).
    host.cleanupOrderIfDone(h.taker);
    host.cleanupOrderIfDone(h.maker);
  }

  // Sort a collected set of due holds by id and reject them in that order --
  // the resolution order feeds the event stream, so it must not depend on the
  // hold table's bucket layout.
  template <LastLookHost Host>
  void rejectAllOf(Host& host, std::vector<uint64_t>& due)
  {
    std::sort(due.begin(), due.end());
    for (uint64_t hid : due)
    {
      if (auto it = held_.find(hid); it != held_.end())
      {
        resolve(host, it, false);
      }
    }
  }

  // How far the reference has moved since the hold was taken, signed so that a
  // positive value means it moved AGAINST the maker: it sold and the price rose,
  // or it bought and the price fell. Zero when there is no reference to compare
  // against.
  template <LastLookHost Host>
  int64_t moveSinceHold(const Host& host, const Held& h) const
  {
    const int64_t nowRaw = host.referenceRaw();
    if (nowRaw == 0 || h.refAtHoldRaw == 0)
    {
      return 0;
    }
    const int64_t delta = nowRaw - h.refAtHoldRaw;
    // takerSide is the aggressor's. A taker buying leaves the maker short, so a
    // rising price hurts the maker.
    return h.takerSide == Side::BUY ? delta : -delta;
  }

  void record(const Held& h, int64_t moveRaw, bool accepted)
  {
    LastLookStats& st = stats_[h.makerAccount];
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

  // Return a rejected hold's qty to the maker: back onto its price level at the
  // tail (as-if re-entered). If the hold consumed the whole displayed size the
  // order left the book -- rebuild it from the hold record.
  template <LastLookHost Host>
  void restoreMaker(Host& host, const Held& h, SymbolId symbol)
  {
    if (auto ro = host.takeResting(h.maker); ro.has_value())
    {
      ro->leaves += h.qty;  // the returned slice was displayed when it was held
      // T059: undo the book's own optimistic cumQty bump from hold creation
      // (fillBest ran before the maker's decision was known -- see Held's
      // comment). A reject means this hold's qty never traded, so it must
      // not count toward the order's running total. Relative, not an
      // overwrite: correct regardless of any OTHER real fill this order took
      // on its still-resting remainder while the hold was open.
      ro->cumQty -= h.qty;
      if (!host.reinsertTail(ro->side, *ro))
      {
        host.releaseHeldLeg(h.maker, h.qty);
        host.publish(OrderCanceled{h.maker, symbol, CancelReason::BookRefused, h.makerAccount,
                                   ro->clientOrderId, ro->leaves, ro->cumQty});
        return;
      }
      host.publish(OrderModified{h.maker, symbol, ro->price, ro->leaves, false, h.makerAccount,
                                 ro->clientOrderId, ro->cumQty});
    }
    else
    {
      const Side makerSide = (h.takerSide == Side::BUY) ? Side::SELL : Side::BUY;
      RestingOrder rebuilt{h.maker, h.makerAccount, h.price, h.qty, makerSide};
      rebuilt.clientOrderId = h.makerClientOrderId;
      rebuilt.lastLook = true;
      rebuilt.reduceOnly = h.makerReduceOnly;
      // T059: this hold took the order's entire remaining size, so its whole
      // history is exactly what it had filled before the hold opened --
      // nothing else could have touched it in between (it was off the book).
      rebuilt.cumQty = h.makerCumQtyAtHold;
      if (!host.reinsertTail(makerSide, rebuilt))
      {
        // This hold took the maker's whole displayed size, so the order left
        // the book and its node with it: the pool can have filled up while the
        // hold was open. Nothing to modify, so the owner is told the order is
        // gone.
        host.releaseHeldLeg(h.maker, h.qty);
        host.publish(OrderCanceled{h.maker, symbol, CancelReason::BookRefused, h.makerAccount,
                                   h.makerClientOrderId, h.qty, rebuilt.cumQty});
        return;
      }
      // Still tracked in orderAccount_/byAccount_: a fully-held maker is never
      // forgotten while its hold is open (see create()).
      host.publish(OrderModified{h.maker, symbol, h.price, h.qty, false, h.makerAccount,
                                 h.makerClientOrderId, rebuilt.cumQty});
    }
  }

  // Return a rejected hold's qty to the taker per its TIF.
  template <LastLookHost Host>
  void restoreTaker(Host& host, const Held& h, SymbolId symbol)
  {
    const bool rests = h.takerType == OrderType::LIMIT &&
                       (h.takerTif == TimeInForce::GTC || h.takerTif == TimeInForce::GTD);
    if (rests)
    {
      // Reservation keeps backing the restored resting quantity.
      if (auto ro = host.takeResting(h.taker); ro.has_value())
      {
        ro->leaves += h.qty;  // combine with the already-resting remainder, tail requeue
        // T059: this residual's own cumQty is untouched by this hold (a
        // reject settles no trade); h.qty never rode through it, so nothing
        // to undo here, unlike the maker side above.
        if (!host.reinsertTail(ro->side, *ro))
        {
          host.releaseHeldLeg(h.taker, h.qty);
          host.publish(OrderCanceled{h.taker, symbol, CancelReason::BookRefused, h.takerAccount,
                                     ro->clientOrderId, ro->leaves, ro->cumQty});
          return;
        }
        host.publish(OrderModified{h.taker, symbol, ro->price, ro->leaves, false, h.takerAccount,
                                   ro->clientOrderId, ro->cumQty});
      }
      else
      {
        RestingOrder rebuilt{h.taker, h.takerAccount, h.takerPrice, h.qty, h.takerSide};
        rebuilt.clientOrderId = h.takerClientOrderId;
        // Its own flag, for the same reason the maker rebuild carries one: a
        // reduce-only leg reserves no margin, so re-resting it as a plain
        // order leaves an order on the book that can OPEN a position with
        // nothing behind it. There is no post-only counterpart: an order that
        // would cross is refused before it reaches a maker, so no hold's
        // taker is ever post-only.
        rebuilt.reduceOnly = h.takerReduceOnly;
        // T059: this hold held the taker's entire remaining size (nothing
        // else rested), so its life-to-date total is exactly what it had
        // confirmed before the hold opened -- the held qty itself never
        // traded (this is a reject).
        rebuilt.cumQty = h.takerCumQtyAtHold;
        if (!host.reinsertTail(h.takerSide, rebuilt))
        {
          // Nothing of this taker rests, so there is no accept to send: it
          // ends the way its TIF would have ended it, with its held buying
          // power released.
          host.releaseHeldLeg(h.taker, h.qty);
          host.publish(OrderCanceled{h.taker, symbol, CancelReason::BookRefused, h.takerAccount,
                                     h.takerClientOrderId, h.qty, rebuilt.cumQty});
          return;
        }
        host.adoptRestingTaker(h);
        host.publish(OrderAccepted{h.taker, symbol, h.takerSide, h.takerPrice, h.qty, true,
                                   h.qty, h.takerAccount, h.takerClientOrderId, rebuilt.cumQty});
      }
      return;
    }
    // IOC / FOK / MARKET: the residual never rests -- release the taker's held
    // buying power and cancel it with the reason its TIF would have produced.
    host.releaseHeldLeg(h.taker, h.qty);
    const CancelReason reason = (h.takerType == OrderType::MARKET) ? CancelReason::MarketResidual
                                : (h.takerTif == TimeInForce::FOK)
                                    ? CancelReason::FillOrKillResidual
                                    : CancelReason::ImmediateOrCancelResidual;
    // T059: h.qty is exactly the residual being killed (LeavesQty); cumQty
    // is now h.takerCumQtyAtHold -- the taker's confirmed total as of when
    // this hold opened (T058 left this at 0, since Held did not carry the
    // value yet; see Held::takerCumQtyAtHold). Still understated if a
    // SIBLING hold on the same taker resolved+accepted in between (neither
    // fill flows through the other's bookkeeping) -- the same class of
    // honesty limit documented on the accept branch above.
    host.publish(OrderCanceled{h.taker, symbol, reason, h.takerAccount, h.takerClientOrderId,
                               h.qty, h.takerCumQtyAtHold});
  }

  std::unordered_map<uint64_t, Held> held_;

  uint64_t seq_{0};

  std::vector<uint64_t> fresh_;  // stamped at the end of the matching pass

  // Diagnostic only, like the pro-rata skip counters: conduct measurement, not
  // matching state, so it stays out of the state hash and the snapshot.
  std::unordered_map<uint64_t, LastLookStats> stats_;

  uint64_t riskRejected_{0};

  uint64_t toleranceRejected_{0};

  // Mirror of held_.size() readable from other threads (the shard's idle
  // sweeper); the component itself never reads it for logic.
  std::atomic<uint64_t> open_{0};
};

}  // namespace flox::venue::engine
