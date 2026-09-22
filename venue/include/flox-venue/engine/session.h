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
#include "flox-venue/messages.h"

#include "flox/util/base/time.h"

#include <array>
#include <cstddef>
#include <cstdint>

// The instrument's session state machine: which trading state the instrument
// is in, which transitions exist, and what each one does to the state.
//
// It is NOT a template. The matching engine is one because the resting book
// is a template parameter; none of this touches the book, so none of it has
// any business being re-instantiated per book type. The engine owns an
// instance and calls into it; the book effects a transition causes (pulling
// the resting orders, running the uncross) stay on the engine side, because
// those are the parts that do touch the book.
//
// One table, kSessionTransitions, is the whole automaton. Every transition
// the venue has goes through it -- operator halt and resume, the emergency
// stop, the session boundary, delisting, the auction phases and the timed
// volatility pause. Adding a transition means adding a row; it cannot mean
// adding an `if` somewhere else, because there is nowhere else. The table is
// also what the documentation is generated from (docs/venue/matching.md,
// checked by test_venue_engine_session), so the written-down automaton cannot
// drift from the running one.
namespace flox::venue::engine
{

// What moves the instrument from one trading state to another. These are
// causes, not commands: AdminAction is the operator-facing verb set and maps
// onto a subset of these, while LuldBreach and LuldPauseElapsed have no
// operator behind them at all -- the engine raises them itself.
enum class SessionEvent : uint8_t
{
  Halt = 0,          // AdminAction::Halt
  Resume,            // AdminAction::Resume
  HaltAndCancelAll,  // AdminAction::HaltAndCancelAll
  BeginPreOpen,      // AdminAction::BeginPreOpen
  OpenContinuous,    // AdminAction::OpenContinuous
  ResumeAuction,     // AdminAction::ResumeAuction
  CloseSession,      // AdminAction::CloseSession
  OpenSession,       // AdminAction::OpenSession
  Delist,            // AdminAction::Delist
  Relist,            // AdminAction::Relist
  LuldBreach,        // the price left the band: the engine trips a timed pause
  LuldPauseElapsed,  // the timed pause deadline passed
};

inline constexpr size_t kSessionEventCount = 12;

// What a transition does to one flag. `Leave` is load-bearing and not
// laziness: an operator halt on top of a live volatility pause leaves the
// pause deadline alone, and a closed session leaves the halt underneath it
// untouched, so reopening returns to the state the close interrupted.
enum class FlagOp : uint8_t
{
  Leave = 0,
  Set,
  Clear,
};

// When a transition may fire. Two of the twelve are conditional, and the
// condition is part of the automaton rather than a check at the call site.
enum class SessionGuard : uint8_t
{
  Always = 0,
  NotAlreadyDelisted,   // Delist on a delisted instrument is not a transition
  PauseDeadlinePassed,  // the timed pause ends only once its deadline is reached
};

// What the ENGINE does around the transition, because it needs the book and
// the session does not.
enum class SessionEffect : uint8_t
{
  None = 0,
  CancelBookAfter,  // pull every resting and pending order, after the status goes out
  UncrossBefore,    // run the auction uncross before the flags move
};

struct SessionTransition
{
  SessionEvent event;
  const char* name;  // the automaton's name for the row; also the doc row's first cell
  SessionGuard guard;
  FlagOp halted;    // cfg_.halted: config, so the engine owns the storage and this moves it
  FlagOp deadline;  // the timed pause deadline (Set takes the caller's value)
  FlagOp auction;
  FlagOp closed;
  FlagOp delisted;
  TradingStatusReason reason;  // what the published transition says caused it
  SessionEffect effect;
};

// The automaton. Order is the enumerator order, and apply() indexes by it.
//
// Read a row as: when `event` arrives and `guard` holds, the five flags move
// as the ops say, the new state is published with `reason`, and the engine
// performs `effect`. A row that fires always publishes -- the duplicate
// suppression happens later, against what actually went out (see publishes()),
// not here, so "the transition fired" and "the feed carried it" stay separate
// facts.
inline constexpr std::array<SessionTransition, kSessionEventCount> kSessionTransitions = {{
    {SessionEvent::Halt, "Halt", SessionGuard::Always, FlagOp::Set, FlagOp::Leave, FlagOp::Leave,
     FlagOp::Leave, FlagOp::Leave, TradingStatusReason::Administrative, SessionEffect::None},
    {SessionEvent::Resume, "Resume", SessionGuard::Always, FlagOp::Clear, FlagOp::Clear,
     FlagOp::Leave, FlagOp::Leave, FlagOp::Leave, TradingStatusReason::Administrative,
     SessionEffect::None},
    {SessionEvent::HaltAndCancelAll, "HaltAndCancelAll", SessionGuard::Always, FlagOp::Set,
     FlagOp::Clear, FlagOp::Leave, FlagOp::Leave, FlagOp::Leave,
     TradingStatusReason::Administrative, SessionEffect::CancelBookAfter},
    {SessionEvent::BeginPreOpen, "BeginPreOpen", SessionGuard::Always, FlagOp::Leave,
     FlagOp::Leave, FlagOp::Set, FlagOp::Leave, FlagOp::Leave, TradingStatusReason::Auction,
     SessionEffect::None},
    {SessionEvent::OpenContinuous, "OpenContinuous", SessionGuard::Always, FlagOp::Leave,
     FlagOp::Leave, FlagOp::Clear, FlagOp::Leave, FlagOp::Leave, TradingStatusReason::Auction,
     SessionEffect::UncrossBefore},
    {SessionEvent::ResumeAuction, "ResumeAuction", SessionGuard::Always, FlagOp::Clear,
     FlagOp::Clear, FlagOp::Set, FlagOp::Leave, FlagOp::Leave, TradingStatusReason::Auction,
     SessionEffect::None},
    {SessionEvent::CloseSession, "CloseSession", SessionGuard::Always, FlagOp::Leave,
     FlagOp::Leave, FlagOp::Leave, FlagOp::Set, FlagOp::Leave, TradingStatusReason::Session,
     SessionEffect::None},
    {SessionEvent::OpenSession, "OpenSession", SessionGuard::Always, FlagOp::Leave, FlagOp::Leave,
     FlagOp::Leave, FlagOp::Clear, FlagOp::Leave, TradingStatusReason::Session,
     SessionEffect::None},
    {SessionEvent::Delist, "Delist", SessionGuard::NotAlreadyDelisted, FlagOp::Leave,
     FlagOp::Leave, FlagOp::Leave, FlagOp::Leave, FlagOp::Set,
     TradingStatusReason::Administrative, SessionEffect::CancelBookAfter},
    {SessionEvent::Relist, "Relist", SessionGuard::Always, FlagOp::Leave, FlagOp::Leave,
     FlagOp::Leave, FlagOp::Leave, FlagOp::Clear, TradingStatusReason::Administrative,
     SessionEffect::None},
    {SessionEvent::LuldBreach, "LuldBreach", SessionGuard::Always, FlagOp::Set, FlagOp::Set,
     FlagOp::Leave, FlagOp::Leave, FlagOp::Leave, TradingStatusReason::LuldBreach,
     SessionEffect::None},
    {SessionEvent::LuldPauseElapsed, "LuldPauseElapsed", SessionGuard::PauseDeadlinePassed,
     FlagOp::Clear, FlagOp::Clear, FlagOp::Leave, FlagOp::Leave, FlagOp::Leave,
     TradingStatusReason::LuldPauseElapsed, SessionEffect::None},
}};

inline constexpr const SessionTransition& transitionFor(SessionEvent e) noexcept
{
  return kSessionTransitions[static_cast<size_t>(e)];
}

// What the engine has to do about a transition that fired.
struct SessionOutcome
{
  bool fired{false};
  TradingStatusReason reason{TradingStatusReason::None};
  SessionEffect effect{SessionEffect::None};
};

class Session
{
 public:
  // ---- state ----------------------------------------------------------
  // Every accessor is a member read: the reject path in validate() asks
  // delisted() and closed() on each order, so they have to cost what reading
  // the field cost.
  bool closed() const noexcept { return closed_; }
  bool delisted() const noexcept { return delisted_; }
  bool auction() const noexcept { return auction_; }
  SeqNanos haltUntil() const noexcept { return haltUntil_; }

