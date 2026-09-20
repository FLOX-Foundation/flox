/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * A shard owns three threads: the matching consumer, the outbound
 * subscribers, and the idle sweeper. That is the right shape for a shard on a
 * machine it owns and an impossible one for a process holding hundreds. These
 * tests cover the shard with no threads at all, driven by whoever wants to
 * drive it -- and the two things that then have to be true: nothing happens
 * unless somebody steps it, and everything that used to block on a consumer
 * thread still finishes.
 */
#include "flox-venue/journal.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/sequenced_shard.h"
#include "support/tmp_path.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

using namespace flox;
using namespace flox::venue;
using flox::venue::test::tmpPath;

namespace
{

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

venue::SymbolConfig cfg(SymbolId sym, int64_t lastLookWindowNs = 0)
{
  venue::SymbolConfig c;
  c.id = sym;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  c.baseAsset = 0;
  c.quoteAsset = 1;
  c.lastLookWindowNs = DurationNs{lastLookWindowNs};
  return c;
}

NewOrder limit(SymbolId sym, OrderId id, Side s, double p, double q, uint64_t acct)
{
  NewOrder o;
  o.id = id;
  o.symbol = sym;
  o.side = s;
  o.type = OrderType::LIMIT;
  o.price = px(p);
  o.quantity = qty(q);
  o.accountId = acct;
  return o;
}

struct Sink : IEngineEventListener
{
  std::atomic<int> events{0};
  std::atomic<int> fills{0};
  std::atomic<int> rejects{0};
  void onEngineEvent(const EngineEventMsg& e) override
  {
    events.fetch_add(1);
    if (std::get_if<OrderExecuted>(&e.event))
    {
      fills.fetch_add(1);
    }
    if (std::get_if<FillRejected>(&e.event))
    {
      rejects.fetch_add(1);
    }
  }
};

void cleanFiles(const std::string& base)
{
  std::remove(base.c_str());
  const auto g = SequencedShard<>::scanGenerations(base);
  std::error_code ec;
  for (auto ts : g.snapshots)
  {
    std::filesystem::remove(SequencedShard<>::snapshotPath(base, ts), ec);
  }
  for (auto ts : g.segments)
  {
    std::filesystem::remove(SequencedShard<>::segmentPath(base, ts), ec);
  }
}

// ---------------------------------------------------------------------------

TEST(VenueShardStepping, NothingHappensUntilSomebodyStepsIt)
{
  const std::string base = tmpPath("venue_step_basic", ".bin");
  cleanFiles(base);

  Sink sink;
  auto s = std::make_unique<SequencedShard<>>(cfg(1), base, MatchingBook{}, Journal::Sync::Off);
  s->setOwnThreads(false);
  s->subscribeOutbound(&sink);
  s->start();

  s->submit(InboundCommand{limit(1, 1, Side::SELL, 100.0, 5.0, 1)});
  s->submit(InboundCommand{limit(1, 2, Side::BUY, 100.0, 3.0, 2)});

  // No threads: the commands sit in the ring. Waiting proves it, since a
  // shard with a consumer thread would have matched them in microseconds.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(sink.events.load(), 0);

  while (s->pollOnce())
  {
  }
  // ASSERT, not EXPECT: everything below assumes the stepping worked, and
  // stop() on a shard nobody can step waits for a consumer that does not
  // exist. A test that fails must still end.
  ASSERT_GT(sink.fills.load(), 0);
  const int after = sink.events.load();

  // And a step with nothing to do says so.
  EXPECT_FALSE(s->pollOnce());
  EXPECT_EQ(sink.events.load(), after);

  s->stop();
  s.reset();
  cleanFiles(base);
}

TEST(VenueShardStepping, FlushStepsWhenNobodyElseWill)
{
  const std::string base = tmpPath("venue_step_flush", ".bin");
  cleanFiles(base);

  // Leaked on purpose: if flush() ever stops stepping, the thread below hangs
  // in it forever, and a hung thread that outlives the shard it is using is a
  // crash rather than a failure. The test fails on its deadline instead.
  auto* sink = new Sink();
  auto* s = new SequencedShard<>(cfg(1), base, MatchingBook{}, Journal::Sync::Off);
  s->setOwnThreads(false);
  s->subscribeOutbound(sink);
  s->start();

  s->submit(InboundCommand{limit(1, 1, Side::SELL, 100.0, 5.0, 1)});
  s->submit(InboundCommand{limit(1, 2, Side::BUY, 100.0, 3.0, 2)});

  std::atomic<bool> done{false};
  std::thread worker(
      [&]
      {
        s->flush();  // must step; there is no other thread that could
        done.store(true);
      });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!done.load() && std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const bool finished = done.load();
  if (finished)
  {
    worker.join();
    EXPECT_GT(sink->fills.load(), 0);
    s->stop();
    delete s;
    delete sink;
    cleanFiles(base);
  }
  else
  {
    worker.detach();  // leaked with the shard; the process still exits
  }
  EXPECT_TRUE(finished) << "flush() waited for a consumer thread that does not exist";
}

TEST(VenueShardStepping, CheckpointNowWorksUnderStepping)
{
  const std::string base = tmpPath("venue_step_ckpt", ".bin");
  cleanFiles(base);

  auto* sink = new Sink();
  auto* s = new SequencedShard<>(cfg(1), base, MatchingBook{}, Journal::Sync::Off);
  s->setOwnThreads(false);
  s->subscribeOutbound(sink);
  s->start();
  s->submit(InboundCommand{limit(1, 1, Side::SELL, 100.0, 5.0, 1)});

  std::atomic<bool> done{false};
  std::atomic<bool> ok{false};
  std::thread worker(
      [&]
      {
        ok.store(s->checkpointNow());
        done.store(true);
      });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!done.load() && std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const bool finished = done.load();
  if (finished)
  {
    worker.join();
    EXPECT_TRUE(ok.load());
    EXPECT_EQ(s->checkpointsTaken(), 1u);
    const auto gens = SequencedShard<>::scanGenerations(base);
    EXPECT_EQ(gens.snapshots.size(), 1u);
    s->stop();
    delete s;
    delete sink;
    cleanFiles(base);
  }
  else
  {
    worker.detach();
  }
  EXPECT_TRUE(finished) << "checkpointNow() never returned without a consumer thread";
}

TEST(VenueShardStepping, SweepAndStepExpireAHoldWithNoThreadsAtAll)
{
  const std::string base = tmpPath("venue_step_sweep", ".bin");
  cleanFiles(base);

  auto now = std::make_shared<std::atomic<int64_t>>(1000);
  SequencedShard<>::TimeSource clk = [now]
  { return now->load(); };

  Sink sink;
  auto s = std::make_unique<SequencedShard<>>(cfg(1, /*lastLookWindowNs*/ 5000), base,
                                              MatchingBook{}, Journal::Sync::Off, clk,
                                              /*idleSweepIntervalNs*/ 1000);
  s->setOwnThreads(false);  // the sweeper thread is not started either
  s->subscribeOutbound(&sink);
  s->start();

  NewOrder mk = limit(1, 1, Side::SELL, 100.0, 5.0, 1);
  mk.lastLook = true;
  s->submit(InboundCommand{mk});
  s->submit(InboundCommand{limit(1, 2, Side::BUY, 100.0, 3.0, 2)});
  while (s->pollOnce())
  {
  }
  ASSERT_EQ(sink.rejects.load(), 0);

  // Move time past the window. With no sweeper thread the hold expires only
  // because the driver sweeps.
  now->store(1'000'000);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_EQ(sink.rejects.load(), 0) << "something expired the hold without being asked to";

  EXPECT_TRUE(s->sweepOnce());
  while (s->pollOnce())
  {
  }
  // ASSERT for the same reason as above: stop() flushes, and a flush on a
  // shard that cannot be stepped never returns.
  ASSERT_EQ(sink.rejects.load(), 1);

  s->stop();
  s.reset();
  cleanFiles(base);
}

}  // namespace
