/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Replaying a window of journal history.
 *
 * The thing under test is not "does it produce events" -- it is whether the
 * events it produces are the venue's own. A replay that diverges and then
 * converges would still print a plausible-looking fill, and a plausible fill
 * is exactly what a disputed one looks like. So every check here compares the
 * replay against a live engine fed the same commands: same digest over the
 * whole stream, same events inside the window.
 */
#include "flox-venue/journal.h"
#include "flox-venue/journal_window.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "support/tmp_path.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace flox;
using namespace flox::venue;
using flox::venue::test::tmpPath;

namespace
{

constexpr SymbolId SYM = 1;
constexpr uint64_t kSubject = 7;  // the account under investigation
constexpr uint64_t kOther = 8;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  return c;
}

NewOrder limit(OrderId id, Side s, double p, double q, uint64_t acct)
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

constexpr int64_t kWindowFrom = 1500;
constexpr int64_t kWindowTo = 2500;

// The episode both the live run and the replay are fed. Timestamps are chosen
// so the disputed trade sits strictly inside the window and there is traffic
// on either side of it.
struct Script
{
  std::vector<std::pair<int64_t, InboundCommand>> cmds;
};

Script script()
{
  Script s;
  int64_t ts = 1000;
  // Before the window: another pair trades, and the subject rests an order.
  s.cmds.emplace_back(ts += 10, InboundCommand{limit(1, Side::SELL, 100, 1, kOther)});
  s.cmds.emplace_back(ts += 10, InboundCommand{limit(2, Side::BUY, 100, 1, kOther)});
  s.cmds.emplace_back(ts += 10, InboundCommand{limit(3, Side::SELL, 101, 2, kSubject)});
  // Inside the window: the disputed fill, plus traffic that is not the
  // subject's.
  s.cmds.emplace_back(ts = 2000, InboundCommand{limit(4, Side::BUY, 101, 2, kOther)});
  s.cmds.emplace_back(ts = 2100, InboundCommand{limit(5, Side::SELL, 99, 1, kOther)});
  s.cmds.emplace_back(ts = 2200, InboundCommand{limit(6, Side::BUY, 99, 1, kOther)});
  // Exactly on the upper edge. The window is inclusive at both ends, and an
  // edge with nothing on it cannot tell an inclusive bound from an exclusive
  // one -- which is how the first version of this test missed it.
  s.cmds.emplace_back(ts = kWindowTo, InboundCommand{limit(8, Side::SELL, 103, 1, kSubject)});
  // After the window.
  s.cmds.emplace_back(ts = 3000, InboundCommand{limit(7, Side::SELL, 102, 1, kSubject)});
  return s;
}

// A live run: the same commands into an engine, digesting every event the way
// the replay does.
struct Live
{
  uint64_t digest{0};
  uint64_t events{0};
  std::vector<std::pair<int64_t, OutboundEvent>> inWindow;
};

Live runLive(const Script& s)
{
  Live live;
  int64_t cur = 0;
  MatchingEngine<MatchingBook> eng(cfg(),
                                   [&](const OutboundEvent& e)
                                   {
                                     ++live.events;
                                     live.digest = hashEvent(live.digest, e);
                                     if (cur >= kWindowFrom && cur <= kWindowTo)
                                     {
                                       live.inWindow.emplace_back(cur, e);
                                     }
                                   });
  for (const auto& [ts, cmd] : s.cmds)
  {
    cur = ts;
    eng.submit(cmd, ts);
  }
  return live;
}

std::string writeSegment(const Script& s)
{
  const std::string path = tmpPath("window-seg");
  Journal j(path, Journal::Sync::Off);
  for (const auto& [ts, cmd] : s.cmds)
  {
    j.append(cmd, ts);
  }
  return path;
}

}  // namespace

