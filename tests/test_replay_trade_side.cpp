/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// The trade-side convention of the .floxlog tape, pinned end to end.
//
// BinaryLogRecorderHook encodes the aggressor as a byte; ReplayConnector,
// BacktestRunner and OhlcvReplaySource decode it again. Nothing forces the
// four to agree, and today they do not: the recorder writes 0 for a buy while
// both readers treat 1 as the buy. Every tape produced by the recorder
// therefore replays with the aggressor flipped.
//
// These tests never assert a byte value. They assert that a buy recorded
// through the hook is a buy again on every read path, and that the synthetic
// OHLCV source encodes its buy the same way the recorder does. That leaves the
// code agent free to settle the convention in either direction as long as it
// settles it everywhere.

#include "flox/backtest/backtest_runner.h"
#include "flox/book/events/trade_event.h"
#include "flox/engine/symbol_registry.h"
#include "flox/replay/binary_format_v1.h"
#include "flox/replay/binary_log_recorder_hook.h"
#include "flox/replay/ohlcv_replay_source.h"
#include "flox/replay/readers/binary_log_reader.h"
#include "flox/replay/replay_connector.h"
#include "flox/strategy/strategy.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace flox;
using namespace flox::replay;

namespace
{

constexpr int64_t kBaseNs = 1'700'000'000'000'000'000;
constexpr uint32_t kSymbolId = 1;

class ReplayTradeSideTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    _dir = std::filesystem::temp_directory_path() / "flox_replay_trade_side";
    std::filesystem::remove_all(_dir);
    std::filesystem::create_directories(_dir);
  }

  void TearDown() override { std::filesystem::remove_all(_dir); }

  // Records the given aggressor flags through the production recorder hook,
  // one trade per flag, one second apart.
  void recordTrades(const std::vector<bool>& is_buy_flags)
  {
    BinaryLogRecorderHookConfig cfg{};
    cfg.output_dir = _dir;
    BinaryLogRecorderHook hook(cfg);
    hook.start();
    for (size_t i = 0; i < is_buy_flags.size(); ++i)
    {
      const int64_t ts = kBaseNs + static_cast<int64_t>(i) * 1'000'000'000;
      hook.onTrade(kSymbolId, Price::fromDouble(100.0 + static_cast<double>(i)).raw(),
                   Quantity::fromDouble(0.5).raw(), is_buy_flags[i], ts, ts);
    }
    hook.stop();
  }

  std::filesystem::path _dir;
};

std::vector<TradeEvent> replayTrades(const std::filesystem::path& dir)
{
  ReplayConnectorConfig cfg{};
  cfg.data_dir = dir;
  cfg.speed = ReplaySpeed::max();

  ReplayConnector connector(cfg);
  std::vector<TradeEvent> seen;
  connector.setCallbacks([](const BookUpdateEvent&) {},
                         [&](const TradeEvent& ev)
                         { seen.push_back(ev); });
  connector.start();
  while (!connector.isFinished())
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  connector.stop();
  return seen;
}

// Raw side bytes as they sit on disk, in write order.
std::vector<uint8_t> tapeSideBytes(const std::filesystem::path& dir)
{
  ReaderConfig cfg{};
  cfg.data_dir = dir;
  BinaryLogReader reader(cfg);

  std::vector<uint8_t> sides;
  reader.forEach(
      [&](const ReplayEvent& ev)
      {
        if (ev.type == EventType::Trade)
        {
          sides.push_back(ev.trade.side);
        }
        return true;
      });
  return sides;
}

class SideRecordingStrategy : public Strategy
{
 public:
  using Strategy::Strategy;
  std::vector<bool> is_buy_seen;

 protected:
  void onSymbolTrade(SymbolContext& /*ctx*/, const TradeEvent& ev) override
  {
    is_buy_seen.push_back(ev.trade.isBuy);
  }
};

SymbolId registerSymbol(SymbolRegistry& reg, const std::string& name)
{
  ::flox::SymbolInfo info;
  info.exchange = "test";
  info.symbol = name;
  info.type = InstrumentType::Spot;
  info.tickSize = Price::fromDouble(0.01);
  return reg.registerSymbol(info);
}

}  // namespace

