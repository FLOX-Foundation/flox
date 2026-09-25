/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Ordering of merged events on tied timestamps.
//
// Two merge surfaces exist. MergedTapeReader::readTrades / readBooks merge N
// tape directories on read; SegmentOps::merge folds N segment files into one.
// Both order by exchange_ts_ns with std::sort, which is not stable and which
// consults neither the book record's seq nor the order the events were
// recorded in. Every venue that batches an update prints many events on one
// nanosecond, so the tie case is the normal case, and its resolution is what
// makes a replay reproducible.
//
// The contract is already written down: MergedTapeReader's own class comment
// in include/flox/replay/merged_tape_reader.h promises "Tie-break:
// (exchange_ts_ns, tape_index, source order)", and the multi-tape design it
// implements adds seq as the last key for book records.

#include "flox/common.h"
#include "flox/replay/binary_format_v1.h"
#include "flox/replay/merged_tape_reader.h"
#include "flox/replay/ops/segment_ops.h"
#include "flox/replay/readers/binary_log_reader.h"
#include "flox/replay/recording_metadata.h"
#include "flox/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

using namespace flox;
using namespace flox::replay;

namespace
{

constexpr int64_t kBaseNs = 1'700'000'000'000'000'000;
// std::sort only permutes equal keys once the range is past its
// insertion-sort cutoff, so a tie group has to be wide enough to reach the
// quicksort partition.
constexpr size_t kTieWidth = 512;

class ReplayMergeStabilityTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    _root = std::filesystem::temp_directory_path() / "flox_replay_merge_stability";
    std::filesystem::remove_all(_root);
    std::filesystem::create_directories(_root);
  }

  void TearDown() override { std::filesystem::remove_all(_root); }

  std::filesystem::path tapeDir(const std::string& name)
  {
    auto d = _root / name;
    std::filesystem::create_directories(d);
    return d;
  }

  // A tape of trades all stamped `ts_ns`, with trade_id ascending in write
  // order so the recorded order is recoverable from the merged rows.
  void writeTiedTrades(const std::filesystem::path& dir, int64_t ts_ns, size_t count,
                       uint64_t first_trade_id)
  {
    {
      WriterConfig cfg{};
      cfg.output_dir = dir;
      cfg.output_filename = "tape.floxlog";
      BinaryLogWriter writer(cfg);
      for (size_t i = 0; i < count; ++i)
      {
        TradeRecord r{};
        r.exchange_ts_ns = ts_ns;
        r.recv_ts_ns = ts_ns;
        r.price_raw = Price::fromDouble(100.0).raw();
        r.qty_raw = Quantity::fromDouble(1.0).raw();
        r.symbol_id = 1;
        r.trade_id = first_trade_id + i;
        writer.writeTrade(r);
      }
      writer.close();
    }
    writeManifest(dir, /*has_book=*/false);
  }

  // A tape of book snapshots all stamped `ts_ns`. `seq` runs *downwards* in
  // write order, so recorded order and seq order disagree: a merge that
  // preserves source order but ignores seq still fails, which is the half of
  // the contract the current code drops.
  void writeTiedBooksDescendingSeq(const std::filesystem::path& dir, int64_t ts_ns,
                                   size_t count)
  {
    {
      WriterConfig cfg{};
      cfg.output_dir = dir;
      cfg.output_filename = "tape.floxlog";
      BinaryLogWriter writer(cfg);
      for (size_t i = 0; i < count; ++i)
      {
        BookRecordHeader h{};
        h.exchange_ts_ns = ts_ns;
        h.recv_ts_ns = ts_ns;
        h.seq = static_cast<int64_t>(count - i);
        h.symbol_id = 1;
        h.bid_count = 1;
        h.ask_count = 1;
        h.type = 0;
        replay::BookLevel bid{Price::fromDouble(100.0).raw(), Quantity::fromDouble(1.0).raw()};
        replay::BookLevel ask{Price::fromDouble(101.0).raw(), Quantity::fromDouble(1.0).raw()};
        writer.writeBook(h, std::span<const replay::BookLevel>(&bid, 1),
                         std::span<const replay::BookLevel>(&ask, 1));
      }
      writer.close();
    }
    writeManifest(dir, /*has_book=*/true);
  }

  // A tape of book snapshots all stamped `ts_ns` and all carrying seq 0 --
  // what a venue that publishes no sequence number leaves on every record.
  // The recorded position is carried in the bid price, the only field of a
  // book row that survives into the merged output per event.
  void writeTiedBooksWithoutSeq(const std::filesystem::path& dir, int64_t ts_ns,
                                size_t count)
  {
    {
      WriterConfig cfg{};
      cfg.output_dir = dir;
      cfg.output_filename = "tape.floxlog";
      BinaryLogWriter writer(cfg);
      for (size_t i = 0; i < count; ++i)
      {
        BookRecordHeader h{};
        h.exchange_ts_ns = ts_ns;
        h.recv_ts_ns = ts_ns;
        h.seq = 0;
        h.symbol_id = 1;
        h.bid_count = 1;
        h.ask_count = 1;
        h.type = 0;
        replay::BookLevel bid{static_cast<int64_t>(i), Quantity::fromDouble(1.0).raw()};
        replay::BookLevel ask{Price::fromDouble(101.0).raw(), Quantity::fromDouble(1.0).raw()};
        writer.writeBook(h, std::span<const replay::BookLevel>(&bid, 1),
                         std::span<const replay::BookLevel>(&ask, 1));
      }
      writer.close();
    }
    writeManifest(dir, /*has_book=*/true);
  }

  // MergedTapeReader rekeys local symbol ids through metadata.json and skips
  // every event it cannot map, so the manifest is not optional here.
  static void writeManifest(const std::filesystem::path& dir, bool has_book)
  {
    RecordingMetadata meta{};
    meta.recording_id = dir.filename().string();
    meta.exchange = "test";
    meta.has_trades = !has_book;
    meta.has_book_snapshots = has_book;
    ::flox::replay::SymbolInfo s{};
    s.symbol_id = 1;
    s.name = "BTCUSDT";
    s.base_asset = "BTC";
    s.quote_asset = "USDT";
    meta.symbols.push_back(s);
    meta.save(RecordingMetadata::metadataPath(dir));
  }

  std::filesystem::path _root;
};

}  // namespace