// The digest is the claim. Equal digests mean the replay took the same path
// the venue took; without that the windowed extract is just a story.
TEST(JournalWindow, ReplayReproducesTheLiveRunsWholeStream)
{
  const Script s = script();
  const Live live = runLive(s);
  const std::string seg = writeSegment(s);

  const WindowResult r =
      replayWindow(cfg(), /*snapshot=*/"", {seg}, WindowQuery{kWindowFrom, kWindowTo, /*account=*/0});

  EXPECT_EQ(r.recordsReplayed, s.cmds.size());
  EXPECT_EQ(r.eventsTotal, live.events);
  EXPECT_EQ(r.streamDigest, live.digest)
      << "the replay diverged from the run that wrote the journal";
  std::remove(seg.c_str());
}

TEST(JournalWindow, TheWindowHoldsExactlyTheEventsTheLiveRunProducedInIt)
{
  const Script s = script();
  const Live live = runLive(s);
  const std::string seg = writeSegment(s);

  const WindowResult r =
      replayWindow(cfg(), "", {seg}, WindowQuery{kWindowFrom, kWindowTo, /*account=*/0});

  ASSERT_EQ(r.events.size(), live.inWindow.size());
  for (size_t i = 0; i < r.events.size(); ++i)
  {
    EXPECT_EQ(r.events[i].ts, live.inWindow[i].first);
    uint64_t a = 0;
    uint64_t b = 0;
    EXPECT_EQ(hashEvent(a, r.events[i].event), hashEvent(b, live.inWindow[i].second))
        << "event " << i << " differs from the one the live run emitted";
  }
  std::remove(seg.c_str());
}

// The filter answers "what happened to account N", so it must not hand back
// another account's business, and must not attribute a public print to
// whichever side is being asked about.
TEST(JournalWindow, TheAccountFilterReturnsOnlyThatAccountsReports)
{
  const Script s = script();
  const std::string seg = writeSegment(s);

  const WindowResult all = replayWindow(cfg(), "", {seg}, WindowQuery{kWindowFrom, kWindowTo, 0});
  const WindowResult mine =
      replayWindow(cfg(), "", {seg}, WindowQuery{kWindowFrom, kWindowTo, kSubject});

  EXPECT_EQ(mine.eventsInWindow, all.eventsInWindow) << "the filter narrows the answer, not the scan";
  EXPECT_LT(mine.events.size(), all.events.size());
  EXPECT_FALSE(mine.events.empty()) << "the subject did trade inside the window";
  for (const auto& we : mine.events)
  {
    EXPECT_EQ(accountOf(we.event), kSubject);
    EXPECT_GE(we.ts, kWindowFrom);
    EXPECT_LE(we.ts, kWindowTo);
  }

  // An event on the upper edge belongs to the window: the bound is inclusive
  // at both ends, and an operator asked for "up to t2" means up to and
  // including it.
  bool sawEdge = false;
  for (const auto& we : all.events)
  {
    sawEdge = sawEdge || we.ts == kWindowTo;
  }
  EXPECT_TRUE(sawEdge) << "nothing came back from the last instant of the window";

  // An account that never traded gets nothing back, even though the window is
  // full of other people's business. A public print names two sides and
  // belongs to neither: attributing it to whoever is asking would read as fact
  // in a dispute.
  const WindowResult stranger =
      replayWindow(cfg(), "", {seg}, WindowQuery{kWindowFrom, kWindowTo, /*account=*/4242});
  EXPECT_GT(stranger.eventsInWindow, 0u) << "the window is not empty";
  EXPECT_TRUE(stranger.events.empty()) << "but none of it is this account's";
  std::remove(seg.c_str());
}

// An event that names no single account must report none. A public trade
// print names two sides and belongs to neither; a funding or status event
// belongs to the venue. Returning any account for those would put another
// party's business into someone's dispute extract, and no filter downstream
// can undo that -- which is why this is asserted on the function rather than
// through the pipeline, where a wrong answer only shows up for one particular
// account.
TEST(JournalWindow, AnEventWithNoOwnerReportsNoAccount)
{
  Trade t;
  t.tradeId = 1;
  t.symbol = SYM;
  t.price = px(100);
  t.quantity = qty(1);
  t.makerId = 3;
  t.takerId = 4;
  t.makerAccount = kSubject;
  t.takerAccount = kOther;
  EXPECT_EQ(accountOf(OutboundEvent{t}), 0u)
      << "a print with two sides was attributed to one of them";

  OrderAccepted a;
  a.id = 1;
  a.account = kSubject;
  EXPECT_EQ(accountOf(OutboundEvent{a}), kSubject) << "an order report does have an owner";
}

