/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Market-data feed completeness: the two timestamps, the trading-status
 * message and the derivatives layer -- on every path (live increment, resend,
 * snapshot) and in both encodings, without breaking a reader of the previous
 * schema version.
 */
#include "flox-venue/fix_md_codec.h"
#include "flox-venue/ledger.h"
#include "flox-venue/market_data.h"
#include "flox-venue/matching_book.h"
#include "flox-venue/matching_engine.h"
#include "flox-venue/md_encoder.h"
#include "flox-venue/sbe_md_codec.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
constexpr AssetId QUOTE = 1;
constexpr uint64_t VENUE = 999;
constexpr int64_t SEC = 1'000'000'000LL;

Price px(double v) { return Price::fromDouble(v); }
Quantity qty(double v) { return Quantity::fromDouble(v); }
Amount quote(double v) { return amountOf(Volume::fromDouble(v)); }

SymbolConfig cfg()
{
  SymbolConfig c;
  c.id = SYM;
  c.tickSize = px(0.01);
  c.minPrice = px(1.0);
  c.maxPrice = px(1000.0);
  return c;
}

SymbolConfig perpCfg()
{
  SymbolConfig c = cfg();
  c.quoteAsset = QUOTE;
  c.linearPerp = true;
  c.initialMarginBps = 1000;                  // 10x
  c.fundingIntervalNs = DurationNs{8 * SEC};  // a funding calendar the feed can publish
  return c;
}

NewOrder limit(OrderId id, Side s, double p, double q, uint64_t acct = 1)
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

// A publisher wired to an engine, collecting everything it emits.
struct Feed
{
  std::vector<MdMessage> out;
  MarketDataPublisher<> md;
  MatchingEngine<MatchingBook> eng;

  explicit Feed(SymbolConfig c = cfg())
      : md([this](const MdMessage& m)
           { out.push_back(m); }, px(0.01), SYM),
        eng(c, [this](const OutboundEvent& e)
            { md.onEvent(e, eng.engineTimeNs()); })
  {
  }

  const MdMessage* firstOf(MdType t) const
  {
    for (const MdMessage& m : out)
    {
      if (m.type == t)
      {
        return &m;
      }
    }
    return nullptr;
  }
  const MdMessage* lastOf(MdType t) const
  {
    const MdMessage* found = nullptr;
    for (const MdMessage& m : out)
    {
      if (m.type == t)
      {
        found = &m;
      }
    }
    return found;
  }
  size_t countOf(MdType t) const
  {
    size_t n = 0;
    for (const MdMessage& m : out)
    {
      n += m.type == t ? 1 : 0;
    }
    return n;
  }
};

// ---------------------------------------------------------------- timestamps

// Every message on the live path carries both times: the engine's sequencer-ts
// (exactly the ts the causing command was submitted at) and a wall clock that
// is at or after it. Neither is left at zero, on any message type.
void test_timestamps_on_every_message()
{
  std::printf("test_timestamps_on_every_message\n");
  Feed f;
  f.eng.submit(limit(1, Side::SELL, 100, 5), 1000);
  f.eng.submit(limit(2, Side::BUY, 100, 2), 2000);  // trade + executed
  f.eng.submit(CancelOrder{1, SYM, 1}, 3000);
  f.eng.submit(InboundCommand{AdminCmd{SYM, AdminAction::Halt}}, 4000);

  CHECK(!f.out.empty());
  int64_t prevEngine = 0;
  int64_t prevSend = 0;
  for (const MdMessage& m : f.out)
  {
    CHECK(m.engineTsNs != 0);
    CHECK(m.sendTsNs != 0);
    CHECK(m.engineTsNs >= prevEngine);  // sequencer time never goes backwards
    CHECK(m.sendTsNs >= prevSend);      // nor does the latched send clock
    prevEngine = m.engineTsNs;
    prevSend = m.sendTsNs;
  }
  // The engine ts is the submit ts, not a clock read: it is exactly what the
  // caller sequenced the command at.
  CHECK(f.out.front().engineTsNs == 1000);
  CHECK(f.out.back().engineTsNs == 4000);
  CHECK(f.lastOf(MdType::Trade) != nullptr && f.lastOf(MdType::Trade)->engineTsNs == 2000);
}

