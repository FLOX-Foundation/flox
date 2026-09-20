/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * What a checkpoint costs the THREAD, not the shard.
 *
 * A checkpoint pauses its shard while the book is cloned and the journal
 * rotated. One shard on one thread pays that alone. K shards sharing a
 * driver all stop for it, so the number that matters is not one shard's
 * pause but the time the driver spent stopped -- and until a lane exists,
 * that time also includes waiting for the previous snapshot to reach the
 * disk.
 *
 * Usage: bench_venue_checkpoint_lane [shards] [checkpoints-per-shard]
 */
#include "flox-venue/checkpoint_lane.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/sequenced_shard.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{

// Small rings: this measures pauses, not throughput, and 70 shards with the
// default rings would be gigabytes of ring nobody publishes into.
using Shard = SequencedShard<MatchingBook, 1 << 12, 1 << 12>;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

venue::SymbolConfig cfg(SymbolId sym)
{
  venue::SymbolConfig c;
  c.id = sym;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  c.baseAsset = 0;
  c.quoteAsset = 1;
  return c;
}

NewOrder limit(SymbolId sym, OrderId id, double p, double q)
{
  NewOrder o;
  o.id = id;
  o.symbol = sym;
  o.side = (id % 2) == 0 ? Side::BUY : Side::SELL;
  o.type = OrderType::LIMIT;
  o.price = px(p);
  o.quantity = qty(q);
  o.accountId = 1 + (id % 4);
  return o;
}

Shard::TimeSource stepClock()
{
  auto t = std::make_shared<std::atomic<int64_t>>(1000);
  return [t]()
  { return t->fetch_add(1000) + 1000; };
}

struct Run
{
  int64_t driverStoppedNs{0};  // time a driver over these shards would be stopped
  int64_t worstPauseNs{0};
  uint64_t taken{0};
  uint64_t skipped{0};
  double wallMs{0};
};

// `useLane == false` reproduces the shape before the lane existed: every
// shard pauses whenever it likes, and waits for its own previous snapshot
// inside the pause.
Run measure(int shards, int perShard, bool useLane, int ordersPerSegment)
{
  const std::string dir =
      (std::filesystem::temp_directory_path() / "flox_bench_ckpt_lane").string();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);

  CheckpointLane lane;
  CheckpointConfig ck;
  ck.maxSegmentRecords = static_cast<uint64_t>(ordersPerSegment);
  ck.maxSegmentBytes = 0;
  ck.retainGenerations = 2;

  std::vector<std::unique_ptr<Shard>> sh;
  sh.reserve(static_cast<size_t>(shards));
  for (int i = 0; i < shards; ++i)
  {
    const std::string base = dir + "/s" + std::to_string(i) + ".bin";
    sh.push_back(std::make_unique<Shard>(cfg(static_cast<SymbolId>(i + 1)), base, MatchingBook{},
                                         Journal::Sync::Off, stepClock(),
                                         /*idleSweepIntervalNs*/ 0, ck));
    if (useLane)
    {
      sh.back()->setCheckpointLane(&lane);
    }
    sh.back()->start();
  }

  Run r;
  const auto t0 = std::chrono::steady_clock::now();

  // One thread, going round the shards: the whole point is what such a thread
  // pays. Feed each shard a segment's worth of orders, then sweep -- which is
  // where the automatic checkpoint is asked for.
  OrderId id = 1;
  for (int round = 0; round < perShard; ++round)
  {
    for (int i = 0; i < shards; ++i)
    {
      for (int k = 0; k <= ordersPerSegment; ++k)
      {
        sh[static_cast<size_t>(i)]->submit(
            InboundCommand{limit(static_cast<SymbolId>(i + 1), id++, 99.0 + (k % 7) * 0.01, 1.0)});
      }
      sh[static_cast<size_t>(i)]->flush();
    }
    // Ask every shard for its checkpoint in the same pass: equally loaded
    // shards crossing their thresholds together is exactly the case this is
    // about.
    for (int pass = 0; pass < 8; ++pass)
    {
      for (int i = 0; i < shards; ++i)
      {
        sh[static_cast<size_t>(i)]->sweepOnce();
        sh[static_cast<size_t>(i)]->flush();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  r.wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

  for (int i = 0; i < shards; ++i)
  {
    auto& s = *sh[static_cast<size_t>(i)];
    r.taken += s.checkpointsTaken();
    r.skipped += s.checkpointsSkippedBusy();
    const int64_t p = s.lastCheckpointPauseNs();
    r.worstPauseNs = p > r.worstPauseNs ? p : r.worstPauseNs;
  }
  if (useLane)
  {
    r.driverStoppedNs = lane.pauseTotalNs();
    r.worstPauseNs = lane.pauseMaxNs();
  }
  else
  {
    // No lane to add them up, so the shards are asked directly. A driver over
    // them would have sat through every one of these pauses in turn.
    r.driverStoppedNs = 0;
    for (int i = 0; i < shards; ++i)
    {
      r.driverStoppedNs += sh[static_cast<size_t>(i)]->checkpointPauseTotalNs();
    }
  }

  for (auto& s : sh)
  {
    s->stop();
  }
  sh.clear();
  std::filesystem::remove_all(dir, ec);
  return r;
}

}  // namespace

int main(int argc, char** argv)
{
  const int perShard = argc > 2 ? std::atoi(argv[2]) : 2;
  const int ordersPerSegment = argc > 3 ? std::atoi(argv[3]) : 200;

  std::vector<int> ks;
  if (argc > 1)
  {
    ks.push_back(std::atoi(argv[1]));
  }
  else
  {
    ks = {1, 10, 70};
  }

  std::printf("шардов  полоса  остановка-водителя-мс  худшая-пауза-мкс  снимков  пропущено\n");
  for (int k : ks)
  {
    for (bool lane : {false, true})
    {
      const Run r = measure(k, perShard, lane, ordersPerSegment);
      std::printf("%6d  %6s  %20.1f  %16.1f  %7llu  %9llu\n", k, lane ? "есть" : "нет",
                  double(r.driverStoppedNs) / 1e6, double(r.worstPauseNs) / 1e3,
                  (unsigned long long)r.taken, (unsigned long long)r.skipped);
    }
  }
  return 0;
}
