/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * The session automaton: engine::Session and the engine around it.
 *
 * The venue's trading state used to be five flags mutated in place by ten
 * methods. It is one table now (kSessionTransitions), and a table is only
 * worth having if something checks every row of it -- otherwise a wrong cell
 * is harder to find than a wrong `if` was.
 *
 * So this file checks the automaton exhaustively and against something other
 * than itself: every reachable flag combination crossed with every event, both
 * before and after a pause deadline, compared with a SECOND statement of the
 * rules written out by hand below (referenceApply / referenceStatus). Reading
 * the table to test the table would pass on any table.
 *
 * Each refusal -- the two guarded rows -- gets its own case, because a guard
 * that stops guarding is invisible to a test that only looks at what happens
 * when the guard passes.
 *
 * The engine-level half then checks the parts the session deliberately cannot
 * do for itself: the transition reaching the feed, the order of the status
 * against the cancels it causes, and the reject reason an order gets.
 *
 * Finally, the table generates the documentation (docs/venue/matching.md) and
 * this file checks the generated block is what is committed, so the written
 * automaton cannot drift from the running one.
 */
#include "flox-venue/engine/session.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/messages.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace flox;
using namespace flox::venue;
using flox::venue::engine::FlagOp;
using flox::venue::engine::kSessionEventCount;
using flox::venue::engine::kSessionTransitions;
using flox::venue::engine::Session;
using flox::venue::engine::SessionEffect;
using flox::venue::engine::SessionEvent;
using flox::venue::engine::SessionGuard;
using flox::venue::engine::SessionOutcome;