// Replaying the same command stream reproduces every engineTsNs bit for bit;
// sendTsNs is a wall-clock read and does not reproduce. That asymmetry is the
// contract: engineTs may enter a determinism digest, sendTs may not.
void test_engine_ts_replays_send_ts_does_not()
{
  std::printf("test_engine_ts_replays_send_ts_does_not\n");
  const std::vector<std::pair<int64_t, InboundCommand>> stream{
      {10, InboundCommand{limit(1, Side::SELL, 100, 5)}},
      {20, InboundCommand{limit(2, Side::BUY, 101, 5, 2)}},
      {30, InboundCommand{AdminCmd{SYM, AdminAction::Halt}}},
      {40, InboundCommand{AdminCmd{SYM, AdminAction::Resume}}},
  };

  auto run = [&]
  {
    Feed f;
    for (const auto& [ts, c] : stream)
    {
      f.eng.submit(c, ts);
    }
    return f.out;
  };

  const auto a = run();
  const auto b = run();
  CHECK(a.size() == b.size() && !a.empty());
  bool engineIdentical = true;
  bool anySendDiffers = false;
  for (size_t i = 0; i < a.size() && i < b.size(); ++i)
  {
    engineIdentical = engineIdentical && a[i].engineTsNs == b[i].engineTsNs;
    anySendDiffers = anySendDiffers || a[i].sendTsNs != b[i].sendTsNs;
  }
  CHECK(engineIdentical);
  CHECK(anySendDiffers);  // a wall clock advanced between the two runs
}