// Trades have no seq, so their tie-break is the order they were recorded in --
// exactly what the header comment promises. std::sort loses it.
TEST_F(ReplayMergeStabilityTest, TiedTradesKeepTheirRecordedOrderWithinATape)
{
  auto tape = tapeDir("t0");
  writeTiedTrades(tape, kBaseNs, kTieWidth, /*first_trade_id=*/1);

  MergedTapeReaderConfig cfg{};
  cfg.tape_dirs = {tape};
  MergedTapeReader reader(cfg);

  auto rows = reader.readTrades();
  ASSERT_EQ(rows.size(), kTieWidth);
  for (size_t i = 0; i < rows.size(); ++i)
  {
    ASSERT_EQ(rows[i].trade_id, static_cast<uint64_t>(i + 1))
        << "merged trade " << i << " is out of recorded order on a tied timestamp";
  }
}

// Across two tapes, ties resolve by tape_index first and by recorded order
// inside each tape. Both halves have to hold at once. Green on the untouched
// tree -- tape_index is a real key there, so the partition happens to come out
// in order -- but only by accident of the partitioning; a stable sort on
// (exchange_ts_ns, tape_index, source order) makes it deterministic.
TEST_F(ReplayMergeStabilityTest, TiedTradesAcrossTapesOrderByTapeThenRecordedOrder)
{
  auto t0 = tapeDir("t0");
  auto t1 = tapeDir("t1");
  writeTiedTrades(t0, kBaseNs, kTieWidth, /*first_trade_id=*/1);
  writeTiedTrades(t1, kBaseNs, kTieWidth, /*first_trade_id=*/1);

  MergedTapeReaderConfig cfg{};
  cfg.tape_dirs = {t0, t1};
  MergedTapeReader reader(cfg);

  auto rows = reader.readTrades();
  ASSERT_EQ(rows.size(), kTieWidth * 2);
  for (size_t i = 0; i < rows.size(); ++i)
  {
    const uint32_t expected_tape = i < kTieWidth ? 0u : 1u;
    ASSERT_EQ(rows[i].tape_index, expected_tape) << "row " << i << " came from the wrong tape";
    ASSERT_EQ(rows[i].trade_id, static_cast<uint64_t>(i % kTieWidth) + 1)
        << "merged trade " << i << " is out of recorded order on a tied timestamp";
  }
}

// Book records carry seq, and seq is the sort key the merge is specified to
// use after (exchange_ts_ns, tape_index). Written seq-descending, the merged
// rows must still come out seq-ascending.
TEST_F(ReplayMergeStabilityTest, TiedBooksComeOutInSeqOrder)
{
  auto tape = tapeDir("t0");
  constexpr size_t kBooks = 64;
  writeTiedBooksDescendingSeq(tape, kBaseNs, kBooks);

  MergedTapeReaderConfig cfg{};
  cfg.tape_dirs = {tape};
  MergedTapeReader reader(cfg);

  auto [rows, levels] = reader.readBooks();
  ASSERT_EQ(rows.size(), kBooks);
  for (size_t i = 0; i < rows.size(); ++i)
  {
    ASSERT_EQ(rows[i].seq, static_cast<int64_t>(i + 1))
        << "merged book " << i << " ignores seq on a tied timestamp";
  }
  // The flat level array is indexed by each row's level_offset, so it has to
  // be rebuilt in the sorted order, not the collected one.
  for (const auto& row : rows)
  {
    ASSERT_LE(row.level_offset + row.bid_count + row.ask_count, levels.size());
  }
}

