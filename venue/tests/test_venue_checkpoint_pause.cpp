/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * What the checkpoint pause gauge is worth.
 *
 * sequenced_shard.h:559-561 calls lastCheckpointPauseNs() the "consumer-thread
 * stall of the most recent checkpoint", and an operator sizes a venue's
 * worst-case matching gap from it. The stamp is taken before two things that
 * still run on the consumer thread and still stop matching: the ckptMx_ acquire
 * and the std::async thread spawn that publishes the snapshot, and -- on a
 * checkpoint asked for by name -- the wait for the previous publish and for the
 * checkpoint lane, which are taken before the interval even opens. Everything
 * outside [pause0, stamp] is a stall nobody is charged for.
 *
 * The stall is measured here the only way it can be measured without touching
 * the shard: off the consumer thread's own clock. EngineEventMsg::publishMonoNs
 * is stamped by the consumer immediately before it publishes an event
 * (sequenced_shard.h:607), and the checkpoint hook runs on the consumer thread
 * inside the pause, so the distance between them is consumer-thread time and
 * nothing else.
 */
#include "flox-venue/checkpoint_lane.h"
#include "flox-venue/journal.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/sequenced_shard.h"
#include "support/tmp_path.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace flox;
using namespace flox::venue;
using flox::venue::test::tmpPath;

namespace
{

constexpr SymbolId SYM = 1;
constexpr uint64_t kAcct = 7;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

venue::SymbolConfig cfg()
{
  venue::SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  return c;
}

NewOrder limit(OrderId id, double p)
{
  NewOrder o;
  o.id = id;
  o.symbol = SYM;
  o.side = Side::BUY;
  o.type = OrderType::LIMIT;
  o.price = px(p);
  o.quantity = qty(1.0);
  o.accountId = kAcct;
  return o;
}

// Keeps the consumer's own publish stamp for every acceptance. That stamp is
// taken on the consumer thread, so two of them bracket consumer-thread time --
// not the producer's, and not the subscriber's.
struct StampSink : IEngineEventListener
{
  std::atomic<int64_t> lastAcceptNs{0};
  std::atomic<OrderId> lastAcceptId{0};
  std::atomic<uint64_t> accepts{0};

