/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Side-channel frames must not turn into book updates.
//
// EventType::OptionQuote and EventType::PoolState share the frame stream with
// trades and books. The reader parses them into their own members and leaves
// ReplayEvent::book_header untouched, and the same ReplayEvent object is
// reused for every frame -- so book_header still holds the previous book
// event. ReplayConnector dispatches on "Trade, else book", so each
// side-channel frame republishes that stale header with empty sides: a book
// update for another symbol, at another timestamp, wiping both sides of a book
// nobody touched.

#include "flox/book/events/book_update_event.h"
#include "flox/book/events/trade_event.h"
#include "flox/common.h"
#include "flox/replay/binary_format_v1.h"
#include "flox/replay/replay_connector.h"
#include "flox/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <thread>
#include <vector>

using namespace flox;
using namespace flox::replay;

namespace
{

constexpr int64_t kBaseNs = 1'700'000'000'000'000'000;
constexpr uint32_t kBookSymbol = 1;
constexpr uint32_t kSideChannelSymbol = 2;

struct SeenBook
{
  uint32_t symbol;
  int64_t ts_ns;
  size_t bids;
  size_t asks;
};

class ReplaySideChannelTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    _dir = std::filesystem::temp_directory_path() / "flox_replay_side_channel";
    std::filesystem::remove_all(_dir);
    std::filesystem::create_directories(_dir);
  }

  void TearDown() override { std::filesystem::remove_all(_dir); }

  // One book snapshot on kBookSymbol, then whatever `after` writes.
  template <typename F>
  void writeBookThen(F&& after)
  {
    WriterConfig cfg{};
    cfg.output_dir = _dir;
    cfg.output_filename = "tape.floxlog";
    BinaryLogWriter writer(cfg);

    BookRecordHeader h{};
    h.exchange_ts_ns = kBaseNs;
    h.recv_ts_ns = kBaseNs;
    h.symbol_id = kBookSymbol;
    h.bid_count = 1;
    h.ask_count = 1;
    h.type = 0;
    replay::BookLevel bid{Price::fromDouble(100.0).raw(), Quantity::fromDouble(2.0).raw()};
    replay::BookLevel ask{Price::fromDouble(101.0).raw(), Quantity::fromDouble(3.0).raw()};
    writer.writeBook(h, std::span<const replay::BookLevel>(&bid, 1), std::span<const replay::BookLevel>(&ask, 1));

    after(writer);
    writer.close();
  }

  std::vector<SeenBook> replayBooks() const
  {
    ReplayConnectorConfig cfg{};
    cfg.data_dir = _dir;
    cfg.speed = ReplaySpeed::max();

    ReplayConnector connector(cfg);
    std::vector<SeenBook> seen;
    connector.setCallbacks(
        [&](const BookUpdateEvent& ev)
        {
          seen.push_back(SeenBook{ev.update.symbol, static_cast<int64_t>(ev.update.exchangeTsNs.raw()),
                                  ev.update.bids.size(), ev.update.asks.size()});
        },
        [](const TradeEvent&) {});
    connector.start();
    while (!connector.isFinished())
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    connector.stop();
    return seen;
  }

  std::filesystem::path _dir;
};

}  // namespace

// An option side-channel quote is not a book event and must publish no book
// update at all. Today it republishes the previous book's header with both
// sides emptied.
TEST_F(ReplaySideChannelTest, OptionQuoteFrameEmitsNoBookUpdate)
{
  writeBookThen(
      [](BinaryLogWriter& writer)
      {
        OptionQuoteRecord q{};
        q.exchange_ts_ns = kBaseNs + 1'000'000'000;
        q.recv_ts_ns = q.exchange_ts_ns;
        q.symbol_id = kSideChannelSymbol;
        q.mark_price_raw = Price::fromDouble(0.05).raw();
        q.iv_raw = 65 * kIvScale / 100;
        writer.writeOptionQuote(q);
      });

  auto seen = replayBooks();
  ASSERT_FALSE(seen.empty()) << "the book snapshot itself was not replayed";
  EXPECT_EQ(seen.size(), 1u) << "an OptionQuote frame published a phantom book update";
  for (size_t i = 1; i < seen.size(); ++i)
  {
    EXPECT_NE(seen[i].symbol, kBookSymbol)
        << "the phantom book carries the previous book event's symbol";
  }
}

// Same shape on the DEX pool-state frame.
TEST_F(ReplaySideChannelTest, PoolStateFrameEmitsNoBookUpdate)
{
  writeBookThen(
      [](BinaryLogWriter& writer)
      {
        const std::vector<std::byte> payload(64, std::byte{0x11});
        PoolStateRecordHeader h{};
        h.exchange_ts_ns = kBaseNs + 2'000'000'000;
        h.recv_ts_ns = h.exchange_ts_ns;
        h.symbol_id = kSideChannelSymbol;
        h.payload_len = static_cast<uint32_t>(payload.size());
        h.sub_type = static_cast<uint8_t>(PoolStateKind::SwapDelta);
        writer.writePoolState(h, payload.data(), payload.size());
      });

  auto seen = replayBooks();
  ASSERT_FALSE(seen.empty()) << "the book snapshot itself was not replayed";
  EXPECT_EQ(seen.size(), 1u) << "a PoolState frame published a phantom book update";
  for (size_t i = 1; i < seen.size(); ++i)
  {
    EXPECT_NE(seen[i].symbol, kBookSymbol)
        << "the phantom book carries the previous book event's symbol";
  }
}

// Control. Real book frames still reach the connector with their own symbol
// and their levels intact; the fix must suppress side-channel frames, not
// books. Green on the untouched tree.
TEST_F(ReplaySideChannelTest, BookFramesStillReachTheConnector)
{
  writeBookThen(
      [](BinaryLogWriter& writer)
      {
        BookRecordHeader h{};
        h.exchange_ts_ns = kBaseNs + 3'000'000'000;
        h.recv_ts_ns = h.exchange_ts_ns;
        h.symbol_id = kBookSymbol;
        h.bid_count = 1;
        h.ask_count = 1;
        h.type = 1;
        replay::BookLevel bid{Price::fromDouble(99.0).raw(), Quantity::fromDouble(4.0).raw()};
        replay::BookLevel ask{Price::fromDouble(102.0).raw(), Quantity::fromDouble(5.0).raw()};
        writer.writeBook(h, std::span<const replay::BookLevel>(&bid, 1),
                         std::span<const replay::BookLevel>(&ask, 1));
      });

  auto seen = replayBooks();
  ASSERT_EQ(seen.size(), 2u);
  for (const auto& b : seen)
  {
    EXPECT_EQ(b.symbol, kBookSymbol);
    EXPECT_EQ(b.bids, 1u);
    EXPECT_EQ(b.asks, 1u);
  }
}
