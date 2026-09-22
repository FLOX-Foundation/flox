/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * A hold whose legs were named by their submitters has to come back named.
 *
 * RestoreHeld carries both client order ids, writeSnapshot writes them and
 * stateHash folds a non-zero one in -- but the restore side did not copy
 * them. The consequence is not a cosmetic one: the reconstructed state hashes
 * to something the writer never measured, SnapshotEnd refuses the file and
 * the whole generation is discarded as corrupt. A venue with last look and
 * clientOrderId could therefore not restore any checkpoint taken with a hold
 * open.
 *
 * The checkpoint here goes through `cloneForSnapshot`, the path production
 * actually takes (SequencedShard::doCheckpoint clones the live engine and
 * serializes the clone on a background thread, see
 * venue/include/flox-venue/sequenced_shard.h), the same way
 * test_venue_checkpoint_delisted.cpp does.
 *
 * The comparison is against the same run with NO checkpoint in it: the
 * decision's whole event tail, folded through event_hash.h, has to be the
 * identical digest. A report that comes back under a zero id is a different
 * digest, in both directions of the decision.
 */
#include "flox-venue/event_hash.h"
#include "flox-venue/journal.h"
#include "flox-venue/ledger.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "support/tmp_path.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace flox;
using namespace flox::venue;
using flox::venue::test::tmpPath;

namespace
{

constexpr SymbolId SYM = 1;
constexpr AssetId BASE = 0;
constexpr AssetId QUOTE = 1;
constexpr uint64_t VENUE_ACCT = 900;
constexpr uint64_t MAKER_ACCT = 2;
constexpr uint64_t TAKER_ACCT = 1;
constexpr OrderId MAKER_ID = 10;
constexpr OrderId TAKER_ID = 11;
constexpr uint64_t MAKER_CLORD = 7001;
constexpr uint64_t TAKER_CLORD = 7002;
constexpr uint64_t kDigestSeed = 1469598103934665603ULL;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }
int64_t baseRaw(double v) { return static_cast<int64_t>(amountOf(qty(v))); }
int64_t quoteRaw(double v) { return static_cast<int64_t>(amountOf(Volume::fromDouble(v))); }

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(50.0);
  c.maxPrice = px(150.0);
  c.baseAsset = BASE;
  c.quoteAsset = QUOTE;
  // Wide enough that the window never decides anything: every hold in this
  // file is resolved by an explicit decision.
  c.lastLookWindowNs = DurationNs{1'000'000'000};
  c.lastLookAcceptOnTimeout = false;
  return c;
}

// Everything the engine said, plus the running digest of it. Cleared at the
// checkpoint boundary so the comparison covers the decision's tail only --
// the half where the restored engine has to speak for itself.
struct Sink
{
  std::vector<OutboundEvent> events;
  uint64_t digest{kDigestSeed};

  void operator()(const OutboundEvent& e)
  {
    events.push_back(e);
    digest = hashEvent(digest, e);
  }

  void reset()
  {
    events.clear();
    digest = kDigestSeed;
  }
};

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

struct Outcome
{
  uint64_t stream{};                // digest of the decision's event tail
  std::vector<OutboundEvent> tail;  // and the events behind it
  uint64_t stateHash{};
};

template <class T>
const T* findEvent(const std::vector<OutboundEvent>& evs)
{
  for (const OutboundEvent& e : evs)
  {
    if (const auto* x = std::get_if<T>(&e))
    {
      return x;
    }
  }
  return nullptr;
}

// Maker quote (named), taker that lifts all of it (named): one open hold whose
// two legs both carry a client order id.
void openHold(MatchingEngine<MatchingBook>& eng)
{
  eng.submit(InboundCommand{Deposit{MAKER_ACCT, BASE, baseRaw(100.0), SYM}}, 1000);
  eng.submit(InboundCommand{Deposit{TAKER_ACCT, QUOTE, quoteRaw(100000.0), SYM}}, 2000);

  NewOrder maker = limit(MAKER_ID, Side::SELL, 100.0, 5.0, MAKER_ACCT);
  maker.lastLook = true;
  maker.clientOrderId = MAKER_CLORD;
  eng.submit(InboundCommand{maker}, 3000);

  NewOrder taker = limit(TAKER_ID, Side::BUY, 100.0, 5.0, TAKER_ACCT);
  taker.clientOrderId = TAKER_CLORD;
  eng.submit(InboundCommand{taker}, 4000);
}