  void onEngineEvent(const EngineEventMsg& ev) override
  {
    if (const auto* a = std::get_if<OrderAccepted>(&ev.event))
    {
      lastAcceptId.store(a->id, std::memory_order_relaxed);
      lastAcceptNs.store(ev.publishMonoNs, std::memory_order_release);
      accepts.fetch_add(1, std::memory_order_release);
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
    std::filesystem::remove(SequencedShard<>::snapshotPath(base, ts) + ".tmp", ec);
  }
  for (auto ts : g.segments)
  {
    std::filesystem::remove(SequencedShard<>::segmentPath(base, ts), ec);
  }
}

// Tolerance, and why it is this number.
//
// The measurement brackets the checkpoint with two ordinary commands, so it
// picks up their apply plus the ring hand-off around them on top of the pause.
// That residue is not assumed: the test measures it first, on the same shard,
// as the worst of twenty runs of the identical two-command sequence with no
// checkpoint in it (`baselineNs`, ~10 us on an apple-silicon laptop). kSlackNs
// is what the assertion allows ON TOP of that measured residue, for the
// scheduling noise of one round.
//
// 10 us sits between the two quantities that matter: it is comparable to the
// residue it covers, and well under the work the gauge misses today -- a
// std::async thread spawn is tens of microseconds (the median shortfall
// measured here is 35-45 us) and the lane wait in the next test is tens of
// milliseconds. The assertion runs on the MEDIAN of nine rounds rather than
// one sample, so neither a slow round nor a lucky one decides the result.
constexpr int64_t kSlackNs = 10'000;  // 10 us

}  // namespace

// ---------------------------------------------------------------------------
// (1) The tail of the pause: the ckptMx_ acquire and the std::async spawn run
// after the stamp, on the consumer thread, with matching stopped.
//
// The shard is stepped by this thread (setOwnThreads(false)), so the command
// order is exactly A, TimeTick, B and the checkpoint happens at the TimeTick
// boundary -- between the two publish stamps and nowhere else.

TEST(VenueCheckpointPause, TheReportedPauseCoversTheWholeConsumerStall)
{
  const std::string base = tmpPath("venue_pause_spawn", ".bin");
  cleanFiles(base);

  CheckpointConfig ck;
  ck.maxSegmentRecords = 1;  // every sweep finds the threshold crossed
  ck.maxSegmentBytes = 0;

  StampSink sink;
  auto s = std::make_unique<SequencedShard<>>(cfg(), base, MatchingBook{}, Journal::Sync::Off,
                                              &SequencedShard<>::systemNowNs,
                                              /*idleSweepIntervalNs*/ 0, ck);
  s->setOwnThreads(false);
  s->subscribeOutbound(&sink);
  s->start();

  OrderId id = 1;

  // Baseline: the same two commands with no checkpoint between them. This is
  // everything the measurement picks up that is not the pause.
  int64_t baselineNs = 0;
  for (int i = 0; i < 20; ++i)
  {
    s->submit(InboundCommand{limit(id++, 99.0)});
    s->flush();
    const int64_t t0 = sink.lastAcceptNs.load(std::memory_order_acquire);
    s->submit(InboundCommand{TimeTick{SYM}});
    s->submit(InboundCommand{limit(id++, 99.0)});
    s->flush();
    const int64_t t1 = sink.lastAcceptNs.load(std::memory_order_acquire);
    baselineNs = t1 - t0 > baselineNs ? t1 - t0 : baselineNs;
  }

  // The real thing, several times over: the assertion runs on the MEDIAN
  // shortfall, so neither one slow round nor one lucky one decides it.
  constexpr int kRounds = 9;
  std::vector<int64_t> missed;
  for (int r = 0; r < kRounds; ++r)
  {
    // The previous snapshot must be published: an automatic checkpoint that
    // finds one in flight skips instead of pausing, and a skipped round
    // measures nothing.
    const uint64_t taken = s->checkpointsTaken();
    s->submit(InboundCommand{limit(id++, 99.0)});
    s->flush();
    const int64_t before = sink.lastAcceptNs.load(std::memory_order_acquire);

    ASSERT_TRUE(s->sweepOnce());
    s->submit(InboundCommand{limit(id++, 99.0)});
    s->flush();
    const int64_t after = sink.lastAcceptNs.load(std::memory_order_acquire);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (s->checkpointsTaken() == taken && std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::yield();
    }
    ASSERT_GT(s->checkpointsTaken(), taken) << "no checkpoint ran between the two stamps";

    const int64_t stallNs = after - before;
    const int64_t reportedNs = s->lastCheckpointPauseNs();
    const int64_t missedNs = stallNs - reportedNs;
    std::printf("  round %d: stall=%lldns reported=%lldns missed=%lldns\n", r,
                static_cast<long long>(stallNs), static_cast<long long>(reportedNs),
                static_cast<long long>(missedNs));
    missed.push_back(missedNs);
  }
  ASSERT_EQ(missed.size(), static_cast<size_t>(kRounds));
  std::sort(missed.begin(), missed.end());
  const int64_t medianMissedNs = missed[missed.size() / 2];
  std::printf("  baseline=%lldns medianMissed=%lldns budget=%lldns\n",
              static_cast<long long>(baselineNs), static_cast<long long>(medianMissedNs),
              static_cast<long long>(baselineNs + kSlackNs));

  // Everything the consumer spent inside the checkpoint and outside the gauge.
  // Two ordinary command applies and the ring hand-off around them are the
  // only legitimate residue, and `baselineNs` is that residue measured on this
  // very shard.
  EXPECT_LE(medianMissedNs, baselineNs + kSlackNs)
      << "the gauge is short by the work that runs after it is stamped: the ckptMx_ "
         "acquire and the snapshot thread spawn";

  s->stop();
  s.reset();
  cleanFiles(base);
}

// ---------------------------------------------------------------------------
// (2) The head of the pause, made large and deterministic by a lock this test
// holds. A checkpoint asked for by name waits for the lane rather than
// skipping, and that wait is the consumer thread standing still -- taken
// before the interval opens, so none of it is reported.

TEST(VenueCheckpointPause, TheReportedPauseCoversAWaitForAHeldLane)
{
  const std::string base = tmpPath("venue_pause_lane", ".bin");
  cleanFiles(base);

  constexpr int64_t kHoldMs = 60;

  CheckpointLane lane;
  StampSink sink;
  std::atomic<int64_t> hookNs{0};

  auto s = std::make_unique<SequencedShard<>>(cfg(), base, MatchingBook{}, Journal::Sync::Off);
  s->setCheckpointLane(&lane);
  s->subscribeOutbound(&sink);
  // Runs on the consumer thread inside the pause, after the lane was entered
  // and the state cloned.
  s->onCheckpoint([&](int64_t)
                  { hookNs.store(venueMonoNs(), std::memory_order_release); });
  s->start();

  s->submit(InboundCommand{limit(1, 99.0)});
  s->flush();
  const int64_t before = sink.lastAcceptNs.load(std::memory_order_acquire);
  ASSERT_GT(before, 0);

  // Somebody else on this driver is in their pause and will be for kHoldMs.
  ASSERT_TRUE(lane.tryEnter());
  std::thread holder(
      [&]
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(kHoldMs));
        lane.leave();
      });

  ASSERT_TRUE(s->checkpointNow());
  holder.join();

  const int64_t hook = hookNs.load(std::memory_order_acquire);
  ASSERT_GT(hook, 0);
  const int64_t stallNs = hook - before;
  const int64_t reportedNs = s->lastCheckpointPauseNs();
  std::printf("  lane hold=%lldms stall(to hook)=%lldns reported=%lldns\n",
              static_cast<long long>(kHoldMs), static_cast<long long>(stallNs),
              static_cast<long long>(reportedNs));

  // The consumer was demonstrably stopped for at least the hold.
  ASSERT_GE(stallNs, kHoldMs * 1'000'000 - 5'000'000);
  EXPECT_GE(reportedNs, stallNs - 5'000'000)
      << "the gauge starts after the lane wait, so a shard crowded out of the lane reports "
         "a pause it did not have";

  s->stop();
  s.reset();
  cleanFiles(base);
}