// A snapshot states when each resting order arrived, and a resend restamps the
// send time while leaving the event's own time alone -- which is how a
// consumer tells a replayed copy from the original.
void test_snapshot_and_resend_timestamps()
{
  std::printf("test_snapshot_and_resend_timestamps\n");
  Feed f;
  f.eng.submit(limit(1, Side::SELL, 100, 5), 1'000);
  f.eng.submit(limit(2, Side::SELL, 101, 3), 2'000);

  const MdSnapshot snap = f.md.snapshotAtomic();
  CHECK(snap.orders.size() == 2);
  for (const MdMessage& m : snap.orders)
  {
    CHECK(m.engineTsNs == (m.id == 1 ? 1'000 : 2'000));  // the accept time, not "now"
    CHECK(m.sendTsNs >= f.out.back().sendTsNs);          // the snapshot went out later
  }

  const auto replay = f.md.resendFrom(1);
  CHECK(replay.has_value() && replay->size() == f.out.size());
  for (size_t i = 0; i < replay->size(); ++i)
  {
    CHECK((*replay)[i].engineTsNs == f.out[i].engineTsNs);  // the event's own time survives
    CHECK((*replay)[i].sendTsNs >= f.out[i].sendTsNs);      // this transmission is later
  }
}

// -------------------------------------------------------------- trading status

// Halt and resume reach the wire as transitions, with the state the engine is
// actually in -- not inferred from a silent feed.
void test_halt_and_resume_visible()
{
  std::printf("test_halt_and_resume_visible\n");
  Feed f;
  f.eng.submit(limit(1, Side::SELL, 100, 5), 10);
  CHECK(f.countOf(MdType::TradingStatus) == 0);  // nothing has changed yet

  f.eng.submit(InboundCommand{AdminCmd{SYM, AdminAction::Halt}}, 20);
  const MdMessage* halted = f.lastOf(MdType::TradingStatus);
  CHECK(halted != nullptr);
  CHECK(halted->status == TradingStatus::Halted);
  CHECK(halted->reason == TradingStatusReason::Administrative);
  CHECK(halted->untilNs == 0);  // an operator halt has no deadline
  CHECK(halted->seq != 0 && halted->epoch == f.md.epoch());

  // Halting an already halted symbol is not a transition: no duplicate.
  const size_t afterHalt = f.countOf(MdType::TradingStatus);
  f.eng.submit(InboundCommand{AdminCmd{SYM, AdminAction::Halt}}, 30);
  CHECK(f.countOf(MdType::TradingStatus) == afterHalt);

  f.eng.submit(InboundCommand{AdminCmd{SYM, AdminAction::Resume}}, 40);
  CHECK(f.lastOf(MdType::TradingStatus)->status == TradingStatus::Trading);
  CHECK(f.lastOf(MdType::TradingStatus)->engineTsNs == 40);
}

// The timed volatility pause is its own state, carries its deadline, and its
// expiry is published too -- a subscriber never has to time it out itself.
void test_luld_pause_visible()
{
  std::printf("test_luld_pause_visible\n");
  SymbolConfig c = cfg();
  c.luldBps = 500;                   // +/- 5%
  c.luldHaltNs = DurationNs{1'000};  // short pause
  Feed f(c);
  f.eng.submit(limit(1, Side::SELL, 100, 5), 100);
  f.eng.submit(limit(2, Side::BUY, 100, 5, 2), 200);  // last price = 100 -> band [95, 105]

  NewOrder sweep = limit(3, Side::SELL, 0, 5, 2);
  sweep.type = OrderType::MARKET;
  f.eng.submit(limit(4, Side::BUY, 90, 5, 3), 300);  // a bid well below the band
  f.eng.submit(sweep, 400);                          // market sell prints 90 -> breach

  const MdMessage* pause = f.lastOf(MdType::TradingStatus);
  CHECK(pause != nullptr);
  CHECK(pause->status == TradingStatus::LuldPause);
  CHECK(pause->reason == TradingStatusReason::LuldBreach);
  CHECK(pause->untilNs == 400 + 1'000);  // the deadline in sequencer time
  CHECK(pause->engineTsNs == 400);

  // Past the deadline the engine resumes and says so.
  f.eng.submit(InboundCommand{TimeTick{SYM}}, 400 + 1'001);
  const MdMessage* back = f.lastOf(MdType::TradingStatus);
  CHECK(back->status == TradingStatus::Trading);
  CHECK(back->reason == TradingStatusReason::LuldPauseElapsed);
}

// An auction is three states, not one: accumulation, the uncross, and the
// continuous session that follows.
void test_auction_phases_visible()
{
  std::printf("test_auction_phases_visible\n");
  Feed f;
  f.eng.submit(InboundCommand{AdminCmd{SYM, AdminAction::BeginPreOpen}}, 10);
  CHECK(f.lastOf(MdType::TradingStatus)->status == TradingStatus::AuctionPreOpen);

  f.eng.submit(limit(1, Side::SELL, 100, 5), 20);
  f.eng.submit(limit(2, Side::BUY, 100, 5, 2), 30);  // crossed, accumulating
  f.eng.submit(InboundCommand{AdminCmd{SYM, AdminAction::OpenContinuous}}, 40);

  std::vector<flox::venue::TradingStatus> seen;
  for (const MdMessage& m : f.out)
  {
    if (m.type == MdType::TradingStatus)
    {
      seen.push_back(m.status);
    }
  }
  CHECK(seen.size() == 3);
  CHECK(seen[0] == TradingStatus::AuctionPreOpen);
  CHECK(seen[1] == TradingStatus::AuctionUncross);
  CHECK(seen[2] == TradingStatus::Trading);

  // The uncross fills are bracketed by the uncross status and the resume, so a
  // consumer can attribute them to the auction.
  size_t uncrossAt = 0, tradeAt = 0, resumeAt = 0;
  for (size_t i = 0; i < f.out.size(); ++i)
  {
    if (f.out[i].type == MdType::TradingStatus &&
        f.out[i].status == TradingStatus::AuctionUncross)
    {
      uncrossAt = i;
    }
    if (f.out[i].type == MdType::Trade)
    {
      tradeAt = i;
    }
    if (f.out[i].type == MdType::TradingStatus && f.out[i].status == TradingStatus::Trading)
    {
      resumeAt = i;
    }
  }
  CHECK(uncrossAt < tradeAt && tradeAt < resumeAt);
}

// ---------------------------------------------------------------- derivatives

void test_mark_funding_and_open_interest_visible()
{
  std::printf("test_mark_funding_and_open_interest_visible\n");
  Ledger led;
  led.deposit(1, QUOTE, quote(10'000));
  led.deposit(2, QUOTE, quote(10'000));
  Feed f(perpCfg());
  f.eng.setLedger(&led, VENUE);

  f.eng.submit(InboundCommand{SetMark{SYM, px(100)}}, 1 * SEC);
  const MdMessage* first = f.lastOf(MdType::DerivativesUpdate);
  CHECK(first != nullptr);
  CHECK(first->price == px(100));          // mark
  CHECK(first->qty == qty(0));             // no positions yet
  CHECK(first->fundingRateRaw == 0);       // no funding applied yet
  CHECK(first->nextFundingNs == 8 * SEC);  // 8s schedule, first boundary
  CHECK(first->engineTsNs == 1 * SEC);

  // Open a long/short pair: open interest is the long side, not the net.
  f.eng.submit(limit(1, Side::SELL, 100, 4, 2), 2 * SEC);
  f.eng.submit(limit(2, Side::BUY, 100, 4, 1), 3 * SEC);
  f.eng.submit(InboundCommand{SetMark{SYM, px(101)}}, 4 * SEC);
  const MdMessage* withOi = f.lastOf(MdType::DerivativesUpdate);
  CHECK(withOi->price == px(101));
  CHECK(withOi->qty == qty(4));
  CHECK(f.eng.openInterest() == qty(4));

  // A funding settlement publishes the rate that was applied, as fixed point.
  f.eng.submit(InboundCommand{ApplyFunding{SYM, 0.0001, px(101)}}, 9 * SEC);
  const MdMessage* funded = f.lastOf(MdType::DerivativesUpdate);
  CHECK(funded->fundingRateRaw == kFundingRateScale / 10'000);  // 1bp
  CHECK(funded->nextFundingNs == 16 * SEC);                     // next boundary of the schedule
  CHECK(funded->qty == qty(4));

  // The rate persists onto later mark updates: a consumer that joins between
  // settlements still learns what it is paying.
  f.eng.submit(InboundCommand{SetMark{SYM, px(102)}}, 10 * SEC);
  CHECK(f.lastOf(MdType::DerivativesUpdate)->fundingRateRaw == kFundingRateScale / 10'000);

  // Without a configured funding interval there is no calendar to publish, and
  // the feed says 0 rather than inventing one.
  Feed spot(cfg());
  spot.eng.submit(InboundCommand{SetMark{SYM, px(50)}}, 1 * SEC);
  CHECK(spot.lastOf(MdType::DerivativesUpdate)->nextFundingNs == 0);
}

// ------------------------------------------------------------- late joiner

// The snapshot is a startable state: status and derivatives come with the book.
void test_late_joiner_snapshot_carries_state()
{
  std::printf("test_late_joiner_snapshot_carries_state\n");
  Feed f(perpCfg());
  f.eng.submit(limit(1, Side::SELL, 100, 5), 1 * SEC);
  f.eng.submit(InboundCommand{SetMark{SYM, px(100)}}, 2 * SEC);
  f.eng.submit(InboundCommand{AdminCmd{SYM, AdminAction::Halt}}, 3 * SEC);

  const MdSnapshot snap = f.md.snapshotAtomic();
  CHECK(snap.hasStatus);
  CHECK(snap.status.status == TradingStatus::Halted);
  CHECK(snap.status.seq == 0);  // body record, like the orders
  CHECK(snap.status.engineTsNs == 3 * SEC);
  CHECK(snap.hasDerivatives);
  CHECK(snap.derivatives.price == px(100));
  CHECK(snap.orders.size() == 1);

  // And it survives the SBE snapshot encoding used by the distribution server:
  // begin, state, book, end -- with orderCount still counting orders only.
  SbeMdEncoder enc;
  std::vector<uint8_t> wire;
  enc.snapshot(SYM, snap, wire);

  std::vector<uint16_t> tmpls;
  MdSnapshotBegin begin{};
  MdMessage status{};
  MdMessage derivatives{};
  bool sawStatus = false, sawDerivatives = false;
  size_t orders = 0;
  size_t off = 0;
  while (off + 4 <= wire.size())
  {
    const uint32_t len = (static_cast<uint32_t>(wire[off]) << 24) |
                         (static_cast<uint32_t>(wire[off + 1]) << 16) |
                         (static_cast<uint32_t>(wire[off + 2]) << 8) |
                         static_cast<uint32_t>(wire[off + 3]);
    const uint8_t* p = wire.data() + off + 4;
    const uint16_t t = SbeMdCodec::templateId(p, len);
    tmpls.push_back(t);
    if (static_cast<SbeMdCodec::Tmpl>(t) == SbeMdCodec::Tmpl::SnapshotBegin)
    {
      CHECK(SbeMdCodec::decode(p, len, begin));
    }
    else if (static_cast<SbeMdCodec::Tmpl>(t) == SbeMdCodec::Tmpl::TradingStatus)
    {
      sawStatus = SbeMdCodec::decode(p, len, status);
    }
    else if (static_cast<SbeMdCodec::Tmpl>(t) == SbeMdCodec::Tmpl::DerivativesUpdate)
    {
      sawDerivatives = SbeMdCodec::decode(p, len, derivatives);
    }
    else if (static_cast<SbeMdCodec::Tmpl>(t) == SbeMdCodec::Tmpl::AddOrder)
    {
      ++orders;
    }
    off += 4 + len;
  }
  CHECK(sawStatus && status.status == TradingStatus::Halted);
  CHECK(sawDerivatives && derivatives.price == px(100));
  CHECK(begin.orderCount == 1 && orders == 1);
  // State frames sit between SnapshotBegin and the body, so the subscriber is
  // told the instrument is halted before it applies a single order.
  CHECK(tmpls.size() >= 5);
  CHECK(static_cast<SbeMdCodec::Tmpl>(tmpls[0]) == SbeMdCodec::Tmpl::SnapshotBegin);
  CHECK(static_cast<SbeMdCodec::Tmpl>(tmpls[1]) == SbeMdCodec::Tmpl::TradingStatus);
  CHECK(static_cast<SbeMdCodec::Tmpl>(tmpls[2]) == SbeMdCodec::Tmpl::DerivativesUpdate);
  CHECK(static_cast<SbeMdCodec::Tmpl>(tmpls.back()) == SbeMdCodec::Tmpl::SnapshotEnd);
}

// ------------------------------------------------------------------ encodings

void test_sbe_round_trip_new_fields()
{
  std::printf("test_sbe_round_trip_new_fields\n");
  std::vector<uint8_t> buf;

  MdMessage st{};
  st.type = MdType::TradingStatus;
  st.seq = 7;
  st.symbol = SYM;
  st.epoch = 4242;
  st.engineTsNs = 111;
  st.sendTsNs = 222;
  st.status = TradingStatus::LuldPause;
  st.reason = TradingStatusReason::LuldBreach;
  st.untilNs = 333;
  SbeMdCodec::encode(st, buf);
  MdMessage back{};
  CHECK(SbeMdCodec::decode(buf.data(), buf.size(), back));
  CHECK(back.type == MdType::TradingStatus && back.seq == 7 && back.epoch == 4242);
  CHECK(back.engineTsNs == 111 && back.sendTsNs == 222 && back.untilNs == 333);
  CHECK(back.status == TradingStatus::LuldPause);
  CHECK(back.reason == TradingStatusReason::LuldBreach);

  MdMessage dv{};
  dv.type = MdType::DerivativesUpdate;
  dv.seq = 8;
  dv.symbol = SYM;
  dv.epoch = 4242;
  dv.engineTsNs = 444;
  dv.sendTsNs = 555;
  dv.price = px(101.25);
  dv.qty = qty(12.5);
  dv.fundingRateRaw = -1234;  // a negative rate: shorts pay
  dv.nextFundingNs = 8 * SEC;
  SbeMdCodec::encode(dv, buf);
  CHECK(SbeMdCodec::decode(buf.data(), buf.size(), back));
  CHECK(back.type == MdType::DerivativesUpdate);
  CHECK(back.price == px(101.25) && back.qty == qty(12.5));
  CHECK(back.fundingRateRaw == -1234 && back.nextFundingNs == 8 * SEC);
  CHECK(back.engineTsNs == 444 && back.sendTsNs == 555);

  MdMessage tr{MdType::Trade, 9, SYM, 42, Side::SELL, px(100), qty(3), 41, 4242};
  tr.engineTsNs = 666;
  tr.sendTsNs = 777;
  SbeMdCodec::encode(tr, buf);
  CHECK(SbeMdCodec::decode(buf.data(), buf.size(), back));
  CHECK(back.engineTsNs == 666 && back.sendTsNs == 777 && back.makerId == 41);
}

// A reader built against schema version 2 knows nothing about engineTs/sendTs.
// It must still decode every field it does know out of a version-3 frame, by
// bounding itself with the header's blockLength exactly as SBE prescribes.
// This is the previous version's decoder, transcribed against its own offsets.
bool decodeAsV2(const uint8_t* p, size_t n, MdMessage& m)
{
  if (n < SbeMdCodec::kHeaderSize)
  {
    return false;
  }
  const sbe::Header h = sbe::readHeader(p);
  if (h.schemaId != SbeMdCodec::kSchemaId || n < SbeMdCodec::kHeaderSize + h.blockLength)
  {
    return false;
  }
  const uint8_t* b = p + SbeMdCodec::kHeaderSize;
  const uint16_t v2Order = 37;  // seq,sym,id,side,px,qty as version 2 knew it
  if (h.templateId == 2)        // Trade
  {
    if (h.blockLength < v2Order + 8)
    {
      return false;
    }
    m.type = MdType::Trade;
    m.makerId = sbe::getU64(b + v2Order);
    m.epoch = h.blockLength >= v2Order + 16 ? sbe::getU64(b + v2Order + 8) : 0;
  }
  else if (h.templateId >= 1 && h.templateId <= 5)  // AddOrder / Executed / Cancel / Replace
  {
    if (h.blockLength < v2Order)
    {
      return false;
    }
    m.type = static_cast<MdType>(h.templateId - 1);
    m.epoch = h.blockLength >= v2Order + 8 ? sbe::getU64(b + v2Order) : 0;
  }
  else
  {
    return false;  // version 2 has no template 14/15: it skips the frame
  }
  m.seq = sbe::getU64(b + 0);
  m.symbol = static_cast<SymbolId>(sbe::getU32(b + 8));
  m.id = sbe::getU64(b + 12);
  m.side = static_cast<Side>(b[20]);
  m.price = Price::fromRaw(sbe::getI64(b + 21));
  m.qty = Quantity::fromRaw(sbe::getI64(b + 29));
  return true;
}

void test_previous_schema_version_still_reads()
{
  std::printf("test_previous_schema_version_still_reads\n");
  Feed f;
  f.eng.submit(limit(1, Side::SELL, 100, 5), 1'000);
  f.eng.submit(limit(2, Side::BUY, 100, 2, 2), 2'000);
  f.eng.submit(InboundCommand{AdminCmd{SYM, AdminAction::Halt}}, 3'000);
  CHECK(f.countOf(MdType::TradingStatus) == 1);

  std::vector<uint8_t> buf;
  size_t decoded = 0;
  size_t skipped = 0;
  for (const MdMessage& m : f.out)
  {
    SbeMdCodec::encode(m, buf);
    MdMessage old{};
    if (!decodeAsV2(buf.data(), buf.size(), old))
    {
      ++skipped;  // a template version 2 never knew: skipped, not corrupting
      continue;
    }
    ++decoded;
    CHECK(old.seq == m.seq);
    CHECK(old.symbol == m.symbol);
    CHECK(old.price == m.price);
    CHECK(old.qty == m.qty);
    CHECK(old.epoch == m.epoch);  // the version-1 trailing field still lands
    CHECK(old.engineTsNs == 0);   // version 2 simply does not see the new ones
  }
  CHECK(decoded > 0);
  CHECK(skipped == 1);  // exactly the new TradingStatus message

  // And the reverse: this decoder accepts a frame that stops after the
  // version-2 fields (an older PUBLISHER), leaving the timestamps at zero.
  MdMessage m{MdType::AddOrder, 5, SYM, 9, Side::BUY, px(100), qty(1), 0, 77};
  SbeMdCodec::encode(m, buf);
  buf[0] = static_cast<uint8_t>(SbeMdCodec::kBlockOrderV1 & 0xFF);  // blockLength -> v1
  buf[1] = static_cast<uint8_t>(SbeMdCodec::kBlockOrderV1 >> 8);
  buf.resize(SbeMdCodec::kHeaderSize + SbeMdCodec::kBlockOrderV1);
  MdMessage back{};
  CHECK(SbeMdCodec::decode(buf.data(), buf.size(), back));
  CHECK(back.seq == 5 && back.epoch == 77 && back.qty == qty(1));
  CHECK(back.engineTsNs == 0 && back.sendTsNs == 0);
}

// The FIX encoding carries the same information in standard fields: 273
// MDEntryTime and 60 TransactTime for the engine time, 52 SendingTime for the
// send time, 35=f SecurityStatus for the halt, and standard MDEntryTypes for
// the mark and open interest.
void test_fix_carries_time_status_and_derivatives()
{
  std::printf("test_fix_carries_time_status_and_derivatives\n");
  FixMdEncoder enc;
  std::vector<uint8_t> out;
  std::vector<MdRequest> reqs;
  std::vector<uint8_t> reply;
  size_t consumed = 0;

  auto feedIn = [&](const std::string& msg)
  {
    reqs.clear();
    reply.clear();
    consumed = 0;
    const std::string framed = FixCodec::frame(msg);
    return enc.parse(reinterpret_cast<const uint8_t*>(framed.data()), framed.size(), consumed, reqs,
                     reply);
  };

  std::string logon;
  FixMdCodec::add(logon, 35, "A");
  FixMdCodec::add(logon, 34, "1");
  FixMdCodec::add(logon, 49, "CLIENT");
  FixMdCodec::add(logon, 56, "VENUE");
  FixMdCodec::add(logon, 52, FixMdCodec::nowStamp());
  FixMdCodec::add(logon, 108, "30");
  CHECK(feedIn(logon) == MdEncoder::Parse::Ok);

  std::string req;
  FixMdCodec::add(req, 35, "V");
  FixMdCodec::add(req, 34, "2");
  FixMdCodec::add(req, 49, "CLIENT");
  FixMdCodec::add(req, 56, "VENUE");
  FixMdCodec::add(req, 52, FixMdCodec::nowStamp());
  FixMdCodec::add(req, 262, "REQ1");
  FixMdCodec::add(req, 263, "1");
  FixMdCodec::add(req, 55, std::to_string(SYM));
  CHECK(feedIn(req) == MdEncoder::Parse::Ok);
  CHECK(!reqs.empty() && reqs[0].kind == MdRequestKind::Subscribe);

  const int64_t engineTs = 1'700'000'000LL * SEC;

  MdMessage add{MdType::AddOrder, 1, SYM, 42, Side::BUY, px(100), qty(5), 0, 9};
  add.engineTsNs = engineTs;
  add.sendTsNs = engineTs + 1'000;
  out.clear();
  enc.increment(add, out);
  const std::string inc(out.begin(), out.end());
  CHECK(inc.find("\x01"
                 "273=") != std::string::npos);  // MDEntryTime
  CHECK(inc.find("\x01"
                 "60=") != std::string::npos);  // TransactTime
  CHECK(inc.find("\x01"
                 "35=X") != std::string::npos);

  MdMessage st{};
  st.type = MdType::TradingStatus;
  st.seq = 2;
  st.symbol = SYM;
  st.engineTsNs = engineTs;
  st.sendTsNs = engineTs;
  st.status = TradingStatus::Halted;
  st.reason = TradingStatusReason::Administrative;
  out.clear();
  enc.increment(st, out);
  const std::string status(out.begin(), out.end());
  CHECK(status.find("\x01"
                    "35=f") != std::string::npos);  // SecurityStatus
  CHECK(status.find("\x01"
                    "326=2") != std::string::npos);  // Trading Halt
  CHECK(status.find("\x01"
                    "60=") != std::string::npos);

  MdMessage pause = st;
  pause.status = TradingStatus::LuldPause;
  pause.reason = TradingStatusReason::LuldBreach;
  pause.untilNs = engineTs + 5 * SEC;
  out.clear();
  enc.increment(pause, out);
  const std::string paused(out.begin(), out.end());
  CHECK(paused.find("\x01"
                    "326=2") != std::string::npos);
  CHECK(paused.find("LULD") != std::string::npos);
  CHECK(paused.find("\x01"
                    "5003=") != std::string::npos);  // the pause deadline

  MdMessage dv{};
  dv.type = MdType::DerivativesUpdate;
  dv.seq = 3;
  dv.symbol = SYM;
  dv.engineTsNs = engineTs;
  dv.sendTsNs = engineTs;
  dv.price = px(101.5);
  dv.qty = qty(4);
  dv.fundingRateRaw = kFundingRateScale / 10'000;
  dv.nextFundingNs = engineTs + 8 * SEC;
  out.clear();
  enc.increment(dv, out);
  const std::string deriv(out.begin(), out.end());
  CHECK(deriv.find("\x01"
                   "269=6") != std::string::npos);  // SettlementPrice = mark
  CHECK(deriv.find("\x01"
                   "269=C") != std::string::npos);  // OpenInterest
  CHECK(deriv.find("\x01"
                   "270=101.5") != std::string::npos);
  CHECK(deriv.find("\x01"
                   "5001=0.0001") != std::string::npos);  // funding rate, exact decimal
  CHECK(deriv.find("\x01"
                   "5002=") != std::string::npos);  // next funding time

  // A snapshot leads with the status, so a FIX consumer joining a halted
  // instrument is told so before it receives the book.
  MdSnapshot snap;
  snap.epoch = 9;
  snap.lastSeq = 3;
  snap.hasStatus = true;
  snap.status = st;
  snap.hasDerivatives = true;
  snap.derivatives = dv;
  snap.orders.push_back(add);
  snap.orders.back().seq = 0;
  out.clear();
  enc.snapshot(SYM, snap, out);
  const std::string full(out.begin(), out.end());
  CHECK(full.find("\x01"
                  "35=f") < full.find("\x01"
                                      "35=W"));
  CHECK(full.find("\x01"
                  "35=W") != std::string::npos);
  CHECK(full.find("\x01"
                  "273=") != std::string::npos);  // per-entry engine time in the refresh
}

}  // namespace

TEST(MdCompleteness, EngineSuite)
{
  test_timestamps_on_every_message();
  test_engine_ts_replays_send_ts_does_not();
  test_snapshot_and_resend_timestamps();
  test_halt_and_resume_visible();
  test_luld_pause_visible();
  test_auction_phases_visible();
  test_mark_funding_and_open_interest_visible();
  test_late_joiner_snapshot_carries_state();
  test_sbe_round_trip_new_fields();
  test_previous_schema_version_still_reads();
  test_fix_carries_time_status_and_derivatives();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
