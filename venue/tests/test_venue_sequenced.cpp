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
#include "support/tmp_path.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include <cstdio>
#include <cstdlib>

using namespace flox;
using namespace flox::venue;
using flox::venue::test::tmpPath;

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
  //
  // The shard opens its journal for APPEND and replays it on start (recovery
  // semantics), so a stale file from a previous run must be removed first.
  const std::string journalPath = tmpPath("venue_sequenced_journal", ".bin");
  std::remove(journalPath.c_str());
  auto shard = std::make_unique<SequencedShard<LadderBook>>(
      cfg(), journalPath, LadderBook{ladderCfg()}, Journal::Sync::Off);
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

namespace
{

// A book that refuses to take the Nth resting order. The shard is templated on
// the book, so the throw is injected through the type rather than through a
// hook added to production code for a test's benefit.
class BookThatThrows : public MatchingBook
{
 public:
  BookThatThrows() = default;
  explicit BookThatThrows(int throwOnNth) : throwOnNth_(throwOnNth) {}

  [[nodiscard]] BookAddResult addResting(Side side, const RestingOrder& o)
  {
    if (++adds_ == throwOnNth_)
    {
      throw std::runtime_error("the book refused the order");
    }
    return MatchingBook::addResting(side, o);
  }

 private:
  int throwOnNth_ = 0;
  int adds_ = 0;
};

InboundCommand restingSell(uint64_t id, double price)
{
  NewOrder o;
  o.id = id;
  o.symbol = SYM;
  o.side = Side::SELL;
  o.type = OrderType::LIMIT;
  o.price = Price::fromDouble(price);
  o.quantity = Quantity::fromDouble(1.0);
  o.tif = TimeInForce::GTC;
  o.accountId = 1;
  return InboundCommand{o};
}

}  // namespace

// The engine is not transactional. The journal is written before the command
// is applied, which is the right order: it keeps the durable state recoverable
// whatever happens next. What it does not do is undo a half-applied command in
// memory, so after a throw the shard holds a state that disagrees with the one
// it would recover into.
//
// Worse than that, in fact. Letting the throw leave onCommand does not crash
// the shard, it wedges it: the ingress never advances past the command and
// flush() spins forever. That is why the wait below is bounded -- a hang is
// not a red test, and without the bound this one would only be caught by
// ctest's timeout minutes later.
TEST(Sequenced, AShardThatThrewPartWayThroughStopsInsteadOfServingDivergedState)
{
  const std::string journalPath = tmpPath("venue_sequenced_apply_fail", ".bin");
  std::remove(journalPath.c_str());

  HashSink sink;
  auto shard = std::make_unique<SequencedShard<BookThatThrows>>(
      cfg(), journalPath, BookThatThrows{2}, Journal::Sync::Off);
  ASSERT_TRUE(shard->subscribeOutbound(&sink, true));
  shard->start();

  // Three commands in one batch, the second of which the book refuses. The
  // third matters: it is already in the ingress when the failure happens, so
  // it exercises the consumer's own refusal to keep journaling afterwards --
  // the producer-side rejection cannot help with what is already queued.
  std::atomic<bool> done{false};
  std::thread worker(
      [&]
      {
        shard->submit(restingSell(10, 101.00));
        shard->submit(restingSell(11, 102.00));
        shard->submit(restingSell(12, 103.00));
        shard->flush();
        done.store(true, std::memory_order_release);
      });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!done.load(std::memory_order_acquire))
  {
    // The shard is wedged and the worker is still inside it. Detach, and let
    // the shard leak rather than destroy it under a thread that is using it.
    worker.detach();
    (void)shard.release();
    FAIL() << "flush() did not return: a throw while applying wedged the ingress";
  }
  worker.join();

  EXPECT_TRUE(shard->failed());
  EXPECT_NE(shard->failedAtTs(), 0);

  // Two records: the one that applied, and the one that was written ahead of
  // the apply that threw. The third was dropped rather than written, because a
  // record appended now would describe a transition this engine never made.
  EXPECT_EQ(shard->journaled(), 2u);

  // And from here the shard takes nothing at all.
  EXPECT_EQ(shard->submit(restingSell(13, 104.00)),
            SequencedShard<BookThatThrows>::kSubmitRejected);
  shard->flush();
  EXPECT_EQ(shard->journaled(), 2u);

  shard->stop();
  std::remove(journalPath.c_str());
}