// `viaSnapshot`: take the checkpoint the production way (clone -> journal ->
// fresh engine) between the hold and the decision, and let the RESTORED engine
// answer the maker. Otherwise the live engine answers it, and that run is the
// reference the restored one is compared against.
void runScenario(bool viaSnapshot, bool accept, Outcome& out)
{
  Ledger led;
  Sink sink;
  MatchingEngine<MatchingBook> eng(cfg(), [&](const OutboundEvent& e)
                                   { sink(e); });
  eng.setLedger(&led, VENUE_ACCT);
  openHold(eng);

  const FillHeld* held = findEvent<FillHeld>(sink.events);
  ASSERT_NE(held, nullptr) << "no hold opened: the scenario stopped testing what it says";
  EXPECT_EQ(held->clientOrderId, TAKER_CLORD);
  const uint64_t heldId = held->heldId;
  const InboundCommand decision{LastLookDecision{heldId, SYM, accept, MAKER_ACCT}};

  if (!viaSnapshot)
  {
    sink.reset();
    eng.submit(decision, 5000);
    out = Outcome{sink.digest, sink.events, eng.stateHash()};
    return;
  }

  const std::string path = tmpPath("venue_checkpoint_held_clordid", ".snap");
  std::remove(path.c_str());
  auto clone = eng.cloneForSnapshot();
  ASSERT_NE(clone.engine, nullptr);
  EXPECT_EQ(clone.engine->stateHash(), eng.stateHash());
  {
    Journal out2(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    clone.engine->writeSnapshot(out2);
    out2.flush();
  }

  Ledger led2;
  Sink restored;
  MatchingEngine<MatchingBook> rec(cfg(), [&](const OutboundEvent& e)
                                   { restored(e); });
  rec.setLedger(&led2, VENUE_ACCT);
  const auto records = Journal::loadTimed(path);
  std::remove(path.c_str());
  ASSERT_GE(records.size(), 2u);
  size_t at = 0;
  for (const auto& [ts, cmd] : records)
  {
    // SnapshotEnd is the hash check: a hold restored without its two client
    // order ids hashes to something else and the generation is refused here.
    ASSERT_TRUE(rec.applySnapshotRecord(cmd, ts))
        << "snapshot record " << at << " refused by the loader";
    ++at;
  }
  EXPECT_EQ(rec.stateHash(), eng.stateHash())
      << "restored state diverged from the live engine at the checkpoint boundary";

  restored.reset();
  rec.submit(decision, 5000);
  out = Outcome{restored.digest, restored.events, rec.stateHash()};
}

}  // namespace

// The accept path: the hold prints, and both execution reports have to name
// their order the way its submitter did.
TEST(VenueCheckpointHeldClOrdId, AcceptedHoldReportsUnderTheSubmittedIds)
{
  Outcome live{};
  ASSERT_NO_FATAL_FAILURE(runScenario(/*viaSnapshot*/ false, /*accept*/ true, live));
  Outcome rec{};
  ASSERT_NO_FATAL_FAILURE(runScenario(/*viaSnapshot*/ true, /*accept*/ true, rec));

  ASSERT_NE(findEvent<Trade>(rec.tail), nullptr) << "the accepted hold did not print";
  size_t seen = 0;
  for (const OutboundEvent& e : rec.tail)
  {
    const auto* x = std::get_if<OrderExecuted>(&e);
    if (x == nullptr)
    {
      continue;
    }
    ++seen;
    EXPECT_EQ(x->clientOrderId, x->aggressor ? TAKER_CLORD : MAKER_CLORD)
        << "execution report for order " << x->id << " came back under a foreign id";
  }
  EXPECT_EQ(seen, 2u) << "an accepted hold reports both legs";

  EXPECT_EQ(rec.stream, live.stream)
      << "the restored engine's decision tail differs from the run with no checkpoint in it";
  EXPECT_EQ(rec.stateHash, live.stateHash);
}

// The reject path: nothing prints, both legs go back on the book, and the
// three reports that carry a client order id (the taker's FillRejected, the
// rebuilt maker's OrderModified, the rebuilt taker's OrderAccepted) have to
// carry the submitted one.
TEST(VenueCheckpointHeldClOrdId, RejectedHoldRestoresBothLegsUnderTheSubmittedIds)
{
  Outcome live{};
  ASSERT_NO_FATAL_FAILURE(runScenario(/*viaSnapshot*/ false, /*accept*/ false, live));
  Outcome rec{};
  ASSERT_NO_FATAL_FAILURE(runScenario(/*viaSnapshot*/ true, /*accept*/ false, rec));

  const FillRejected* fr = findEvent<FillRejected>(rec.tail);
  ASSERT_NE(fr, nullptr) << "the refused hold produced no reject report";
  EXPECT_EQ(fr->clientOrderId, TAKER_CLORD);

  const OrderModified* om = findEvent<OrderModified>(rec.tail);
  ASSERT_NE(om, nullptr) << "the maker's quantity did not come back to the book";
  EXPECT_EQ(om->id, MAKER_ID);
  EXPECT_EQ(om->clientOrderId, MAKER_CLORD);

  const OrderAccepted* oa = findEvent<OrderAccepted>(rec.tail);
  ASSERT_NE(oa, nullptr) << "the taker residual did not rest again";
  EXPECT_EQ(oa->id, TAKER_ID);
  EXPECT_EQ(oa->clientOrderId, TAKER_CLORD);

  EXPECT_EQ(rec.stream, live.stream)
      << "the restored engine's decision tail differs from the run with no checkpoint in it";
  EXPECT_EQ(rec.stateHash, live.stateHash);
}