  // The trading state, ranked. Closed wins over the halt or auction phase
  // underneath it and delisting wins over everything: they are the
  // instrument's outermost states, and what they cover is preserved untouched
  // so leaving them returns to exactly the state they interrupted.
  //
  // `halted` is passed in because the flag lives on SymbolConfig, where an
  // operator sets it before start(); this reads it rather than mirroring it,
  // since two copies of one flag is how the reject reason and the published
  // status drift apart.
  TradingStatus status(bool halted) const noexcept
  {
    if (delisted_)
    {
      return TradingStatus::Delisted;
    }
    if (closed_)
    {
      return TradingStatus::Closed;
    }
    if (auction_)
    {
      return TradingStatus::AuctionPreOpen;
    }
    if (halted)
    {
      return static_cast<bool>(haltUntil_) ? TradingStatus::LuldPause : TradingStatus::Halted;
    }
    return TradingStatus::Trading;
  }

  // The guard of the LuldPauseElapsed row, exported so the engine can ask it
  // on the submit path without paying for the rest of apply(). apply() calls
  // the same function, so the fast check and the automaton cannot disagree.
  bool pauseElapsed(bool halted, SeqNanos now) const noexcept
  {
    return halted && static_cast<bool>(haltUntil_) && now >= haltUntil_;
  }