// ---------------------------------------------------------------------------
// (3) GREEN CONTROL. The gauge must stay a gauge: positive on every checkpoint,
// never larger than the wall time of the whole operation, and accumulated into
// the total and into the lane the same way. A fix that simply inflates the
// number would break this.

TEST(VenueCheckpointPause, TheGaugeStaysBoundedByTheWallClockAndAccumulates)
{
  const std::string base = tmpPath("venue_pause_control", ".bin");
  cleanFiles(base);

  CheckpointLane lane;
  auto s = std::make_unique<SequencedShard<>>(cfg(), base, MatchingBook{}, Journal::Sync::Off);
  s->setCheckpointLane(&lane);
  s->start();

  s->submit(InboundCommand{limit(1, 99.0)});
  s->flush();

  int64_t total = 0;
  for (int i = 0; i < 3; ++i)
  {
    s->submit(InboundCommand{limit(static_cast<OrderId>(10 + i), 98.0)});
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(s->checkpointNow());
    const int64_t wallNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - t0)
                               .count();
    const int64_t reported = s->lastCheckpointPauseNs();
    EXPECT_GT(reported, 0);
    EXPECT_LE(reported, wallNs) << "the pause cannot be longer than the call that contains it";
    total += reported;
  }

  EXPECT_GE(s->checkpointPauseTotalNs(), total);
  EXPECT_EQ(lane.checkpoints(), 3u);
  EXPECT_GT(lane.pauseMaxNs(), 0);

  s->stop();
  s.reset();
  cleanFiles(base);
}
