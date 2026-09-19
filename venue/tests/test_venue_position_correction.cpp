/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox-venue/event_hash.h"
#include "flox-venue/journal.h"
#include "flox-venue/ledger.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/rest_json.h"
#include "flox-venue/sbe_order_entry_codec.h"
#include "support/tmp_path.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace flox;
using namespace flox::venue;
using flox::venue::test::tmpPath;

namespace
{

constexpr SymbolId SYM = 1;
constexpr AssetId QUOTE = 1;
constexpr uint64_t VENUE_ACCT = 999;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(1.0);
  c.maxPrice = px(1000.0);
  c.quoteAsset = QUOTE;
  c.linearPerp = true;
  c.initialMarginBps = 1000;
  return c;
}

NewOrder ord(OrderId id, Side s, double p, double q, uint64_t acct)
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

AdjustPosition adjust(uint64_t acct, double deltaQty, double entry, AdjustReason r,
                      const char* note)
{
  AdjustPosition a{};
  a.accountId = acct;
  a.symbol = SYM;
  a.qtyDeltaRaw = qty(deltaQty).raw();
  a.entryRaw = entry == 0.0 ? 0 : px(entry).raw();
  a.reason = r;
  std::strncpy(a.note, note, kAdjustNoteLen - 1);
  return a;
}

// A live position: account 1 long 5 at 100 against account 2.
struct Venue
{
  Ledger led;
  std::vector<OutboundEvent> ev;
  MatchingEngine<MatchingBook> eng;

  Venue() : eng(cfg(), [this](const OutboundEvent& e)
                { ev.push_back(e); })
  {
    led.deposit(1, QUOTE, static_cast<Amount>(10000) * 100000000);
    led.deposit(2, QUOTE, static_cast<Amount>(10000) * 100000000);
    eng.setLedger(&led, VENUE_ACCT);
    eng.submit(InboundCommand{ord(1, Side::SELL, 100, 5, 2)}, 1);
    eng.submit(InboundCommand{ord(2, Side::BUY, 100, 5, 1)}, 2);
    ev.clear();
  }

  const PositionAdjusted* lastAdjustment() const
  {
    for (auto it = ev.rbegin(); it != ev.rend(); ++it)
    {
      if (const auto* a = std::get_if<PositionAdjusted>(&*it))
      {
        return a;
      }
    }
    return nullptr;
  }

  const OrderRejected* lastReject() const
  {
    for (auto it = ev.rbegin(); it != ev.rend(); ++it)
    {
      if (const auto* r = std::get_if<OrderRejected>(&*it))
      {
        return r;
      }
    }
    return nullptr;
  }
};

// The ordinary case: an external record says the position is a lot smaller
// than the venue thinks, and the operator books the difference.
TEST(PositionCorrection, ACorrectionMovesThePositionAndSaysWhatItBecame)
{
  Venue v;
  ASSERT_EQ(v.eng.positionQty(1), qty(5).raw());

  v.eng.submit(InboundCommand{adjust(1, -2.0, 0.0, AdjustReason::Reconciliation, "daily recon")},
               3);

  EXPECT_EQ(v.eng.positionQty(1), qty(3).raw());
  const auto* a = v.lastAdjustment();
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->account, 1u);
  EXPECT_EQ(a->qtyDeltaRaw, -qty(2).raw());
  EXPECT_EQ(a->qtyAfterRaw, qty(3).raw());
  EXPECT_EQ(a->entryAfterRaw, px(100).raw());  // untouched: no entry was given
  EXPECT_EQ(a->reason, AdjustReason::Reconciliation);
  EXPECT_STREQ(a->note, "daily recon");
}

// The whole point of the reason code: a reader a week later can tell a
// reconciliation from a settlement correction without asking anyone.
TEST(PositionCorrection, TheReasonAndTheNoteSurviveIntoTheEvent)
{
  Venue v;
  v.eng.submit(
      InboundCommand{adjust(1, 1.0, 0.0, AdjustReason::CounterpartyReport, "LP fill 88213")}, 3);

  const auto* a = v.lastAdjustment();
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->reason, AdjustReason::CounterpartyReport);
  EXPECT_STREQ(a->note, "LP fill 88213");
}