  // ---- transitions ----------------------------------------------------
  // Apply one row of the table. `deadline` is the value FlagOp::Set writes
  // into the pause deadline (LuldBreach: now + the configured pause length);
  // `now` is what the PauseDeadlinePassed guard measures against. Returns
  // what the engine still has to do: publish the new state with the row's
  // reason, and perform the row's effect.
  SessionOutcome apply(SessionEvent e, bool& halted, SeqNanos now, SeqNanos deadline) noexcept
  {
    const SessionTransition& t = transitionFor(e);
    if (!allowed(t.guard, halted, now))
    {
      return {};
    }
    move(t.halted, halted);
    if (t.deadline == FlagOp::Set)
    {
      haltUntil_ = deadline;
    }
    else if (t.deadline == FlagOp::Clear)
    {
      haltUntil_ = SeqNanos{};
    }
    move(t.auction, auction_);
    move(t.closed, closed_);
    move(t.delisted, delisted_);
    return {true, t.reason, t.effect};
  }

  // ---- what went out --------------------------------------------------
  // The transition memo: the last state PUBLISHED, so the feed carries
  // transitions and only transitions. Not hashed and not snapshotted -- it is
  // a record of what went out, not engine state; the state itself is the
  // flags, which stateHash covers and a snapshot restores.
  //
  // Returns whether this (status, deadline) pair differs from the last one
  // published, and records it when it does: one call, so a caller cannot ask
  // and then forget to record.
  bool publishes(TradingStatus status, int64_t untilNs) noexcept
  {
    if (published_ && status == lastStatus_ && untilNs == lastStatusUntil_)
    {
      return false;
    }
    published_ = true;
    lastStatus_ = status;
    lastStatusUntil_ = untilNs;
    return true;
  }

  // The deadline a status publishes. It belongs to the timed pause and to
  // nothing else: any other state publishes 0 even while a pause deadline is
  // still stored underneath it, because a subscriber must never be handed an
  // expiry for a state that does not expire.
  int64_t publishedUntil(TradingStatus status) const noexcept
  {
    return status == TradingStatus::LuldPause ? haltUntil_.raw() : 0;
  }

