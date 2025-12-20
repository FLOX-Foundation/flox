/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/backtest_runner.h"
#include "flox/replay/abstract_event_reader.h"

#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace flox;

namespace
{

// Simple in-memory reader for testing
class MockReader : public replay::IMultiSegmentReader
{
 public:
  explicit MockReader(std::vector<replay::ReplayEvent> events) : _events(std::move(events)) {}

  uint64_t forEach(EventCallback callback) override
  {
    uint64_t count = 0;
    for (const auto& ev : _events)
    {
      if (!callback(ev))
      {
        break;
      }
      ++count;
    }
    return count;
  }

  uint64_t forEachFrom(int64_t start_ts_ns, EventCallback callback) override
  {
    uint64_t count = 0;
    for (const auto& ev : _events)
    {
      if (ev.timestamp_ns < start_ts_ns)
      {
        continue;
      }
      if (!callback(ev))
      {
        break;
      }
      ++count;
    }
    return count;
  }

  const std::vector<replay::SegmentInfo>& segments() const override { return _segments; }
  uint64_t totalEvents() const override { return _events.size(); }

 private:
  std::vector<replay::ReplayEvent> _events;
  std::vector<replay::SegmentInfo> _segments;
};

std::vector<replay::ReplayEvent> createTestEvents(size_t count)
{
  std::vector<replay::ReplayEvent> events;
  events.reserve(count);

  for (size_t i = 0; i < count; ++i)
  {
    replay::ReplayEvent ev{};
    ev.type = replay::EventType::Trade;
    ev.timestamp_ns = static_cast<int64_t>((i + 1) * 1000000);  // 1ms apart
    ev.trade.symbol_id = 1;
    ev.trade.price_raw = 10000 + static_cast<int64_t>(i);
    ev.trade.qty_raw = 100;
    ev.trade.side = (i % 2 == 0) ? 1 : 0;
    ev.trade.exchange_ts_ns = ev.timestamp_ns;
    events.push_back(ev);
  }

  return events;
}

}  // namespace

TEST(InteractiveRunnerTest, StepExecutesOneEvent)
{
  auto events = createTestEvents(10);
  MockReader reader(events);

  BacktestRunner runner;
  std::atomic<int> eventsSeen{0};

  runner.setEventCallback([&](const replay::ReplayEvent&, const BacktestState&)
                          { ++eventsSeen; });

  // Run in background thread
  std::thread t([&]()
                { runner.start(reader); });

  // Wait for initial pause
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // Step 3 times
  runner.step();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_EQ(eventsSeen.load(), 1);

  runner.step();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_EQ(eventsSeen.load(), 2);

  runner.step();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_EQ(eventsSeen.load(), 3);

  // Stop and join
  runner.stop();
  t.join();
}

TEST(InteractiveRunnerTest, RunUntilBreakpoint)
{
  auto events = createTestEvents(100);
  MockReader reader(events);

  BacktestRunner runner;

  // Break after 50 events
  runner.addBreakpoint(Breakpoint::afterEvents(50));

  std::thread t([&]()
                { runner.start(reader); });

  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // Run until breakpoint
  runner.resume();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto state = runner.state();
  EXPECT_TRUE(state.isPaused);
  EXPECT_GE(state.eventCount, 50u);
  EXPECT_FALSE(state.isFinished);

  runner.stop();
  t.join();
}

TEST(InteractiveRunnerTest, RunToCompletion)
{
  auto events = createTestEvents(10);
  MockReader reader(events);

  BacktestRunner runner;

  std::thread t([&]()
                { runner.start(reader); });

  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // Run without breakpoints
  runner.resume();

  // Wait for completion
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto state = runner.state();
  EXPECT_TRUE(state.isFinished);
  EXPECT_EQ(state.eventCount, 10u);

  t.join();
}

