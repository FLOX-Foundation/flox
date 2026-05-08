/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/run/trace_reader.h"
#include "flox/run/trace_recorder.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace
{

std::filesystem::path makeTmp(const std::string& tag)
{
  auto p = std::filesystem::temp_directory_path() / ("flox_run_test_" + tag);
  std::filesystem::remove_all(p);
  return p;
}

}  // namespace

TEST(RunTrace, RoundTripsSignalsOrdersFills)
{
  using namespace flox::run;
  auto tmp = makeTmp("roundtrip");

  TraceRecorderOptions opts;
  opts.strategy_id = "smoke";
  opts.strategy_hash = "sha256:test";
  opts.run_started_ns = 1'700'000'000'000'000'000LL;
  TapeRef tref;
  tref.path = "BTCUSDT.floxlog";
  tref.content_hash = "sha256:abc";
  tref.first_event_ns = 1'700'000'000'000'000'000LL;
  tref.last_event_ns = 1'700'000'000'500'000'000LL;
  opts.tape_refs.push_back(tref);

  {
    TraceRecorder rec(tmp.string(), opts);

    SignalView s;
    s.run_ts_ns = 1'700'000'000'100'000'000LL;
    s.feed_ts_ns = 1'700'000'000'099'000'000LL;
    s.signal_id = 42;
    s.flags = SignalFlags::Enter;
    s.strength_raw = 75'000'000;
    s.name = "ratio-cross";
    s.symbol_ids = {1u, 2u};
    std::string payload = "{\"src\":\"ETH\",\"dst\":\"BTC\"}";
    s.payload = payload;
    rec.writeSignal(s);

    OrderEventView e;
    e.run_ts_ns = 1'700'000'000'200'000'000LL;
    e.feed_ts_ns = 1'700'000'000'099'000'000LL;
    e.order_id = 7;
    e.parent_signal_id = 42;
    e.price_raw = 50'000'00000000LL;
    e.qty_raw = 100'000'000LL;
    e.symbol_id = 1;
    e.event_kind = OrderEventKind::Submit;
    e.side = 0;
    e.order_type = 1;
    e.flags = OrderEventFlags::PostOnly;
    rec.writeOrderEvent(e);

    FillView f;
    f.run_ts_ns = 1'700'000'000'300'000'000LL;
    f.feed_ts_ns = 1'700'000'000'250'000'000LL;
    f.order_id = 7;
    f.fill_id = 12345;
    f.price_raw = 50'000'00000000LL;
    f.qty_raw = 100'000'000LL;
    f.fee_raw = 50'000LL;
    f.symbol_id = 1;
    f.side = 0;
    f.liquidity = FillLiquidity::Maker;
    rec.writeFill(f);

    rec.setRunEndedNs(1'700'000'000'400'000'000LL);
    rec.close();
  }

  TraceReader reader(tmp.string());
  const auto& m = reader.manifest();
  EXPECT_EQ(m.format_version, 1u);
  EXPECT_EQ(m.strategy_id, "smoke");
  ASSERT_EQ(m.tape_refs.size(), 1u);
  EXPECT_EQ(m.tape_refs[0].path, "BTCUSDT.floxlog");
  EXPECT_EQ(m.run_ended_ns, 1'700'000'000'400'000'000LL);

  auto sigs = reader.readAllSignals();
  ASSERT_EQ(sigs.size(), 1u);
  EXPECT_EQ(sigs[0].signal_id, 42u);
  EXPECT_EQ(sigs[0].name, "ratio-cross");
  ASSERT_EQ(sigs[0].symbol_ids.size(), 2u);
  EXPECT_EQ(sigs[0].symbol_ids[0], 1u);
  EXPECT_EQ(sigs[0].symbol_ids[1], 2u);
  EXPECT_EQ(std::string(sigs[0].payload.begin(), sigs[0].payload.end()),
            "{\"src\":\"ETH\",\"dst\":\"BTC\"}");
  EXPECT_EQ(sigs[0].flags, flox::run::SignalFlags::Enter);

  auto orders = reader.readAllOrderEvents();
  ASSERT_EQ(orders.size(), 1u);
  EXPECT_EQ(orders[0].order_id, 7u);
  EXPECT_EQ(orders[0].parent_signal_id, 42u);
  EXPECT_EQ(orders[0].event_kind, OrderEventKind::Submit);
  EXPECT_EQ(orders[0].flags, flox::run::OrderEventFlags::PostOnly);

  auto fills = reader.readAllFills();
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].order_id, 7u);
  EXPECT_EQ(fills[0].fill_id, 12345u);
  EXPECT_EQ(fills[0].liquidity, static_cast<uint8_t>(FillLiquidity::Maker));

  std::filesystem::remove_all(tmp);
}

TEST(RunTrace, EmptySegmentsAreOmitted)
{
  using namespace flox::run;
  auto tmp = makeTmp("empty");
  TraceRecorderOptions opts;
  opts.strategy_id = "empty";
  opts.run_started_ns = 1'700'000'000'000'000'000LL;
  {
    TraceRecorder rec(tmp.string(), opts);
    rec.close();
  }
  TraceReader reader(tmp.string());
  EXPECT_TRUE(reader.manifest().segments.empty());
  EXPECT_TRUE(reader.readAllSignals().empty());
  EXPECT_TRUE(reader.readAllOrderEvents().empty());
  EXPECT_TRUE(reader.readAllFills().empty());
  std::filesystem::remove_all(tmp);
}

TEST(RunTrace, MultipleSignalsPreserveOrder)
{
  using namespace flox::run;
  auto tmp = makeTmp("order");
  TraceRecorderOptions opts;
  opts.strategy_id = "ord";
  opts.run_started_ns = 1'000'000'000LL;
  {
    TraceRecorder rec(tmp.string(), opts);
    for (uint32_t i = 0; i < 32; ++i)
    {
      SignalView s;
      s.run_ts_ns = 1'000'000'000LL + i * 1000;
      s.signal_id = i;
      s.name = "tick";
      s.symbol_ids = {i};
      rec.writeSignal(s);
    }
    rec.close();
  }
  TraceReader reader(tmp.string());
  auto sigs = reader.readAllSignals();
  ASSERT_EQ(sigs.size(), 32u);
  for (uint32_t i = 0; i < 32; ++i)
  {
    EXPECT_EQ(sigs[i].signal_id, i);
    ASSERT_EQ(sigs[i].symbol_ids.size(), 1u);
    EXPECT_EQ(sigs[i].symbol_ids[0], i);
  }
  std::filesystem::remove_all(tmp);
}