  // Re-sync the memo to a state that was restored rather than published, so
  // the next real transition is measured against the recovered state.
  void memoRestored(TradingStatus status) noexcept
  {
    lastStatus_ = status;
    lastStatusUntil_ = publishedUntil(status);
    published_ = true;
  }

  // ---- checkpoint -----------------------------------------------------
  // The state hash contribution of these fields, in the order the engine
  // folded them before they moved here. The order is the format: change it
  // and every snapshot already on disk fails verification.
  //
  // delisted_ and closed_ fold only when set -- the same "zero == absent"
  // rule the balance traversal follows -- so an instrument that was never
  // closed and never delisted hashes exactly as it did before the fields
  // existed, which is what lets an older snapshot still verify on load.
  uint64_t mixState(uint64_t h) const noexcept
  {
    h = mix(h, auction_ ? 1U : 0U);
    if (delisted_)
    {
      h = mix(h, 0xB00EU);
    }
    h = mix(h, static_cast<uint64_t>(haltUntil_.raw()));
    if (closed_)
    {
      h = mix(h, 0xB00AU);
      h = mix(h, 1U);
    }
    return h;
  }

  // The admin records that reproduce this session state, in file order. They
  // ride the existing AdminCmd path rather than a record of their own, so a
  // session transition replays at its exact point in the stream and costs no
  // new journal tag.
  //
  // Order is the format and it is the ranking in status(): the halt first,
  // then the auction phase, then the session boundary, then delisting, so the
  // state restores outermost-last. Only the set ones are written, so an
  // instrument that was never closed and never delisted writes the file it
  // always did.
  size_t snapshotActions(bool halted, std::array<AdminAction, 4>& out) const noexcept
  {
    size_t n = 0;
    out[n++] = halted ? AdminAction::Halt : AdminAction::Resume;
    if (auction_)
    {
      out[n++] = AdminAction::BeginPreOpen;
    }
    if (closed_)
    {
      out[n++] = AdminAction::CloseSession;
    }
    if (delisted_)
    {
      out[n++] = AdminAction::Delist;
    }
    return n;
  }

  // The pause deadline travels in SnapshotEnd rather than as an admin record:
  // it is a number, not a transition.
  void restoreHaltUntil(SeqNanos v) noexcept { haltUntil_ = v; }

  // The snapshot clone copies the live session state, delisted_ included: a
  // checkpoint taken on a delisted instrument must restore it delisted. See
  // the note in docs/venue/matching.md.
  void copyForSnapshotClone(const Session& src) noexcept
  {
    auction_ = src.auction_;
    haltUntil_ = src.haltUntil_;
    closed_ = src.closed_;
    delisted_ = src.delisted_;
    lastStatus_ = src.lastStatus_;
    lastStatusUntil_ = src.lastStatusUntil_;
    published_ = src.published_;
  }

 private:
  static void move(FlagOp op, bool& flag) noexcept
  {
    if (op == FlagOp::Set)
    {
      flag = true;
    }
    else if (op == FlagOp::Clear)
    {
      flag = false;
    }
  }

  bool allowed(SessionGuard g, bool halted, SeqNanos now) const noexcept
  {
    switch (g)
    {
      case SessionGuard::Always:
        return true;
      case SessionGuard::NotAlreadyDelisted:
        return !delisted_;
      case SessionGuard::PauseDeadlinePassed:
        return pauseElapsed(halted, now);
    }
    return true;
  }

  bool auction_{false};

  SeqNanos haltUntil_{};

  // Deliberately separate from cfg_.halted: a closed session and an operator
  // halt are different facts with different reject reasons, and a close must
  // not clear a halt underneath it. Hashed and checkpointed.
  bool closed_{false};

  bool delisted_{false};  // withdrawn from trading; outranks halt / session / auction

  TradingStatus lastStatus_{TradingStatus::Trading};

  int64_t lastStatusUntil_{0};

  bool published_{false};
};

}  // namespace flox::venue::engine