namespace
{

constexpr SymbolId SYM = 1;
constexpr int64_t kDeadline = 1'000;  // the pause deadline every parameterised case uses

// ---------------------------------------------------------------------------
// The automaton, written a second time, by hand
// ---------------------------------------------------------------------------

// The five flags the session is. `deadline` is "a pause deadline is stored",
// which is what the status ladder and the pause guard actually read; its value
// only matters against `now`.
struct Flags
{
  bool halted{false};
  bool deadline{false};
  bool auction{false};
  bool closed{false};
  bool delisted{false};
};

bool operator==(const Flags& a, const Flags& b)
{
  return a.halted == b.halted && a.deadline == b.deadline && a.auction == b.auction &&
         a.closed == b.closed && a.delisted == b.delisted;
}

// The status ladder as prose: delisting is outermost, then the closed session,
// then the auction phase, then the halt -- and a halt with a deadline is the
// timed volatility pause rather than an operator halt.
TradingStatus referenceStatus(const Flags& f)
{
  if (f.delisted)
  {
    return TradingStatus::Delisted;
  }
  if (f.closed)
  {
    return TradingStatus::Closed;
  }
  if (f.auction)
  {
    return TradingStatus::AuctionPreOpen;
  }
  if (f.halted)
  {
    return f.deadline ? TradingStatus::LuldPause : TradingStatus::Halted;
  }
  return TradingStatus::Trading;
}

// What each event does, stated independently of kSessionTransitions. Returns
// whether the event is a transition at all; `f` is left untouched when it is
// not.
bool referenceApply(SessionEvent e, Flags& f, bool deadlineReached)
{
  switch (e)
  {
    case SessionEvent::Halt:
      // An operator halt leaves a live pause deadline alone: halting an
      // already paused instrument must not turn the pause into a plain halt.
      f.halted = true;
      return true;
    case SessionEvent::Resume:
      f.halted = false;
      f.deadline = false;
      return true;
    case SessionEvent::HaltAndCancelAll:
      f.halted = true;
      f.deadline = false;  // an operator halt has no deadline
      return true;
    case SessionEvent::BeginPreOpen:
      f.auction = true;
      return true;
    case SessionEvent::OpenContinuous:
      f.auction = false;
      return true;
    case SessionEvent::ResumeAuction:
      f.halted = false;
      f.deadline = false;
      f.auction = true;
      return true;
    case SessionEvent::CloseSession:
      f.closed = true;  // the halt and the auction phase underneath are untouched
      return true;
    case SessionEvent::OpenSession:
      f.closed = false;
      return true;
    case SessionEvent::Delist:
      if (f.delisted)
      {
        return false;  // delisting a delisted instrument is not a transition
      }
      f.delisted = true;
      return true;
    case SessionEvent::Relist:
      f.delisted = false;
      return true;
    case SessionEvent::LuldBreach:
      f.halted = true;
      f.deadline = true;
      return true;
    case SessionEvent::LuldPauseElapsed:
      if (!f.halted || !f.deadline || !deadlineReached)
      {
        return false;  // the pause ends at its deadline and not before
      }
      f.halted = false;
      f.deadline = false;
      return true;
  }
  return false;
}

TradingStatusReason referenceReason(SessionEvent e)
{
  switch (e)
  {
    case SessionEvent::Halt:
    case SessionEvent::Resume:
    case SessionEvent::HaltAndCancelAll:
    case SessionEvent::Delist:
    case SessionEvent::Relist:
      return TradingStatusReason::Administrative;
    case SessionEvent::BeginPreOpen:
    case SessionEvent::OpenContinuous:
    case SessionEvent::ResumeAuction:
      return TradingStatusReason::Auction;
    case SessionEvent::CloseSession:
    case SessionEvent::OpenSession:
      return TradingStatusReason::Session;
    case SessionEvent::LuldBreach:
      return TradingStatusReason::LuldBreach;
    case SessionEvent::LuldPauseElapsed:
      return TradingStatusReason::LuldPauseElapsed;
  }
  return TradingStatusReason::None;
}

// ---------------------------------------------------------------------------
// Building a session in a given state
// ---------------------------------------------------------------------------

Session sessionIn(const Flags& f)
{
  Session s;
  bool unusedHalt = false;
  if (f.auction)
  {
    s.apply(SessionEvent::BeginPreOpen, unusedHalt, SeqNanos{}, SeqNanos{});
  }
  if (f.closed)
  {
    s.apply(SessionEvent::CloseSession, unusedHalt, SeqNanos{}, SeqNanos{});
  }
  if (f.delisted)
  {
    s.apply(SessionEvent::Delist, unusedHalt, SeqNanos{}, SeqNanos{});
  }
  if (f.deadline)
  {
    s.restoreHaltUntil(SeqNanos::fromRaw(kDeadline));
  }
  return s;
}

Flags flagsOf(const Session& s, bool halted)
{
  return Flags{halted, static_cast<bool>(s.haltUntil()), s.auction(), s.closed(), s.delisted()};
}

const char* eventName(SessionEvent e)
{
  return kSessionTransitions[static_cast<size_t>(e)].name;
}

// H halted, D pause deadline stored, A auction phase, C session closed,
// X delisted; a lower-case letter is the flag off. Letters rather than a
// bitmask so a failing case names the state it failed in, and no dashes
// because a parameterised test name has to be an identifier.
std::string flagName(const Flags& f)
{
  std::string n;
  n += f.halted ? 'H' : 'h';
  n += f.deadline ? 'D' : 'd';
  n += f.auction ? 'A' : 'a';
  n += f.closed ? 'C' : 'c';
  n += f.delisted ? 'X' : 'x';
  return n;
}

Flags flagsFromIndex(int i)
{
  return Flags{(i & 1) != 0, (i & 2) != 0, (i & 4) != 0, (i & 8) != 0, (i & 16) != 0};
}

// ---------------------------------------------------------------------------
// An engine plus everything it published
// ---------------------------------------------------------------------------

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(1.0);
  c.maxPrice = px(1000.0);
  return c;
}

NewOrder limitOrder(OrderId id, Side s, double p, double q, uint64_t acct = 1)
{
  NewOrder o;
  o.id = id;
  o.symbol = SYM;
  o.side = s;
  o.type = OrderType::LIMIT;
  o.price = px(p);
  o.quantity = qty(q);
  o.accountId = acct;
  return o;
}

struct Eng
{
  std::vector<OutboundEvent> out;
  MatchingEngine<MatchingBook> eng;

  explicit Eng(SymbolConfig c = cfg())
      : eng(c, [this](const OutboundEvent& e)
            { out.push_back(e); })
  {
  }

  void admin(AdminAction a, int64_t ts) { eng.submit(InboundCommand{AdminCmd{SYM, a}}, ts); }

  std::vector<TradingStatusChanged> statuses() const
  {
    std::vector<TradingStatusChanged> v;
    for (const OutboundEvent& e : out)
    {
      if (const auto* s = std::get_if<TradingStatusChanged>(&e))
      {
        v.push_back(*s);
      }
    }
    return v;
  }