// Setting the average entry is the other half of a correction: a position of
// the right size at the wrong entry prices every later PnL wrong.
TEST(PositionCorrection, AnEntryPriceCanBeCorrectedOnItsOwn)
{
  Venue v;
  v.eng.submit(InboundCommand{adjust(1, 0.0, 99.5, AdjustReason::SettlementCorrection, "settle")},
               3);

  EXPECT_EQ(v.eng.positionQty(1), qty(5).raw());  // size untouched
  EXPECT_EQ(v.eng.positionEntry(1).raw(), px(99.5).raw());
  const auto* a = v.lastAdjustment();
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->qtyDeltaRaw, 0);
  EXPECT_EQ(a->entryAfterRaw, px(99.5).raw());
}

// A correction that says nothing is refused rather than journaled and
// broadcast: an audit trail full of no-ops is worse than one without them.
TEST(PositionCorrection, ACorrectionThatChangesNothingIsRefused)
{
  Venue v;
  v.eng.submit(InboundCommand{adjust(1, 0.0, 0.0, AdjustReason::Manual, "oops")}, 3);

  EXPECT_EQ(v.eng.positionQty(1), qty(5).raw());
  EXPECT_EQ(v.lastAdjustment(), nullptr);
  const auto* r = v.lastReject();
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->reason, RejectReason::AdjustmentEmpty);
}

// Opening a position out of nothing needs an entry price. A zero entry would
// make every later PnL wrong in a way nothing downstream can detect, so the
// engine refuses instead of booking it.
TEST(PositionCorrection, OpeningAPositionWithNoEntryPriceIsRefused)
{
  Venue v;
  const uint64_t stranger = 77;
  ASSERT_EQ(v.eng.positionQty(stranger), 0);

  v.eng.submit(InboundCommand{adjust(stranger, 3.0, 0.0, AdjustReason::Migration, "import")}, 3);

  EXPECT_EQ(v.eng.positionQty(stranger), 0);
  const auto* r = v.lastReject();
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->reason, RejectReason::AdjustmentNeedsEntry);

  // With an entry it is allowed: the operator said where it came from.
  v.ev.clear();
  v.eng.submit(InboundCommand{adjust(stranger, 3.0, 101.0, AdjustReason::Migration, "import")}, 4);
  EXPECT_EQ(v.eng.positionQty(stranger), qty(3).raw());
  EXPECT_EQ(v.eng.positionEntry(stranger).raw(), px(101).raw());
}

// A correction is not a trade. Nothing about it should move money: the
// discrepancy is by definition not backed by a fill, and inventing the cash
// flow a fill would have produced makes the books agree by adding a second
// error.
TEST(PositionCorrection, NoMoneyMoves)
{
  Venue v;
  const Amount beforeAcct = v.led.available(1, QUOTE);
  const Amount beforeVenue = v.led.available(VENUE_ACCT, QUOTE);

  v.eng.submit(InboundCommand{adjust(1, -5.0, 0.0, AdjustReason::Reconciliation, "flatten")}, 3);

  EXPECT_EQ(v.eng.positionQty(1), 0);
  EXPECT_EQ(v.led.available(1, QUOTE), beforeAcct);
  EXPECT_EQ(v.led.available(VENUE_ACCT, QUOTE), beforeVenue);
  bool sawTrade = false;
  bool sawFee = false;
  for (const auto& e : v.ev)
  {
    sawTrade = sawTrade || std::get_if<Trade>(&e) != nullptr;
    sawFee = sawFee || std::get_if<FeeCharged>(&e) != nullptr;
  }
  EXPECT_FALSE(sawTrade);
  EXPECT_FALSE(sawFee);
}

// Flat is flat: an entry price left on a zero position is a number that means
// nothing and reads like it means something.
TEST(PositionCorrection, APositionCorrectedToFlatKeepsNoEntryPrice)
{
  Venue v;
  v.eng.submit(InboundCommand{adjust(1, -5.0, 0.0, AdjustReason::Reconciliation, "flatten")}, 3);

  EXPECT_EQ(v.eng.positionQty(1), 0);
  EXPECT_EQ(v.eng.positionEntry(1).raw(), 0);
  const auto* a = v.lastAdjustment();
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->entryAfterRaw, 0);
}

