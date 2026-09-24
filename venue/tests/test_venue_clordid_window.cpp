/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/journal.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "support/tmp_path.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <vector>

using namespace flox;
using namespace flox::venue;
using flox::venue::test::tmpPath;

namespace
{

constexpr SymbolId SYM = 1;
constexpr int64_t kDay = 86'400'000'000'000LL;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

SymbolConfig cfg(int64_t windowNs)
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(1.0);
  c.maxPrice = px(1000.0);
  c.clOrdIdWindowNs = windowNs;
  return c;
}

NewOrder ord(OrderId id, uint64_t clOrdId, uint64_t acct = 1)
{
  NewOrder o;
  o.id = id;
  o.symbol = SYM;
  o.side = Side::BUY;
  o.type = OrderType::LIMIT;
  o.price = px(100.0);
  o.quantity = qty(1.0);
  o.accountId = acct;
  o.clientOrderId = clOrdId;
  return o;
}

struct Venue
{
  std::vector<OutboundEvent> ev;
  MatchingEngine<MatchingBook> eng;
  OrderId nextId{1};

  explicit Venue(int64_t windowNs)
      : eng(cfg(windowNs), [this](const OutboundEvent& e)
            { ev.push_back(e); })
  {
  }

  // Returns true if the order was accepted (not refused as a duplicate).
  bool submit(uint64_t clOrdId, int64_t tsNs)
  {
    ev.clear();
    eng.submit(InboundCommand{ord(nextId++, clOrdId)}, tsNs);
    for (const auto& e : ev)
    {
      if (const auto* r = std::get_if<OrderRejected>(&e))
      {
        if (r->reason == RejectReason::DuplicateClientOrderId)
        {
          return false;
        }
      }
    }
    return true;
  }
};

// The case the dedup exists for, unchanged: a client repeating the same id is
// refused, whatever the window.
TEST(ClOrdIdWindow, ARepeatedIdIsStillRefused)
{
  for (const int64_t window : {int64_t{0}, kDay})
  {
    Venue v{window};
    EXPECT_TRUE(v.submit(4242, 1));
    EXPECT_FALSE(v.submit(4242, 2)) << "window " << window;
  }
}

// The default is what the venue always did: ids are remembered forever. This
// change must not alter behaviour until an operator asks for it.
TEST(ClOrdIdWindow, WithNoWindowAnIdIsRememberedForever)
{
  Venue v{0};
  EXPECT_TRUE(v.submit(7, 1));
  // Several times over, far apart: one repeat would pass even if the window
  // rotated on every touch, because a rotation moves the id to the other half
  // where it still blocks. Only the second rotation drops it.
  EXPECT_FALSE(v.submit(7, 1000 * kDay));
  EXPECT_FALSE(v.submit(7, 2000 * kDay));
  EXPECT_FALSE(v.submit(7, 3000 * kDay));
}

// Past the window the id is free again. Stated rather than discovered:
// exchanges scope client order id uniqueness to the trading day.
TEST(ClOrdIdWindow, PastTheWindowAnOldIdIsAcceptedAgain)
{
  Venue v{kDay};
  EXPECT_TRUE(v.submit(7, 1));
  // One rotation moves it to the previous half, where it still blocks.
  EXPECT_FALSE(v.submit(7, kDay + 1));
  // A second rotation drops that half entirely.
  EXPECT_TRUE(v.submit(7, 2 * kDay + 2));
}

// Inside the window it blocks, and the rotation does not cut the protection
// short: an id used just before a boundary survives into the next window.
TEST(ClOrdIdWindow, AnIdUsedJustBeforeARotationStillBlocksAfterIt)
{
  Venue v{kDay};
  EXPECT_TRUE(v.submit(7, kDay - 1));
  EXPECT_FALSE(v.submit(7, kDay + 1)) << "the protection was cut short by a rotation";
}

// The window is per account: one client's ids say nothing about another's.
TEST(ClOrdIdWindow, TheWindowIsPerAccount)
{
  Venue v{kDay};
  v.ev.clear();
  v.eng.submit(InboundCommand{ord(1, 99, /*acct=*/1)}, 1);
  v.ev.clear();
  v.eng.submit(InboundCommand{ord(2, 99, /*acct=*/2)}, 2);

  bool refused = false;
  for (const auto& e : v.ev)
  {
    if (const auto* r = std::get_if<OrderRejected>(&e))
    {
      refused = refused || r->reason == RejectReason::DuplicateClientOrderId;
    }
  }
  EXPECT_FALSE(refused) << "another account's id was taken for this one's";
}

// Memory is what this is for. After many windows of distinct ids the engine
// holds two windows' worth, not all of them -- and the state hash is what
// proves the older ones are gone rather than merely unreachable.
TEST(ClOrdIdWindow, OldGenerationsAreActuallyDropped)
{
  Venue bounded{kDay};
  uint64_t id = 1;
  for (int day = 0; day < 10; ++day)
  {
    for (int i = 0; i < 20; ++i)
    {
      bounded.submit(id++, static_cast<int64_t>(day) * kDay + i + 1);
    }
  }

  // An id from the first day is long gone: accepted again.
  EXPECT_TRUE(bounded.submit(1, 10 * kDay + 100));
  // One from the current window is not.
  EXPECT_FALSE(bounded.submit(id - 1, 10 * kDay + 101));
}

