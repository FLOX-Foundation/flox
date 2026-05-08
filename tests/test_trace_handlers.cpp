/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/run/trace_handlers.h"
#include "flox/run/trace_reader.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace
{

class CountingSignalSink : public flox::ISignalHandler
{
 public:
  int count = 0;
  void onSignal(const flox::Signal&) override { ++count; }
};

class CountingExecutionSink : public flox::IOrderExecutionListener
{
 public:
  using flox::IOrderExecutionListener::IOrderExecutionListener;
  int submits = 0;
  int fills = 0;
  void onOrderSubmitted(const flox::Order&) override { ++submits; }
  void onOrderFilled(const flox::Order&) override { ++fills; }
};

flox::run::TraceRecorder makeRec(const std::filesystem::path& p)
{
  std::filesystem::remove_all(p);
  flox::run::TraceRecorderOptions opts;
  opts.strategy_id = "trace-handlers-test";
  opts.run_started_ns = 1'700'000'000'000'000'000LL;
  return flox::run::TraceRecorder(p.string(), std::move(opts));
}

}  // namespace

TEST(TraceHandlers, SignalHandlerForwardsAndRecords)
{
  auto tmp = std::filesystem::temp_directory_path() / "flox_trace_handler_sig";
  auto rec = makeRec(tmp);
  CountingSignalSink inner;
  flox::run::TraceSignalHandler h(&inner, &rec);
  h.setFeedTsNs(1'700'000'000'100'000'000LL);

  auto buy = flox::Signal::marketBuy(1, flox::Quantity::fromDouble(0.5), 42);
  h.onSignal(buy);
  auto sell = flox::Signal::marketSell(2, flox::Quantity::fromDouble(0.7), 43);
  h.onSignal(sell);

  rec.close();
  EXPECT_EQ(inner.count, 2);

  flox::run::TraceReader reader(tmp.string());
  auto sigs = reader.readAllSignals();
  ASSERT_EQ(sigs.size(), 2u);
  EXPECT_EQ(sigs[0].signal_id, 42u);
  EXPECT_EQ(sigs[1].signal_id, 43u);
  EXPECT_EQ(sigs[0].feed_ts_ns, 1'700'000'000'100'000'000LL);
  EXPECT_NE(sigs[0].name, "");
  std::filesystem::remove_all(tmp);
}

TEST(TraceHandlers, ExecutionListenerCapturesEvents)
{
  auto tmp = std::filesystem::temp_directory_path() / "flox_trace_handler_exec";
  auto rec = makeRec(tmp);
  CountingExecutionSink inner(1);
  flox::run::TraceExecutionListener h(2, &inner, &rec);
  h.setFeedTsNs(1'700'000'000'200'000'000LL);

  flox::Order o;
  o.id = 7;
  o.symbol = 1;
  o.side = flox::Side::BUY;
  o.type = flox::OrderType::LIMIT;
  o.price = flox::Price::fromDouble(50000.0);
  o.quantity = flox::Quantity::fromDouble(0.1);

  h.onOrderSubmitted(o);
  h.onOrderAccepted(o);
  h.onOrderFilled(o);
  rec.close();

  EXPECT_EQ(inner.submits, 1);
  EXPECT_EQ(inner.fills, 1);

  flox::run::TraceReader reader(tmp.string());
  auto events = reader.readAllOrderEvents();
  auto fills = reader.readAllFills();
  ASSERT_EQ(events.size(), 2u);  // submit + ack (fill goes to fills segment)
  EXPECT_EQ(events[0].order_id, 7u);
  EXPECT_EQ(events[0].event_kind, flox::run::OrderEventKind::Submit);
  EXPECT_EQ(events[1].event_kind, flox::run::OrderEventKind::Ack);
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].order_id, 7u);
  EXPECT_NEAR(static_cast<double>(fills[0].price_raw) / 1e8, 50000.0, 1e-6);
  std::filesystem::remove_all(tmp);
}

TEST(TraceHandlers, NullRecorderDisablesCaptureWithoutBreakingForward)
{
  CountingSignalSink inner;
  flox::run::TraceSignalHandler h(&inner, nullptr);
  auto buy = flox::Signal::marketBuy(1, flox::Quantity::fromDouble(1.0), 99);
  h.onSignal(buy);
  EXPECT_EQ(inner.count, 1);
}