// A buy recorded through the hook must come back out of the replay connector
// as a buy, and a sell as a sell. Today the recorder writes 0 for the buy and
// ReplayConnector decodes `side == 1` as the buy, so both flags arrive
// flipped.
TEST_F(ReplayTradeSideTest, RecordedAggressorSurvivesTheReplayConnector)
{
  recordTrades({true, false});

  auto seen = replayTrades(_dir);
  ASSERT_EQ(seen.size(), 2u);
  EXPECT_TRUE(seen[0].trade.isBuy) << "a trade recorded with is_buy=true replayed as a sell";
  EXPECT_FALSE(seen[1].trade.isBuy) << "a trade recorded with is_buy=false replayed as a buy";
}

// Same tape, the other reader. BacktestRunner::runTape decodes the side byte
// itself, so it needs its own pin: a fix that only touches the connector must
// not make this one pass.
TEST_F(ReplayTradeSideTest, RecordedAggressorSurvivesTheBacktestTapePath)
{
  recordTrades({true, false});

  SymbolRegistry reg;
  SymbolId sym = registerSymbol(reg, "BTCUSDT");
  ASSERT_EQ(static_cast<uint32_t>(sym), kSymbolId)
      << "the tape records symbol_id " << kSymbolId << "; the registry must hand out the same id";

  SideRecordingStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  BacktestRunner runner;
  runner.setStrategy(&strat);
  runner.runTape(_dir);

  ASSERT_EQ(strat.is_buy_seen.size(), 2u);
  EXPECT_TRUE(strat.is_buy_seen[0]) << "a trade recorded with is_buy=true reached the strategy as a sell";
  EXPECT_FALSE(strat.is_buy_seen[1]) << "a trade recorded with is_buy=false reached the strategy as a buy";
}

// OhlcvReplaySource synthesises a buy for every bar close. Whatever byte the
// recorder picks for a buy, the synthetic source must pick the same one --
// otherwise a bar-driven backtest and a tape-driven backtest disagree on the
// aggressor of the very same price move. Today the recorder writes 0 and the
// OHLCV source writes 1.
TEST_F(ReplayTradeSideTest, OhlcvSourceEncodesABuyTheWayTheRecorderDoes)
{
  recordTrades({true});
  auto sides = tapeSideBytes(_dir);
  ASSERT_EQ(sides.size(), 1u);
  const uint8_t recorded_buy = sides[0];

  OhlcvReplaySource source(
      {OhlcvReplaySource::Bar{kBaseNs, Price::fromDouble(100.0).raw(), kSymbolId}});

  std::optional<uint8_t> synthetic_side;
  source.forEach(
      [&](const ReplayEvent& ev)
      {
        if (ev.type == EventType::Trade)
        {
          synthetic_side = ev.trade.side;
        }
        return true;
      });

  ASSERT_TRUE(synthetic_side.has_value());
  EXPECT_EQ(recorded_buy, *synthetic_side)
      << "the recorder and the synthetic OHLCV source encode a buy differently";
}

