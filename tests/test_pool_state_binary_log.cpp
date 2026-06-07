/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/constant_product_curve.h"
#include "flox/connector/amm_dex_connector.h"
#include "flox/replay/binary_format_v1.h"
#include "flox/replay/pool_state_tape.h"
#include "flox/replay/readers/binary_log_reader.h"
#include "flox/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <vector>

using namespace flox;
using namespace flox::replay;

namespace
{

u256 D(const char* s) { return u256::fromDec(s); }

// Transcode one framed pool record (the bytes a PoolStateWriter produces for a
// single record) onto the binary-log timeline as an EventType::PoolState frame at
// the given timestamp. This is exactly what a chain ingest does: parse an event into
// a pool record, stamp it, append it to the same tape as trades and books.
void emitPool(BinaryLogWriter& w, int64_t ts, uint32_t symbol, PoolVenue venue,
              const std::vector<uint8_t>& framed)
{
  forEachPoolRecord(framed,
                    [&](PoolRecord kind, const uint8_t* rec, uint64_t len)
                    {
                      PoolStateRecordHeader h{};
                      h.exchange_ts_ns = ts;
                      h.symbol_id = symbol;
                      h.sub_type = static_cast<uint8_t>(kind);
                      h.venue = static_cast<uint8_t>(venue);
                      w.writePoolState(h, rec, len);
                    });
}

template <typename Build>
std::vector<uint8_t> framed(Build&& build)
{
  std::vector<uint8_t> buf;
  PoolStateWriter w(buf);
  build(w);
  return buf;
}

// A constant-product pool's history and two unrelated trades, written interleaved in
// timestamp order onto a single binary-log segment. Read back, the events come out in
// one timestamp-ordered stream -- pool records and trades together -- and feeding the
// pool records' payloads through PoolStateReplay::step reconstructs the exact pool
// state, the same result as replaying a standalone tape.
TEST(PoolStateBinaryLogTest, PoolRecordsShareTheTradeTimeline)
{
  const auto dir = std::filesystem::temp_directory_path() / "flox_pool_timeline";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  const u256 r0 = D("1000000000000000000000");
  const u256 r1 = D("2000000000000000000000");
  const u256 a1 = D("5000000000000000000");  // base -> quote

  // Independent reference for the expected post-swap reserves.
  ConstantProductCurve expected(r0, r1, 997, 1000);
  expected.applySwap(0, 1, a1);
  const u256 f0 = expected.balances()[0];
  const u256 f1 = expected.balances()[1];

  constexpr uint32_t kSym = 7;

  {
    WriterConfig cfg{.output_dir = dir};
    BinaryLogWriter writer(cfg);

    emitPool(writer, 100, kSym, PoolVenue::ConstantProduct,
             framed([&](PoolStateWriter& w)
                    { w.descriptorConstantProduct(997, 1000, 18, 18); }));
    emitPool(writer, 100, kSym, PoolVenue::ConstantProduct,
             framed([&](PoolStateWriter& w)
                    { w.checkpoint(100, r0, r1); }));

    TradeRecord t1{};
    t1.exchange_ts_ns = 150;
    t1.symbol_id = 99;  // an unrelated instrument on the same tape
    t1.trade_id = 1;
    writer.writeTrade(t1);

    emitPool(writer, 200, kSym, PoolVenue::ConstantProduct,
             framed([&](PoolStateWriter& w)
                    { w.swap(200, true, a1); }));

    TradeRecord t2{};
    t2.exchange_ts_ns = 250;
    t2.symbol_id = 99;
    t2.trade_id = 2;
    writer.writeTrade(t2);

    emitPool(writer, 300, kSym, PoolVenue::ConstantProduct,
             framed([&](PoolStateWriter& w)
                    { w.checkpoint(300, f0, f1); }));

    writer.close();
  }

  // Replay the pool records off the shared tape, and confirm one ordered stream.
  ConstantProductCurve seed(r0, r1, 997, 1000);
  AmmDexConnector conn("amm", SymbolId{kSym}, seed, 0, 1, 18, 18, 1, D("1000000000000000000"));
  int poolTrades = 0;
  conn.setCallbacks([](const BookUpdateEvent&) {}, [&](const TradeEvent&)
                    { ++poolTrades; });
  PoolStateReplay replay(conn);

  ReaderConfig cfg{.data_dir = dir};
  BinaryLogReader reader(cfg);

  std::vector<EventType> order;
  std::vector<int64_t> times;
  int rawTrades = 0;
  reader.forEach(
      [&](const ReplayEvent& ev)
      {
        order.push_back(ev.type);
        times.push_back(ev.timestamp_ns);
        if (ev.type == EventType::Trade)
        {
          ++rawTrades;
        }
        else if (ev.type == EventType::PoolState)
        {
          const auto kind = static_cast<PoolRecord>(ev.pool_state_header.sub_type);
          const auto* p = reinterpret_cast<const uint8_t*>(ev.pool_state_payload.data());
          replay.step(kind, p, ev.pool_state_payload.size());
        }
        return true;
      });

  // Six events, in non-decreasing timestamp order, pool records and trades mixed.
  ASSERT_EQ(order.size(), 6u);
  EXPECT_EQ(order[0], EventType::PoolState);  // descriptor
  EXPECT_EQ(order[1], EventType::PoolState);  // checkpoint
  EXPECT_EQ(order[2], EventType::Trade);      // unrelated trade at 150
  EXPECT_EQ(order[3], EventType::PoolState);  // swap delta
  EXPECT_EQ(order[4], EventType::Trade);      // unrelated trade at 250
  EXPECT_EQ(order[5], EventType::PoolState);  // closing checkpoint
  for (std::size_t i = 1; i < times.size(); ++i)
  {
    EXPECT_LE(times[i - 1], times[i]);
  }

  EXPECT_EQ(rawTrades, 2);
  EXPECT_EQ(poolTrades, 1);  // the single swap, applied through the curve
  EXPECT_EQ(replay.driftCount(), 0u);
  ASSERT_NE(replay.curve(), nullptr);
  EXPECT_EQ(replay.curve()->balances()[0].toDec(), f0.toDec());
  EXPECT_EQ(replay.curve()->balances()[1].toDec(), f1.toDec());

  std::filesystem::remove_all(dir);
}

// The same round-trip through the LZ4 block path (writeFrameToBlock), so a compressed
// tape reconstructs the pool state identically.
TEST(PoolStateBinaryLogTest, CompressedTapeRoundTrips)
{
  const auto dir = std::filesystem::temp_directory_path() / "flox_pool_timeline_lz4";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  const u256 r0 = D("1000000000000000000000");
  const u256 r1 = D("2000000000000000000000");
  const u256 a1 = D("5000000000000000000");

  ConstantProductCurve expected(r0, r1, 997, 1000);
  expected.applySwap(0, 1, a1);
  const u256 f0 = expected.balances()[0];
  const u256 f1 = expected.balances()[1];

  constexpr uint32_t kSym = 7;

  {
    WriterConfig cfg{.output_dir = dir};
    cfg.compression = CompressionType::LZ4;
    BinaryLogWriter writer(cfg);
    emitPool(writer, 100, kSym, PoolVenue::ConstantProduct,
             framed([&](PoolStateWriter& w)
                    { w.descriptorConstantProduct(997, 1000, 18, 18); }));
    emitPool(writer, 100, kSym, PoolVenue::ConstantProduct,
             framed([&](PoolStateWriter& w)
                    { w.checkpoint(100, r0, r1); }));
    emitPool(writer, 200, kSym, PoolVenue::ConstantProduct,
             framed([&](PoolStateWriter& w)
                    { w.swap(200, true, a1); }));
    emitPool(writer, 300, kSym, PoolVenue::ConstantProduct,
             framed([&](PoolStateWriter& w)
                    { w.checkpoint(300, f0, f1); }));
    writer.close();
  }

  ConstantProductCurve seed(r0, r1, 997, 1000);
  AmmDexConnector conn("amm", SymbolId{kSym}, seed, 0, 1, 18, 18, 1, D("1000000000000000000"));
  conn.setCallbacks([](const BookUpdateEvent&) {}, [](const TradeEvent&) {});
  PoolStateReplay replay(conn);

  ReaderConfig cfg{.data_dir = dir};
  BinaryLogReader reader(cfg);
  int poolRecords = 0;
  reader.forEach(
      [&](const ReplayEvent& ev)
      {
        if (ev.type == EventType::PoolState)
        {
          ++poolRecords;
          const auto kind = static_cast<PoolRecord>(ev.pool_state_header.sub_type);
          const auto* p = reinterpret_cast<const uint8_t*>(ev.pool_state_payload.data());
          replay.step(kind, p, ev.pool_state_payload.size());
        }
        return true;
      });

  EXPECT_EQ(poolRecords, 4);
  EXPECT_EQ(replay.driftCount(), 0u);
  ASSERT_NE(replay.curve(), nullptr);
  EXPECT_EQ(replay.curve()->balances()[0].toDec(), f0.toDec());
  EXPECT_EQ(replay.curve()->balances()[1].toDec(), f1.toDec());

  std::filesystem::remove_all(dir);
}

}  // namespace