// The correction is a journaled command like any other, so it survives a
// restart. A correction applied outside the journal would vanish on recovery,
// which is the whole reason this is a command and not a setter.
TEST(PositionCorrection, TheCorrectionSurvivesAJournalRoundTrip)
{
  const std::string path = tmpPath("venue_position_correction", ".bin");
  std::remove(path.c_str());

  const AdjustPosition a = adjust(1, -2.0, 98.25, AdjustReason::CounterpartyReport, "LP 4412");
  {
    Journal j(path, Journal::Sync::Off, Journal::OpenMode::Truncate);
    j.append(InboundCommand{a}, 7);
    j.flush();
  }

  const auto records = Journal::loadTimed(path);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].first, 7);
  const auto* back = std::get_if<AdjustPosition>(&records[0].second);
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(back->accountId, a.accountId);
  EXPECT_EQ(back->qtyDeltaRaw, a.qtyDeltaRaw);
  EXPECT_EQ(back->entryRaw, a.entryRaw);
  EXPECT_EQ(back->reason, a.reason);
  EXPECT_STREQ(back->note, a.note);

  std::remove(path.c_str());
}

// Replay is the contract: the same commands produce the same state, and the
// correction has to be part of that or recovery lands somewhere else.
TEST(PositionCorrection, ReplayReproducesTheCorrectedPosition)
{
  Venue live;
  live.eng.submit(InboundCommand{adjust(1, -2.0, 97.0, AdjustReason::Reconciliation, "recon")}, 3);
  const uint64_t liveHash = live.eng.stateHash();

  Venue replayed;
  replayed.eng.submit(InboundCommand{adjust(1, -2.0, 97.0, AdjustReason::Reconciliation, "recon")},
                      3);

  EXPECT_EQ(replayed.eng.stateHash(), liveHash);
  EXPECT_EQ(replayed.eng.positionQty(1), qty(3).raw());
}

// The account owner has to learn that its position moved under it, or it sees
// a jump with nothing on the session to explain it.
TEST(PositionCorrection, TheEventIsEncodedForSbeClients)
{
  PositionAdjusted e{};
  e.account = 42;
  e.symbol = SYM;
  e.qtyDeltaRaw = qty(-2).raw();
  e.qtyAfterRaw = qty(3).raw();
  e.entryAfterRaw = px(100).raw();
  e.reason = AdjustReason::Reconciliation;
  std::strncpy(e.note, "recon", kAdjustNoteLen - 1);

  std::vector<uint8_t> out;
  SbeOrderEntryCodec::encode(OutboundEvent{e}, out, 9);

  ASSERT_GT(out.size(), sbe::kHeaderSize);
  EXPECT_EQ(SbeOrderEntryCodec::templateId(out.data(), out.size()),
            static_cast<uint16_t>(SbeOrderEntryCodec::OutTmpl::PositionAdjusted));
}

// JSON readers get a typed object, not an object with no type at all.
TEST(PositionCorrection, TheEventIsEncodedForJsonReaders)
{
  PositionAdjusted e{};
  e.account = 42;
  e.symbol = SYM;
  e.qtyDeltaRaw = qty(-2).raw();
  e.qtyAfterRaw = qty(3).raw();
  e.entryAfterRaw = px(100).raw();
  e.reason = AdjustReason::Migration;
  std::strncpy(e.note, "import", kAdjustNoteLen - 1);

  const std::string json = RestJson::encode(OutboundEvent{e});
  EXPECT_NE(json.find("\"type\":\"positionAdjusted\""), std::string::npos) << json;
  EXPECT_NE(json.find("\"reason\":\"migration\""), std::string::npos) << json;
  EXPECT_NE(json.find("\"note\":\"import\""), std::string::npos) << json;
}

// The stream digest is what every replay-equivalence test in the suite
// compares. An event the digest does not see makes those tests agree while the
// streams differ -- so two corrections that differ only in their reason must
// produce different digests.
TEST(PositionCorrection, TheStreamDigestSeesTheCorrection)
{
  const auto digestOf = [](AdjustReason r, const char* note)
  {
    Venue v;
    v.eng.submit(InboundCommand{adjust(1, -1.0, 0.0, r, note)}, 3);
    uint64_t h = 1469598103934665603ULL;
    for (const auto& e : v.ev)
    {
      h = hashEvent(h, e);
    }
    return h;
  };

  const uint64_t base = digestOf(AdjustReason::Reconciliation, "recon");
  EXPECT_NE(digestOf(AdjustReason::Migration, "recon"), base) << "the reason is not in the digest";
  EXPECT_NE(digestOf(AdjustReason::Reconciliation, "other"), base) << "the note is not in the digest";
}

}  // namespace
