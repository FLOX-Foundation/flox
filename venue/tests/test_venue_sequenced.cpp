/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/event_hash.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/sequenced_shard.h"
#include "flox-venue/workload.h"
#include "flox/book/ladder_book.h"

#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>

#include <cstdio>
#include <cstdlib>

using namespace flox;
using namespace flox::venue;

namespace
{
int g_failures = 0;
int g_checks = 0;
void check(bool ok, const char* e, int line)
{
  ++g_checks;
  if (!ok)
  {
    ++g_failures;
    std::printf("  FAIL line %d: %s\n", line, e);
  }
}
#define CHECK(x) check((x), #x, __LINE__)

constexpr SymbolId SYM = 1;

venue::SymbolConfig cfg()
{
  venue::SymbolConfig c;
  c.id = SYM;
  c.tickSize = Price::fromDouble(0.01);
  c.minPrice = Price::fromDouble(50.0);
  c.maxPrice = Price::fromDouble(150.0);
  return c;
}

LadderBook::Config ladderCfg()
{
  return LadderBook::Config{0, Price::fromDouble(0.01).raw(), 16000, 1 << 20};
}

uint64_t referenceHash(const std::vector<InboundCommand>& cmds)
{
  uint64_t h = 1469598103934665603ULL;
  MatchingEngine<LadderBook> eng(cfg(), [&](const OutboundEvent& e)
                                 { h = hashEvent(h, e); }, LadderBook{ladderCfg()});
  for (const auto& c : cmds)
  {
    eng.submit(c);
  }
  return h;
}

struct HashSink : IEngineEventListener
{
  uint64_t h = 1469598103934665603ULL;
  uint64_t count = 0;
  void onEngineEvent(const EngineEventMsg& e) override
  {
    h = hashEvent(h, e.event);
    ++count;
  }
};

}  // namespace

TEST(Sequenced, EngineSuite)
{
  workload::Params p;
  p.symbol = SYM;
  p.count = 500'000;
  const auto cmds = workload::symmetricLimits(p);

  const uint64_t hRef = referenceHash(cmds);

  HashSink sink;
  // Heap-allocated: the shard embeds the Disruptor ring storage (large).
  // Throughput benchmark of the Disruptor path: journal in Sync::Off so the
  // measurement is the ring + engine, not 500k fsyncs. Durability itself is
  // covered by the journal round-trip / torn-tail tests.
  auto shard = std::make_unique<SequencedShard<LadderBook>>(
      cfg(), "/tmp/flox_test_venue_sequenced_seq_journal.bin", LadderBook{ladderCfg()},
      Journal::Sync::Off);
  CHECK(shard->subscribeOutbound(&sink, true));
  shard->start();

  const auto t0 = std::chrono::steady_clock::now();
  for (const auto& c : cmds)
  {
    shard->submit(c);
  }
  shard->flush();
  const auto t1 = std::chrono::steady_clock::now();

  const uint64_t hSeq = sink.h;
  const uint64_t journaled = shard->journaled();
  shard->stop();

  const double sec = std::chrono::duration<double>(t1 - t0).count();
  std::printf("sequenced shard: %zu commands in %.3fs = %.0f cmd/s end-to-end\n", cmds.size(), sec,
              static_cast<double>(cmds.size()) / sec);
  std::printf("outbound events: %llu, journaled: %llu\n",
              static_cast<unsigned long long>(sink.count),
              static_cast<unsigned long long>(journaled));

  CHECK(journaled == cmds.size());
  CHECK(hSeq == hRef);  // Disruptor path == direct path, bit-for-bit

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