// A file this build cannot read is refused by name, not read partially. An
// operator settling a dispute must never be handed a short history that looks
// complete.
TEST(JournalWindow, AForeignFormatVersionThrowsRatherThanReturningAPrefix)
{
  const Script s = script();
  const std::string seg = writeSegment(s);

  // The stamp byte follows the 8-byte timestamp of the first record. Bit 7
  // marks "versioned"; the low bits carry the version. Set a version no build
  // has.
  {
    std::FILE* f = std::fopen(seg.c_str(), "r+b");
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(std::fseek(f, 8, SEEK_SET), 0);
    const unsigned char foreign = 0x80 | 0x3f;
    ASSERT_EQ(std::fwrite(&foreign, 1, 1, f), 1u);
    std::fclose(f);
  }

  bool threw = false;
  std::string what;
  try
  {
    (void)replayWindow(cfg(), "", {seg}, WindowQuery{kWindowFrom, kWindowTo, 0});
  }
  catch (const JournalFormatError& e)
  {
    threw = true;
    what = e.what();
  }
  EXPECT_TRUE(threw) << "a foreign version must not come back as a shorter history";
  EXPECT_NE(what.find("version"), std::string::npos) << "and it must name what it found: " << what;
  std::remove(seg.c_str());
}

// A window replayed over a pro-rata instrument.
//
// The extract is evidence, and what makes it evidence is that the replay
// resolves fills under the rule the venue actually ran. The allocation rule
// travels on the config, so replayWindow has to read it: a pro-rata segment
// replayed under price-time hands a disputing counterparty an allocation the
// venue never made -- the same three makers, different sizes, and nothing in
// the answer says which rule produced it.
//
// One level of 2 + 3 + 5 met by a buyer for 5. Pro-rata gives every maker
// half of what it showed (1.0 / 1.5 / 2.5); price-time gives the first two
// everything and the third nothing.
TEST(JournalWindow, AProRataSegmentIsReplayedUnderProRata)
{
  SymbolConfig c = cfg();
  c.matchPolicy = MatchPolicy::ProRata;

  Script s;
  s.cmds.emplace_back(2000, InboundCommand{limit(1, Side::SELL, 100, 2, kOther)});
  s.cmds.emplace_back(2010, InboundCommand{limit(2, Side::SELL, 100, 3, kOther)});
  s.cmds.emplace_back(2020, InboundCommand{limit(3, Side::SELL, 100, 5, kOther)});
  s.cmds.emplace_back(2030, InboundCommand{limit(4, Side::BUY, 100, 5, kSubject)});

  const std::string seg = writeSegment(s);
  const WindowResult r = replayWindow(c, "", {seg}, WindowQuery{kWindowFrom, kWindowTo, 0});
  std::remove(seg.c_str());

  EXPECT_EQ(r.recordsReplayed, s.cmds.size());

  std::vector<std::pair<OrderId, Quantity>> fills;
  int trades = 0;
  for (const auto& we : r.events)
  {
    if (const auto* t = std::get_if<Trade>(&we.event); t != nullptr)
    {
      ++trades;
      bool seen = false;
      for (auto& [id, q] : fills)
      {
        if (id == t->makerId)
        {
          q += t->quantity;
          seen = true;
        }
      }
      if (!seen)
      {
        fills.emplace_back(t->makerId, t->quantity);
      }
    }
  }

  ASSERT_EQ(trades, 3) << "pro-rata resolves every maker at the level, price-time stops early: "
                          "the window was replayed under the wrong allocation rule";
  ASSERT_EQ(fills.size(), 3u);
  for (const auto& [id, q] : fills)
  {
    const Quantity want = id == 1 ? qty(1.0) : (id == 2 ? qty(1.5) : qty(2.5));
    EXPECT_EQ(q, want) << "maker " << id << " was allocated a size this venue never gave it";
  }
}
