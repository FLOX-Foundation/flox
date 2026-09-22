/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * A checkpoint taken on a delisted instrument must restore it delisted.
 * `cloneForSnapshot` is the checkpoint path production actually takes
 * (SequencedShard::doCheckpoint clones the live engine and serializes the
 * clone on a background thread, see venue/include/flox-venue/sequenced_shard.h)
 * -- so this test goes through it rather than through the live engine's own
 * writeSnapshot, which would not have caught the bug this pins: the live
 * session's delisted_ flag is correct either way, only the clone's copy of it
 * was ever wrong.
 */
#include "flox-venue/journal.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "support/tmp_path.h"

#include <gtest/gtest.h>

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

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }
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
  return c;
}

NewOrder limitOrder(OrderId id, Side s, double p, double q, uint64_t acct)
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

struct RejectSink
{
  std::vector<OrderRejected> rejects;
  void operator()(const OutboundEvent& e)
  {
    if (const auto* r = std::get_if<OrderRejected>(&e))
    {
      rejects.push_back(*r);
    }
  }
};

}  // namespace

// The path this test pins: delist -> cloneForSnapshot (the real checkpoint
// path) -> writeSnapshot the clone -> load into a fresh engine ->
// applySnapshotRecord. The restored engine must reject a new order with the
// SAME reason a live delisted engine would, and report TradingStatus::Delisted.
TEST(VenueCheckpointDelisted, RestoredEngineStaysDelisted)
{
  const std::string path = tmpPath("venue_checkpoint_delisted", ".snap");
  std::remove(path.c_str());

  Ledger led;
  SymbolConfig c = cfg();
  RejectSink liveSink;
  MatchingEngine<MatchingBook> eng(c, [&](const OutboundEvent& e)
                                   { liveSink(e); });
  eng.setLedger(&led, 900);
  eng.submit(InboundCommand{Deposit{1, QUOTE, {}, quoteRaw(100000.0), SYM}}, 1000);
  eng.submit(InboundCommand{limitOrder(1, Side::SELL, 100.0, 1.0, 2)}, 2000);

  eng.submit(InboundCommand{AdminCmd{SYM, AdminAction::Delist}}, 3000);
  ASSERT_TRUE(eng.delisted());
  ASSERT_EQ(eng.tradingStatus(), TradingStatus::Delisted);

  // Pinning the reference behaviour: a delisted LIVE engine rejects a new
  // order with InstrumentDelisted. The restored engine below must match it.
  liveSink.rejects.clear();
  eng.submit(InboundCommand{limitOrder(2, Side::BUY, 100.0, 1.0, 1)}, 4000);
  ASSERT_EQ(liveSink.rejects.size(), 1u);
  EXPECT_EQ(liveSink.rejects.front().reason, RejectReason::InstrumentDelisted);

  // The real checkpoint path: clone (this is what SequencedShard::doCheckpoint
  // serializes off the hot path), then write the snapshot FROM THE CLONE.
  auto clone = eng.cloneForSnapshot();
  ASSERT_TRUE(clone.engine->delisted())
      << "cloneForSnapshot dropped the delisting flag";
  EXPECT_EQ(clone.engine->stateHash(), eng.stateHash());
  {
    Journal out(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    clone.engine->writeSnapshot(out);
    out.flush();
  }

  Ledger led2;
  RejectSink restoredSink;
  MatchingEngine<MatchingBook> rec(c, [&](const OutboundEvent& e)
                                   { restoredSink(e); });
  rec.setLedger(&led2, 900);
  const auto records = Journal::loadTimed(path);
  ASSERT_GE(records.size(), 2u);
  for (const auto& [ts, cmd] : records)
  {
    ASSERT_TRUE(rec.applySnapshotRecord(cmd, ts));
  }
  std::remove(path.c_str());

  EXPECT_TRUE(rec.delisted());
  EXPECT_EQ(rec.tradingStatus(), TradingStatus::Delisted);
  EXPECT_EQ(rec.stateHash(), eng.stateHash())
      << "restored state diverged from the live engine at the checkpoint boundary";

  restoredSink.rejects.clear();
  rec.submit(InboundCommand{limitOrder(2, Side::BUY, 100.0, 1.0, 1)}, 5000);
  ASSERT_EQ(restoredSink.rejects.size(), 1u)
      << "restored engine accepted an order on a delisted instrument";
  EXPECT_EQ(restoredSink.rejects.front().reason, RejectReason::InstrumentDelisted);
}