// The other merge surface. SegmentOps::merge with sort_by_timestamp folds
// several segment files into one and must not reshuffle a tie group either:
// the merged segment is what a later replay reads, so an unstable sort here
// bakes the nondeterminism into a file.
TEST_F(ReplayMergeStabilityTest, MergedSegmentsKeepRecordedOrderOnTiedTimestamps)
{
  auto in = tapeDir("in");
  auto out = tapeDir("out");

  const std::vector<std::string> names{"a.floxlog", "b.floxlog"};
  for (size_t s = 0; s < names.size(); ++s)
  {
    WriterConfig cfg{};
    cfg.output_dir = in;
    cfg.output_filename = names[s];
    BinaryLogWriter writer(cfg);
    for (size_t i = 0; i < kTieWidth; ++i)
    {
      TradeRecord r{};
      r.exchange_ts_ns = kBaseNs;
      r.recv_ts_ns = kBaseNs;
      r.price_raw = Price::fromDouble(100.0).raw();
      r.qty_raw = Quantity::fromDouble(1.0).raw();
      r.symbol_id = 1;
      r.trade_id = static_cast<uint64_t>(s) * kTieWidth + i + 1;
      writer.writeTrade(r);
    }
    writer.close();
  }

  MergeConfig mcfg{};
  mcfg.output_dir = out;
  mcfg.output_name = "merged.floxlog";
  mcfg.sort_by_timestamp = true;
  auto result = SegmentOps::merge({in / names[0], in / names[1]}, mcfg);
  ASSERT_TRUE(result.success);

  ReaderConfig rcfg{};
  rcfg.data_dir = out;
  BinaryLogReader reader(rcfg);

  std::vector<uint64_t> ids;
  reader.forEach(
      [&](const ReplayEvent& ev)
      {
        if (ev.type == EventType::Trade)
        {
          ids.push_back(ev.trade.trade_id);
        }
        return true;
      });

  ASSERT_EQ(ids.size(), kTieWidth * 2);
  for (size_t i = 0; i < ids.size(); ++i)
  {
    ASSERT_EQ(ids[i], static_cast<uint64_t>(i + 1))
        << "merged segment reordered trade " << i << " inside a tied timestamp";
  }
}

// Control. Distinct timestamps must still come out in time order after the
// tie-break is fixed. Green on the untouched tree.
TEST_F(ReplayMergeStabilityTest, DistinctTimestampsStillMergeInTimeOrder)
{
  auto t0 = tapeDir("t0");
  auto t1 = tapeDir("t1");

  auto writeSpaced = [&](const std::filesystem::path& dir, int64_t offset_ns)
  {
    {
      WriterConfig cfg{};
      cfg.output_dir = dir;
      cfg.output_filename = "tape.floxlog";
      BinaryLogWriter writer(cfg);
      for (size_t i = 0; i < 32; ++i)
      {
        TradeRecord r{};
        r.exchange_ts_ns = kBaseNs + offset_ns + static_cast<int64_t>(i) * 2'000'000;
        r.recv_ts_ns = r.exchange_ts_ns;
        r.price_raw = Price::fromDouble(100.0).raw();
        r.qty_raw = Quantity::fromDouble(1.0).raw();
        r.symbol_id = 1;
        r.trade_id = i + 1;
        writer.writeTrade(r);
      }
      writer.close();
    }
    writeManifest(dir, /*has_book=*/false);
  };

  writeSpaced(t0, 0);
  writeSpaced(t1, 1'000'000);

  MergedTapeReaderConfig cfg{};
  cfg.tape_dirs = {t0, t1};
  MergedTapeReader reader(cfg);

  auto rows = reader.readTrades();
  ASSERT_EQ(rows.size(), 64u);
  for (size_t i = 1; i < rows.size(); ++i)
  {
    ASSERT_LE(rows[i - 1].exchange_ts_ns, rows[i].exchange_ts_ns);
  }
}

// Every key can be present and the order still be wrong. A venue that
// publishes no sequence number leaves seq at 0 on every record, so
// (exchange_ts_ns, tape_index, seq) ties completely across a batched print and
// the only thing left to order by is the order the events were recorded in --
// which is a property of the sort, not of the key. The tie group is wide
// enough to reach the quicksort partition, where an unstable sort starts
// permuting equal elements.
TEST_F(ReplayMergeStabilityTest, TiedBooksWithNoSeqKeepTheirRecordedOrder)
{
  auto tape = tapeDir("t0");
  writeTiedBooksWithoutSeq(tape, kBaseNs, kTieWidth);

  MergedTapeReaderConfig cfg{};
  cfg.tape_dirs = {tape};
  MergedTapeReader reader(cfg);

  auto [rows, levels] = reader.readBooks();
  ASSERT_EQ(rows.size(), kTieWidth);
  for (size_t i = 0; i < rows.size(); ++i)
  {
    ASSERT_EQ(rows[i].seq, 0) << "the fixture stopped testing the all-ties case";
    ASSERT_LT(rows[i].level_offset, levels.size());
    // The bid level of each book carries the position it was written at.
    ASSERT_EQ(levels[rows[i].level_offset].price_raw, static_cast<int64_t>(i))
        << "merged book " << i << " is out of recorded order on a timestamp and a seq "
        << "that both tie";
  }
}