TEST(InteractiveRunnerTest, StepUntilTrade)
{
  std::vector<replay::ReplayEvent> events;

  // Add some book updates
  for (int i = 0; i < 5; ++i)
  {
    replay::ReplayEvent ev{};
    ev.type = replay::EventType::BookDelta;
    ev.timestamp_ns = (i + 1) * 1000000;
    ev.book_header.symbol_id = 1;
    events.push_back(ev);
  }

  // Add a trade
  replay::ReplayEvent trade{};
  trade.type = replay::EventType::Trade;
  trade.timestamp_ns = 6000000;
  trade.trade.symbol_id = 1;
  trade.trade.price_raw = 10000;
  trade.trade.qty_raw = 100;
  trade.trade.side = 1;
  events.push_back(trade);

  MockReader reader(events);
  BacktestRunner runner;

  std::thread t([&]()
                { runner.start(reader); });

  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // Step until next trade (should skip 5 book updates)
  runner.stepUntil(BacktestMode::StepTrade);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto state = runner.state();
  EXPECT_EQ(state.tradeCount, 1u);
  EXPECT_EQ(state.bookUpdateCount, 5u);

  runner.stop();
  t.join();
}

TEST(InteractiveRunnerTest, BreakpointAtTime)
{
  auto events = createTestEvents(100);
  MockReader reader(events);

  BacktestRunner runner;

  // Break at timestamp 50ms
  runner.addBreakpoint(Breakpoint::atTime(50000000));

  std::thread t([&]()
                { runner.start(reader); });

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  runner.resume();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto state = runner.state();
  EXPECT_TRUE(state.isPaused);
  EXPECT_GE(state.currentTimeNs, 50000000u);

  runner.stop();
  t.join();
}

TEST(InteractiveRunnerTest, CustomBreakpoint)
{
  auto events = createTestEvents(100);
  MockReader reader(events);

  BacktestRunner runner;

  // Break when price > 10050
  runner.addBreakpoint(Breakpoint::when([](const replay::ReplayEvent& ev)
                                        { return ev.type == replay::EventType::Trade && ev.trade.price_raw > 10050; }));

  std::thread t([&]()
                { runner.start(reader); });

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  runner.resume();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto state = runner.state();
  EXPECT_TRUE(state.isPaused);
  // Should have processed roughly 51 events (0-50 have prices 10000-10050)
  EXPECT_GE(state.eventCount, 51u);

  runner.stop();
  t.join();
}

TEST(InteractiveRunnerTest, PauseCallback)
{
  auto events = createTestEvents(10);
  MockReader reader(events);

  BacktestRunner runner;
  std::atomic<int> pauseCount{0};

  runner.setPauseCallback([&](const BacktestState&)
                          { ++pauseCount; });

  std::thread t([&]()
                { runner.start(reader); });

  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // Initial pause
  EXPECT_GE(pauseCount.load(), 1);

  // Step should trigger pause callback
  runner.step();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_GE(pauseCount.load(), 2);

  runner.stop();
  t.join();
}

TEST(InteractiveRunnerTest, StateInspection)
{
  auto events = createTestEvents(5);
  MockReader reader(events);

  BacktestRunner runner;

  std::atomic<bool> initialPauseCalled{false};
  runner.setPauseCallback(
      [&](const BacktestState& s)
      {
        if (s.eventCount == 0 && !initialPauseCalled.load())
        {
          initialPauseCalled.store(true);
        }
      });

  std::thread t([&]()
                { runner.start(reader); });

  // Wait for initial pause callback
  while (!initialPauseCalled.load())
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  auto state = runner.state();
  EXPECT_EQ(state.eventCount, 0u);
  EXPECT_TRUE(state.isPaused);
  EXPECT_FALSE(state.isFinished);

  runner.resume();

  // Wait for completion
  t.join();

  state = runner.state();
  EXPECT_EQ(state.eventCount, 5u);
  EXPECT_EQ(state.tradeCount, 5u);
  EXPECT_TRUE(state.isFinished);
}