  // Index of the first event of each kind, so a test can say "the status came
  // out before the cancels" rather than counting.
  int firstStatusAt() const { return firstOf<TradingStatusChanged>(); }
  int firstCancelAt() const { return firstOf<OrderCanceled>(); }
  int firstTradeAt() const { return firstOf<Trade>(); }

  template <class T>
  int firstOf() const
  {
    for (size_t i = 0; i < out.size(); ++i)
    {
      if (std::holds_alternative<T>(out[i]))
      {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  int rejects(RejectReason r) const
  {
    int n = 0;
    for (const OutboundEvent& e : out)
    {
      if (const auto* rj = std::get_if<OrderRejected>(&e); rj != nullptr && rj->reason == r)
      {
        ++n;
      }
    }
    return n;
  }
};

// ---------------------------------------------------------------------------
// The documentation block, generated from the table
// ---------------------------------------------------------------------------

const char* opCell(FlagOp op)
{
  switch (op)
  {
    case FlagOp::Set:
      return "set";
    case FlagOp::Clear:
      return "clear";
    case FlagOp::Leave:
      return "--";
  }
  return "--";
}

const char* guardCell(SessionGuard g)
{
  switch (g)
  {
    case SessionGuard::Always:
      return "always";
    case SessionGuard::NotAlreadyDelisted:
      return "not already delisted";
    case SessionGuard::PauseDeadlinePassed:
      return "halted, with a deadline that has passed";
  }
  return "always";
}

const char* effectCell(SessionEffect e)
{
  switch (e)
  {
    case SessionEffect::None:
      return "--";
    case SessionEffect::CancelBookAfter:
      return "pulls the book, after the status";
    case SessionEffect::UncrossBefore:
      return "uncrosses, before the flags move";
  }
  return "--";
}

const char* reasonCell(TradingStatusReason r)
{
  switch (r)
  {
    case TradingStatusReason::None:
      return "`None`";
    case TradingStatusReason::Administrative:
      return "`Administrative`";
    case TradingStatusReason::LuldBreach:
      return "`LuldBreach`";
    case TradingStatusReason::LuldPauseElapsed:
      return "`LuldPauseElapsed`";
    case TradingStatusReason::Auction:
      return "`Auction`";
    case TradingStatusReason::Session:
      return "`Session`";
  }
  return "`None`";
}

std::string generatedTable()
{
  std::ostringstream o;
  o << "| Event | Fires | Halt | Pause deadline | Auction | Session closed | Delisted | Published as | The engine also |\n";
  o << "|---|---|---|---|---|---|---|---|---|\n";
  for (const auto& t : kSessionTransitions)
  {
    o << "| `" << t.name << "` | " << guardCell(t.guard) << " | " << opCell(t.halted) << " | "
      << opCell(t.deadline) << " | " << opCell(t.auction) << " | " << opCell(t.closed) << " | "
      << opCell(t.delisted) << " | " << reasonCell(t.reason) << " | " << effectCell(t.effect)
      << " |\n";
  }
  return o.str();
}

constexpr const char* kDocOpen = "<!-- generated: session transitions -->";
constexpr const char* kDocClose = "<!-- end generated: session transitions -->";

}  // namespace

// ---------------------------------------------------------------------------
// The table itself
// ---------------------------------------------------------------------------

static_assert(kSessionTransitions.size() == kSessionEventCount,
              "kSessionEventCount and the table disagree about how many events exist");

TEST(VenueEngineSessionTable, EveryEventHasItsOwnRowAtItsOwnIndex)
{
  for (size_t i = 0; i < kSessionTransitions.size(); ++i)
  {
    EXPECT_EQ(static_cast<size_t>(kSessionTransitions[i].event), i)
        << "row " << i << " (" << kSessionTransitions[i].name << ") is indexed by the wrong event";
    EXPECT_NE(kSessionTransitions[i].name, nullptr);
  }
}

TEST(VenueEngineSessionTable, NoTransitionIsSilent)
{
  // A row that fires publishes. The de-duplication happens later, against what
  // went out; a row with no reason would be a state change nothing reports.
  for (const auto& t : kSessionTransitions)
  {
    EXPECT_NE(t.reason, TradingStatusReason::None) << t.name << " would change state silently";
  }
}

TEST(VenueEngineSessionTable, OnlyTheTwoGuardedRowsAreGuarded)
{
  for (const auto& t : kSessionTransitions)
  {
    const bool guarded = t.guard != SessionGuard::Always;
    const bool expected =
        t.event == SessionEvent::Delist || t.event == SessionEvent::LuldPauseElapsed;
    EXPECT_EQ(guarded, expected) << t.name << ": a guard appeared or disappeared";
  }
}

// ---------------------------------------------------------------------------
// The status ladder
// ---------------------------------------------------------------------------

TEST(VenueEngineSessionStatus, RankedOverEveryFlagCombination)
{
  for (int i = 0; i < 32; ++i)
  {
    const Flags f = flagsFromIndex(i);
    const Session s = sessionIn(f);
    EXPECT_EQ(s.status(f.halted), referenceStatus(f)) << "flags " << flagName(f);
  }
}

TEST(VenueEngineSessionStatus, TheDeadlineIsPublishedForThePauseAndNothingElse)
{
  for (int i = 0; i < 32; ++i)
  {
    const Flags f = flagsFromIndex(i);
    const Session s = sessionIn(f);
    const TradingStatus st = s.status(f.halted);
    const int64_t until = s.publishedUntil(st);
    if (st == TradingStatus::LuldPause)
    {
      EXPECT_EQ(until, kDeadline) << "flags " << flagName(f);
    }
    else
    {
      EXPECT_EQ(until, 0) << "flags " << flagName(f) << " published an expiry for " << int(st);
    }
  }
}

// ---------------------------------------------------------------------------
// Every transition, from every state, on both sides of the deadline
// ---------------------------------------------------------------------------

struct TransitionCase
{
  Flags from;
  SessionEvent event;
  bool deadlineReached;
};

class VenueEngineSessionTransition : public ::testing::TestWithParam<TransitionCase>
{
};

TEST_P(VenueEngineSessionTransition, MovesTheFlagsTheWayTheRulesSay)
{
  const TransitionCase c = GetParam();
  Session s = sessionIn(c.from);
  bool halted = c.from.halted;
  const SeqNanos now = SeqNanos::fromRaw(c.deadlineReached ? kDeadline : kDeadline - 1);
  const SeqNanos newDeadline = SeqNanos::fromRaw(kDeadline);

  Flags want = c.from;
  const bool wantFired = referenceApply(c.event, want, c.deadlineReached);

  const SessionOutcome got = s.apply(c.event, halted, now, newDeadline);

  EXPECT_EQ(got.fired, wantFired);
  EXPECT_TRUE(flagsOf(s, halted) == want)
      << "got " << flagName(flagsOf(s, halted)) << ", want " << flagName(want);
  EXPECT_EQ(s.status(halted), referenceStatus(want));
  if (wantFired)
  {
    EXPECT_EQ(got.reason, referenceReason(c.event));
  }
  else
  {
    // A refused row is not a transition: nothing to publish, nothing to do.
    EXPECT_EQ(got.reason, TradingStatusReason::None);
    EXPECT_EQ(got.effect, SessionEffect::None);
  }
}

std::vector<TransitionCase> allTransitionCases()
{
  std::vector<TransitionCase> v;
  for (int i = 0; i < 32; ++i)
  {
    for (size_t e = 0; e < kSessionEventCount; ++e)
    {
      v.push_back({flagsFromIndex(i), static_cast<SessionEvent>(e), false});
      v.push_back({flagsFromIndex(i), static_cast<SessionEvent>(e), true});
    }
  }
  return v;
}

INSTANTIATE_TEST_SUITE_P(EveryStateEveryEvent, VenueEngineSessionTransition,
                         ::testing::ValuesIn(allTransitionCases()),
                         [](const ::testing::TestParamInfo<TransitionCase>& i)
                         {
                           return std::string(eventName(i.param.event)) + "_from_" +
                                  flagName(i.param.from) + (i.param.deadlineReached ? "_at_deadline" : "_before_deadline");
                         });

// ---------------------------------------------------------------------------
// The refusals, one case each
// ---------------------------------------------------------------------------

TEST(VenueEngineSessionRefusal, DelistingADelistedInstrumentIsNotATransition)
{
  Session s;
  bool halted = false;
  EXPECT_TRUE(s.apply(SessionEvent::Delist, halted, SeqNanos{}, SeqNanos{}).fired);
  const SessionOutcome again = s.apply(SessionEvent::Delist, halted, SeqNanos{}, SeqNanos{});
  EXPECT_FALSE(again.fired);
  EXPECT_EQ(again.effect, SessionEffect::None) << "a second delist would sweep the book again";
  EXPECT_TRUE(s.delisted());
}

TEST(VenueEngineSessionRefusal, ThePauseDoesNotEndBeforeItsDeadline)
{
  Session s;
  bool halted = false;
  s.apply(SessionEvent::LuldBreach, halted, SeqNanos::fromRaw(0), SeqNanos::fromRaw(kDeadline));
  ASSERT_TRUE(halted);
  const SessionOutcome early =
      s.apply(SessionEvent::LuldPauseElapsed, halted, SeqNanos::fromRaw(kDeadline - 1), SeqNanos{});
  EXPECT_FALSE(early.fired);
  EXPECT_TRUE(halted);
  EXPECT_EQ(s.haltUntil().raw(), kDeadline);
  EXPECT_EQ(s.status(halted), TradingStatus::LuldPause);
}

TEST(VenueEngineSessionRefusal, ThePauseEndsExactlyAtItsDeadline)
{
  Session s;
  bool halted = false;
  s.apply(SessionEvent::LuldBreach, halted, SeqNanos::fromRaw(0), SeqNanos::fromRaw(kDeadline));
  const SessionOutcome at =
      s.apply(SessionEvent::LuldPauseElapsed, halted, SeqNanos::fromRaw(kDeadline), SeqNanos{});
  EXPECT_TRUE(at.fired);
  EXPECT_FALSE(halted);
  EXPECT_EQ(s.haltUntil().raw(), 0);
}

TEST(VenueEngineSessionRefusal, AnOperatorHaltHasNoDeadlineToElapse)
{
  Session s;
  bool halted = false;
  s.apply(SessionEvent::Halt, halted, SeqNanos{}, SeqNanos{});
  ASSERT_TRUE(halted);
  ASSERT_EQ(s.haltUntil().raw(), 0);
  // No deadline stored: the pause row must not fire, however far time moves.
  EXPECT_FALSE(
      s.apply(SessionEvent::LuldPauseElapsed, halted, SeqNanos::fromRaw(1'000'000), SeqNanos{})
          .fired);
  EXPECT_TRUE(halted) << "an operator halt lifted itself on a clock";
}

TEST(VenueEngineSessionRefusal, AStoredDeadlineDoesNotEndAPauseThatIsNotOn)
{
  Session s;
  bool halted = false;
  s.restoreHaltUntil(SeqNanos::fromRaw(kDeadline));  // a deadline under a symbol that is not halted
  EXPECT_FALSE(
      s.apply(SessionEvent::LuldPauseElapsed, halted, SeqNanos::fromRaw(kDeadline), SeqNanos{})
          .fired);
  EXPECT_EQ(s.haltUntil().raw(), kDeadline);
}

TEST(VenueEngineSessionRefusal, TheFastGuardAndTheTableAgree)
{
  // The submit path asks pauseElapsed() instead of paying for apply(); the two
  // must never disagree about when the pause is over.
  for (int i = 0; i < 32; ++i)
  {
    const Flags f = flagsFromIndex(i);
    for (int64_t now : {kDeadline - 1, kDeadline, kDeadline + 1})
    {
      Session probe = sessionIn(f);
      bool halted = f.halted;
      const bool fast = probe.pauseElapsed(halted, SeqNanos::fromRaw(now));
      const bool fired =
          probe.apply(SessionEvent::LuldPauseElapsed, halted, SeqNanos::fromRaw(now), SeqNanos{})
              .fired;
      EXPECT_EQ(fast, fired) << "flags " << flagName(f) << " at " << now;
    }
  }
}

// ---------------------------------------------------------------------------
// What went out
// ---------------------------------------------------------------------------

TEST(VenueEngineSessionMemo, ARepeatOfTheSameStateIsNotPublished)
{
  Session s;
  EXPECT_TRUE(s.publishes(TradingStatus::Halted, 0));
  EXPECT_FALSE(s.publishes(TradingStatus::Halted, 0));
  EXPECT_TRUE(s.publishes(TradingStatus::Trading, 0));
  EXPECT_TRUE(s.publishes(TradingStatus::LuldPause, 5));
  EXPECT_FALSE(s.publishes(TradingStatus::LuldPause, 5));
  EXPECT_TRUE(s.publishes(TradingStatus::LuldPause, 6)) << "a new deadline is a new state";
}

TEST(VenueEngineSessionMemo, TheFirstTradingStatusIsPublished)
{
  // Nothing has gone out yet, so even the state the engine starts in is a
  // transition the first time something asks for it.
  Session s;
  EXPECT_TRUE(s.publishes(TradingStatus::Trading, 0));
}

TEST(VenueEngineSessionMemo, ARestoredStateIsNotRepublished)
{
  Session s;
  s.memoRestored(TradingStatus::Closed);
  EXPECT_FALSE(s.publishes(TradingStatus::Closed, 0));
  EXPECT_TRUE(s.publishes(TradingStatus::Trading, 0));
}

// ---------------------------------------------------------------------------
// The engine around the session
// ---------------------------------------------------------------------------

TEST(VenueEngineSessionEngine, EveryAdminActionPublishesItsTransition)
{
  struct Case
  {
    AdminAction action;
    TradingStatus status;
    TradingStatusReason reason;
  };
  const Case cases[] = {
      {AdminAction::Halt, TradingStatus::Halted, TradingStatusReason::Administrative},
      {AdminAction::Resume, TradingStatus::Trading, TradingStatusReason::Administrative},
      {AdminAction::HaltAndCancelAll, TradingStatus::Halted, TradingStatusReason::Administrative},
      {AdminAction::BeginPreOpen, TradingStatus::AuctionPreOpen, TradingStatusReason::Auction},
      {AdminAction::ResumeAuction, TradingStatus::AuctionPreOpen, TradingStatusReason::Auction},
      {AdminAction::CloseSession, TradingStatus::Closed, TradingStatusReason::Session},
      {AdminAction::OpenSession, TradingStatus::Trading, TradingStatusReason::Session},
      {AdminAction::Delist, TradingStatus::Delisted, TradingStatusReason::Administrative},
      {AdminAction::Relist, TradingStatus::Trading, TradingStatusReason::Administrative},
  };
  for (const Case& c : cases)
  {
    Eng e;
    // Start from a state the action actually moves away from, so the memo does
    // not swallow the transition.
    if (c.status == TradingStatus::Trading)
    {
      e.admin(c.action == AdminAction::OpenSession ? AdminAction::CloseSession
              : c.action == AdminAction::Relist    ? AdminAction::Delist
                                                   : AdminAction::Halt,
              1);
    }
    e.admin(c.action, 2);
    const auto st = e.statuses();
    ASSERT_FALSE(st.empty()) << int(c.action) << " published nothing";
    EXPECT_EQ(st.back().status, c.status) << int(c.action);
    EXPECT_EQ(st.back().reason, c.reason) << int(c.action);
    EXPECT_EQ(st.back().symbol, SYM);
    EXPECT_EQ(st.back().untilNs, 0);
    EXPECT_EQ(e.eng.tradingStatus(), c.status);
  }
}

TEST(VenueEngineSessionEngine, OpenContinuousPublishesTheUncrossBetweenPreOpenAndTrading)
{
  Eng e;
  e.admin(AdminAction::BeginPreOpen, 1);
  e.eng.submit(InboundCommand{limitOrder(1, Side::BUY, 100, 2, 1)}, 2);
  e.eng.submit(InboundCommand{limitOrder(2, Side::SELL, 99, 2, 2)}, 3);  // crossed, no match yet
  ASSERT_EQ(e.firstTradeAt(), -1) << "a pre-open book matched";
  e.admin(AdminAction::OpenContinuous, 4);

  const auto st = e.statuses();
  ASSERT_EQ(st.size(), 3u);
  EXPECT_EQ(st[0].status, TradingStatus::AuctionPreOpen);
  EXPECT_EQ(st[1].status, TradingStatus::AuctionUncross);
  EXPECT_EQ(st[2].status, TradingStatus::Trading);
  EXPECT_EQ(st[2].reason, TradingStatusReason::Auction);
  EXPECT_GT(e.firstTradeAt(), 0) << "the uncross matched nothing";
}

TEST(VenueEngineSessionEngine, HaltAndCancelAllSendsTheStatusBeforeTheCancels)
{
  Eng e;
  e.eng.submit(InboundCommand{limitOrder(1, Side::BUY, 100, 2, 1)}, 1);
  e.admin(AdminAction::HaltAndCancelAll, 2);
  ASSERT_GE(e.firstStatusAt(), 0);
  ASSERT_GE(e.firstCancelAt(), 0) << "the book was not pulled";
  EXPECT_LT(e.firstStatusAt(), e.firstCancelAt())
      << "the cancels arrive with no halt to explain them";
  EXPECT_EQ(e.eng.restingOrderCount(), 0u);
}

TEST(VenueEngineSessionEngine, DelistSendsTheStatusBeforeTheCancels)
{
  Eng e;
  e.eng.submit(InboundCommand{limitOrder(1, Side::BUY, 100, 2, 1)}, 1);
  e.admin(AdminAction::Delist, 2);
  ASSERT_GE(e.firstCancelAt(), 0) << "the book was not pulled";
  EXPECT_LT(e.firstStatusAt(), e.firstCancelAt());
  EXPECT_EQ(e.eng.restingOrderCount(), 0u);
  EXPECT_TRUE(e.eng.delisted());
}

TEST(VenueEngineSessionEngine, ASecondDelistPublishesNothing)
{
  Eng e;
  e.admin(AdminAction::Delist, 1);
  const size_t after = e.out.size();
  e.admin(AdminAction::Delist, 2);
  EXPECT_EQ(e.out.size(), after) << "the refused row still reached the feed";
}

TEST(VenueEngineSessionEngine, ClosingOverAHaltReopensStillHalted)
{
  Eng e;
  e.admin(AdminAction::Halt, 1);
  e.admin(AdminAction::CloseSession, 2);
  EXPECT_EQ(e.eng.tradingStatus(), TradingStatus::Closed);
  e.admin(AdminAction::OpenSession, 3);
  EXPECT_EQ(e.eng.tradingStatus(), TradingStatus::Halted)
      << "the session boundary cleared a halt an operator raised";
}

TEST(VenueEngineSessionEngine, DelistingOutranksEverythingUnderneath)
{
  Eng e;
  e.admin(AdminAction::BeginPreOpen, 1);
  e.admin(AdminAction::CloseSession, 2);
  e.admin(AdminAction::Delist, 3);
  EXPECT_EQ(e.eng.tradingStatus(), TradingStatus::Delisted);
  e.admin(AdminAction::OpenSession, 4);
  EXPECT_EQ(e.eng.tradingStatus(), TradingStatus::Delisted);
  e.admin(AdminAction::Relist, 5);
  EXPECT_EQ(e.eng.tradingStatus(), TradingStatus::AuctionPreOpen) << "the pre-open underneath was lost";
}

TEST(VenueEngineSessionEngine, EachOutermostStateHasItsOwnRejectReason)
{
  {
    Eng e;
    e.admin(AdminAction::Halt, 1);
    e.eng.submit(InboundCommand{limitOrder(1, Side::BUY, 100, 1, 1)}, 2);
    EXPECT_EQ(e.rejects(RejectReason::Halted), 1);
  }
  {
    Eng e;
    e.admin(AdminAction::CloseSession, 1);
    e.eng.submit(InboundCommand{limitOrder(1, Side::BUY, 100, 1, 1)}, 2);
    EXPECT_EQ(e.rejects(RejectReason::MarketClosed), 1);
  }
  {
    Eng e;
    e.admin(AdminAction::Delist, 1);
    e.eng.submit(InboundCommand{limitOrder(1, Side::BUY, 100, 1, 1)}, 2);
    EXPECT_EQ(e.rejects(RejectReason::InstrumentDelisted), 1);
  }
}

TEST(VenueEngineSessionEngine, ThePauseCarriesItsDeadlineAndEndsOnIt)
{
  SymbolConfig c = cfg();
  c.luldBps = 500;
  c.luldHaltNs = DurationNs{1'000};
  Eng e(c);
  e.eng.submit(InboundCommand{limitOrder(1, Side::SELL, 100, 5, 1)}, 1);
  e.eng.submit(InboundCommand{limitOrder(2, Side::BUY, 100, 5, 2)}, 2);  // last = 100
  e.eng.submit(InboundCommand{limitOrder(3, Side::BUY, 200, 1, 2)}, 3);  // outside the band

  auto st = e.statuses();
  ASSERT_FALSE(st.empty());
  EXPECT_EQ(st.back().status, TradingStatus::LuldPause);
  EXPECT_EQ(st.back().reason, TradingStatusReason::LuldBreach);
  EXPECT_EQ(st.back().untilNs, 3 + 1'000);

  e.eng.submit(InboundCommand{limitOrder(4, Side::BUY, 100, 1, 2)}, 500);
  EXPECT_EQ(e.rejects(RejectReason::Halted), 1) << "the pause let an order through";

  e.eng.submit(InboundCommand{limitOrder(5, Side::BUY, 100, 1, 2)}, 3 + 1'000);
  st = e.statuses();
  EXPECT_EQ(st.back().status, TradingStatus::Trading);
  EXPECT_EQ(st.back().reason, TradingStatusReason::LuldPauseElapsed);
  EXPECT_EQ(st.back().untilNs, 0) << "a state that does not expire carried an expiry";
}

TEST(VenueEngineSessionEngine, AnOperatorHaltOverAPauseLeavesThePauseRunning)
{
  SymbolConfig c = cfg();
  c.luldBps = 500;
  c.luldHaltNs = DurationNs{1'000};
  Eng e(c);
  e.eng.submit(InboundCommand{limitOrder(1, Side::SELL, 100, 5, 1)}, 1);
  e.eng.submit(InboundCommand{limitOrder(2, Side::BUY, 100, 5, 2)}, 2);
  e.eng.submit(InboundCommand{limitOrder(3, Side::BUY, 200, 1, 2)}, 3);
  ASSERT_EQ(e.eng.tradingStatus(), TradingStatus::LuldPause);
  e.admin(AdminAction::Halt, 4);
  EXPECT_EQ(e.eng.tradingStatus(), TradingStatus::LuldPause)
      << "an operator halt turned a timed pause into an untimed one";
  e.admin(AdminAction::Resume, 5);
  EXPECT_EQ(e.eng.tradingStatus(), TradingStatus::Trading);
}

TEST(VenueEngineSessionEngine, ResumeAuctionClearsThePauseAndEntersPreOpen)
{
  SymbolConfig c = cfg();
  c.luldBps = 500;
  c.luldHaltNs = DurationNs{1'000};
  Eng e(c);
  e.eng.submit(InboundCommand{limitOrder(1, Side::SELL, 100, 5, 1)}, 1);
  e.eng.submit(InboundCommand{limitOrder(2, Side::BUY, 100, 5, 2)}, 2);
  e.eng.submit(InboundCommand{limitOrder(3, Side::BUY, 200, 1, 2)}, 3);
  ASSERT_EQ(e.eng.tradingStatus(), TradingStatus::LuldPause);
  e.admin(AdminAction::ResumeAuction, 4);
  EXPECT_EQ(e.eng.tradingStatus(), TradingStatus::AuctionPreOpen);
  const auto st = e.statuses();
  EXPECT_EQ(st.back().untilNs, 0) << "the pre-open carried the cleared pause deadline";
}

TEST(VenueEngineSessionEngine, RehaltingAnAlreadyHaltedSymbolPublishesNothing)
{
  Eng e;
  e.admin(AdminAction::Halt, 1);
  const size_t after = e.out.size();
  e.admin(AdminAction::Halt, 2);
  EXPECT_EQ(e.out.size(), after);
}

// ---------------------------------------------------------------------------
// The documentation is the table
// ---------------------------------------------------------------------------

TEST(VenueEngineSessionDoc, TheWrittenAutomatonIsTheRunningOne)
{
  const std::string path = FLOX_VENUE_SESSION_DOC;
  std::ifstream in(path);
  ASSERT_TRUE(in.good()) << "cannot read " << path;
  std::stringstream buf;
  buf << in.rdbuf();
  std::string doc = buf.str();

  const size_t open = doc.find(kDocOpen);
  ASSERT_NE(open, std::string::npos) << path << " has no generated session-transition block";
  const size_t bodyAt = open + std::string(kDocOpen).size() + 1;  // past the marker's newline
  const size_t close = doc.find(kDocClose, bodyAt);
  ASSERT_NE(close, std::string::npos) << path << " never closes the generated block";

  const std::string want = generatedTable();
  const std::string have = doc.substr(bodyAt, close - bodyAt);
  if (have != want && std::getenv("FLOX_UPDATE_SESSION_TABLE") != nullptr)
  {
    doc.replace(bodyAt, close - bodyAt, want);
    std::ofstream out(path, std::ios::trunc);
    out << doc;
    std::printf("session transition table rewritten in %s\n", path.c_str());
    return;
  }
  EXPECT_EQ(have, want) << "the table moved and the documentation did not. Rerun with "
                           "FLOX_UPDATE_SESSION_TABLE=1 to regenerate it.";
}