// A tape written by the fixed recorder must be tellable from one written
// before the fix. Two answers satisfy this, and the code agent picks one:
//
//   (a) a format marker on the tape itself -- a bumped SegmentHeader.version,
//       a new SegmentFlags bit, a non-zero byte in SegmentHeader.reserved, or
//       a bumped FrameHeader.rec_version on the trade frame; or
//   (b) a written decision in docs/spec/floxlog.md -- the tape-format spec is
//       where the side byte is defined (currently "0 buy, 1 sell") -- stating
//       that tapes written before the fix carry an inverted side and are not
//       migrated. The check is for the word "invert" (any case) in that file;
//       it appears nowhere in it today.
//
// The test passes when either holds. Today neither does.
TEST_F(ReplayTradeSideTest, FixedTapesAreTellableFromPreFixTapes)
{
  recordTrades({true});

  std::filesystem::path segment;
  for (const auto& entry : std::filesystem::directory_iterator(_dir))
  {
    if (entry.path().extension() == ".floxlog")
    {
      segment = entry.path();
    }
  }
  ASSERT_FALSE(segment.empty());

  bool marker = false;
  {
    std::ifstream in(segment, std::ios::binary);
    ASSERT_TRUE(in.good());

    SegmentHeader header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    ASSERT_EQ(in.gcount(), static_cast<std::streamsize>(sizeof(header)));

    constexpr uint8_t kKnownFlags = SegmentFlags::HasIndex | SegmentFlags::Compressed |
                                    SegmentFlags::Encrypted | SegmentFlags::Sorted;
    marker = marker || header.version != kFormatVersion;
    marker = marker || (header.flags & static_cast<uint8_t>(~kKnownFlags)) != 0;
    for (unsigned char b : header.reserved)
    {
      marker = marker || b != 0;
    }

    FrameHeader frame{};
    in.read(reinterpret_cast<char*>(&frame), sizeof(frame));
    if (in.gcount() == static_cast<std::streamsize>(sizeof(frame)))
    {
      marker = marker || frame.rec_version != 1;
    }
  }

  bool documented = false;
  {
    const std::filesystem::path spec =
        std::filesystem::path(FLOX_REPO_ROOT) / "docs" / "spec" / "floxlog.md";
    std::ifstream in(spec);
    ASSERT_TRUE(in.good()) << "cannot open the tape-format spec at " << spec;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (auto& c : text)
    {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    documented = text.find("invert") != std::string::npos;
  }

  EXPECT_TRUE(marker || documented)
      << "a tape written by the fixed recorder carries no format marker and "
         "docs/spec/floxlog.md records no decision about the tapes written before the fix";
}

// Control, and the other half of the side decision: docs/spec/floxlog.md is
// the published tape format, and its TradeRecord row states which byte means
// buy ("0 buy, 1 sell" today). Whatever the fix settles on, the spec has to
// say the same thing -- a third-party writer follows the spec, not the
// recorder. Green on the untouched tree (the recorder currently agrees with
// the spec and the readers do not), and it stays green only if a fix that
// moves the recorder's byte moves the spec with it.
TEST_F(ReplayTradeSideTest, SpecAndRecorderAgreeOnWhichByteIsTheBuy)
{
  recordTrades({true, false});
  auto sides = tapeSideBytes(_dir);
  ASSERT_EQ(sides.size(), 2u);
  ASSERT_NE(sides[0], sides[1]);

  const std::filesystem::path spec =
      std::filesystem::path(FLOX_REPO_ROOT) / "docs" / "spec" / "floxlog.md";
  std::ifstream in(spec);
  ASSERT_TRUE(in.good()) << "cannot open the tape-format spec at " << spec;

  std::optional<int> documented_buy;
  std::string line;
  while (std::getline(in, line))
  {
    if (line.find("`side`") == std::string::npos)
    {
      continue;
    }
    const size_t buy = line.find("buy");
    if (buy == std::string::npos)
    {
      continue;
    }
    // The digit nearest to the left of the word "buy" is the byte the spec
    // assigns to it.
    for (size_t i = buy; i-- > 0;)
    {
      if (line[i] == '0' || line[i] == '1')
      {
        documented_buy = line[i] - '0';
        break;
      }
    }
    break;
  }

  ASSERT_TRUE(documented_buy.has_value())
      << "docs/spec/floxlog.md no longer states which side byte is the buy";
  EXPECT_EQ(static_cast<int>(sides[0]), *documented_buy)
      << "the recorder encodes a buy as " << static_cast<int>(sides[0])
      << " but docs/spec/floxlog.md says " << *documented_buy;
}

// Control. Everything on the trade record other than the side already survives
// the round trip; it must keep surviving whatever the side fix does to the
// record layout. Green on the untouched tree.
TEST_F(ReplayTradeSideTest, RecordedPriceQuantityAndTimestampRoundTrip)
{
  recordTrades({true, false});

  auto seen = replayTrades(_dir);
  ASSERT_EQ(seen.size(), 2u);
  for (size_t i = 0; i < seen.size(); ++i)
  {
    EXPECT_EQ(seen[i].trade.symbol, kSymbolId);
    EXPECT_EQ(seen[i].trade.price.raw(),
              Price::fromDouble(100.0 + static_cast<double>(i)).raw());
    EXPECT_EQ(seen[i].trade.quantity.raw(), Quantity::fromDouble(0.5).raw());
    EXPECT_EQ(seen[i].trade.exchangeTsNs.raw(),
              static_cast<uint64_t>(kBaseNs + static_cast<int64_t>(i) * 1'000'000'000));
  }
}