// Replay is the contract. The same commands at the same timestamps produce
// the same state, rotations included.
TEST(ClOrdIdWindow, ReplayReproducesTheSameWindowState)
{
  const auto run = []
  {
    Venue v{kDay};
    uint64_t id = 1;
    for (int day = 0; day < 4; ++day)
    {
      for (int i = 0; i < 5; ++i)
      {
        v.submit(id++, static_cast<int64_t>(day) * kDay + i + 1);
      }
    }
    return v.eng.stateHash();
  };

  EXPECT_EQ(run(), run());
}

// Two different splits of the same ids are different states: the split
// decides which id is forgotten next, so the hash has to see it.
TEST(ClOrdIdWindow, TheSplitBetweenGenerationsIsPartOfTheState)
{
  Venue a{kDay};
  a.submit(1, 1);
  a.submit(2, 2);  // both in the current half

  Venue b{kDay};
  b.submit(1, 1);
  b.submit(2, kDay + 1);  // 1 rotated to the previous half, 2 in the current

  EXPECT_NE(a.eng.stateHash(), b.eng.stateHash())
      << "the generation split is invisible to the state hash";
}

// Two windows holding the same ids in the same halves are still different
// states if they rotated at different moments: the next rotation lands
// elsewhere, so a future id is forgotten at a different point.
TEST(ClOrdIdWindow, WhenTheWindowRotatedIsPartOfTheState)
{
  // Both engines end at the same clock, so the engine's own time cannot be
  // what separates them -- only the account's rotation moment can. An earlier
  // version of this test let the engine clock do the work and passed with the
  // rotation time left out of the hash entirely.
  const auto atSameClock = [](int64_t firstTouchNs)
  {
    Venue v{kDay};
    v.eng.submit(InboundCommand{ord(1, 11, /*acct=*/1)}, firstTouchNs);
    v.eng.submit(InboundCommand{ord(2, 22, /*acct=*/2)}, 100);
    return v.eng.stateHash();
  };

  EXPECT_NE(atSameClock(1), atSameClock(50))
      << "the rotation time is invisible to the state hash";
}

// The rotation moment is state the hash folds, so it has to ride the
// snapshot too: without it a recovered engine rebuilds every id but rotates
// on a different schedule, and SnapshotEnd refuses the file it was handed.
// With clOrdIdWindowNs at its default of 0 nothing rotates and the hole is
// invisible, which is why this test sets a window.
TEST(ClOrdIdWindow, ASnapshotOfARotatedWindowStillLoads)
{
  const std::string path = tmpPath("clordid_window_snapshot", ".snap");
  std::remove(path.c_str());

  Venue v{kDay};
  v.eng.submit(InboundCommand{ord(1, 11, /*acct=*/1)}, 1);
  v.eng.submit(InboundCommand{ord(2, 22, /*acct=*/1)}, kDay + 1);  // rotates
  {
    Journal out(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    v.eng.writeSnapshot(out);
    out.flush();
  }

  std::vector<OutboundEvent> ev;
  MatchingEngine<MatchingBook> rec(cfg(kDay), [&](const OutboundEvent& e)
                                   { ev.push_back(e); });
  const auto records = Journal::loadTimed(path);
  ASSERT_GE(records.size(), 2u);
  for (const auto& [ts, cmd] : records)
  {
    EXPECT_TRUE(rec.applySnapshotRecord(cmd, ts)) << "record " << cmd.index();
  }
  EXPECT_EQ(rec.stateHash(), v.eng.stateHash());
  // And the restored window rotates on the writer's schedule, not on a fresh
  // one: the id from the current half is still blocked just before the next
  // boundary and free just past the one after it.
  ev.clear();
  rec.submit(InboundCommand{ord(3, 22, /*acct=*/1)}, 2 * kDay - 1);
  bool refused = false;
  for (const auto& e : ev)
  {
    if (const auto* r = std::get_if<OrderRejected>(&e))
    {
      refused = refused || r->reason == RejectReason::DuplicateClientOrderId;
    }
  }
  EXPECT_TRUE(refused) << "the restored window rotated on its own schedule";
  std::remove(path.c_str());
}

// The half the writer was on matters as much as the ids. Here the current
// half is empty (the repeat found the id in the previous one and inserted
// nothing), so the file carries a single generation-1 batch -- and the
// rotation moment has to ride it, or the recovered engine rotates on a
// schedule of its own.
TEST(ClOrdIdWindow, ASnapshotWithOnlyThePreviousHalfPopulatedStillLoads)
{
  const std::string path = tmpPath("clordid_window_prev_only", ".snap");
  std::remove(path.c_str());

  Venue v{kDay};
  v.eng.submit(InboundCommand{ord(1, 11, /*acct=*/1)}, 1);
  v.eng.submit(InboundCommand{ord(2, 11, /*acct=*/1)}, kDay + 1);  // rotates, then finds it in prev

  {
    Journal out(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    v.eng.writeSnapshot(out);
    out.flush();
  }

  MatchingEngine<MatchingBook> rec(cfg(kDay), [](const OutboundEvent&) {});
  for (const auto& [ts, cmd] : Journal::loadTimed(path))
  {
    EXPECT_TRUE(rec.applySnapshotRecord(cmd, ts)) << "record " << cmd.index();
  }
  EXPECT_EQ(rec.stateHash(), v.eng.stateHash());
  std::remove(path.c_str());
}

}  // namespace
