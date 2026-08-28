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
#include <utility>
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

// Sanitizer builds run the runner thread several times slower, so a wall-clock
// budget that is generous natively goes flaky under TSAN on a loaded CI runner.
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
constexpr int kTimeoutScale = 20;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
constexpr int kTimeoutScale = 20;
#else
constexpr int kTimeoutScale = 1;
#endif
#else
constexpr int kTimeoutScale = 1;
#endif

// Stops the runner and joins on every exit path. A test that ASSERT_*s while
// the worker thread is live returns from the test body with the thread still
// joinable, and std::thread's destructor then calls std::terminate -- aborting
// the whole binary and hiding both the real failure and every later test.
class RunnerThread
{
 public:
  template <typename Fn>
  RunnerThread(BacktestRunner& runner, Fn&& fn) : _runner(runner), _t(std::forward<Fn>(fn))
  {
  }

  ~RunnerThread()
  {
    _runner.stop();
    if (_t.joinable())
    {
      _t.join();
    }
  }

  RunnerThread(const RunnerThread&) = delete;
  RunnerThread& operator=(const RunnerThread&) = delete;

 private:
  BacktestRunner& _runner;
  std::thread _t;
};

// Helper to wait for a condition with timeout
template <typename Pred>
bool waitFor(Pred pred,
             std::chrono::milliseconds timeout = std::chrono::milliseconds(1000 * kTimeoutScale))
{
  auto start = std::chrono::steady_clock::now();
  while (!pred())
  {
    if (std::chrono::steady_clock::now() - start > timeout)
    {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  return true;
}

TEST(InteractiveRunnerTest, StepExecutesOneEvent)
{
  auto events = createTestEvents(10);
  MockReader reader(events);

  BacktestRunner runner;
  std::atomic<int> eventsSeen{0};

  runner.setEventCallback([&](const replay::ReplayEvent&, const BacktestState&)
                          { ++eventsSeen; });

  // Run in background thread
  RunnerThread t(runner, [&]()
                 { runner.start(reader); });

  // Wait for initial pause
  ASSERT_TRUE(waitFor([&]()
                      { return runner.isPaused(); }));

  // Step 3 times
  runner.step();
  ASSERT_TRUE(waitFor([&]()
                      { return eventsSeen.load() >= 1 && runner.isPaused(); }));
  EXPECT_EQ(eventsSeen.load(), 1);

  runner.step();
  ASSERT_TRUE(waitFor([&]()
                      { return eventsSeen.load() >= 2 && runner.isPaused(); }));
  EXPECT_EQ(eventsSeen.load(), 2);

  runner.step();
  ASSERT_TRUE(waitFor([&]()
                      { return eventsSeen.load() >= 3 && runner.isPaused(); }));
  EXPECT_EQ(eventsSeen.load(), 3);
}

TEST(InteractiveRunnerTest, RunUntilBreakpoint)
{
  auto events = createTestEvents(100);
  MockReader reader(events);

  BacktestRunner runner;

  // Break after 50 events
  runner.addBreakpoint(Breakpoint::afterEvents(50));

  RunnerThread t(runner, [&]()
                 { runner.start(reader); });

  ASSERT_TRUE(waitFor([&]()
                      { return runner.isPaused(); }));

  // Run until breakpoint
  runner.resume();
  ASSERT_TRUE(waitFor([&]()
                      { return runner.isPaused() && runner.state().eventCount >= 50; }));

  auto state = runner.state();
  EXPECT_TRUE(state.isPaused);
  EXPECT_GE(state.eventCount, 50u);
  EXPECT_FALSE(state.isFinished);
}

TEST(InteractiveRunnerTest, RunToCompletion)
{
  auto events = createTestEvents(10);
  MockReader reader(events);

  BacktestRunner runner;

  RunnerThread t(runner, [&]()
                 { runner.start(reader); });

  ASSERT_TRUE(waitFor([&]()
                      { return runner.isPaused(); }));

  // Run without breakpoints
  runner.resume();

  // Wait for completion
  ASSERT_TRUE(waitFor([&]()
                      { return runner.isFinished(); }));

  auto state = runner.state();
  EXPECT_TRUE(state.isFinished);
  EXPECT_EQ(state.eventCount, 10u);
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

  RunnerThread t(runner, [&]()
                 { runner.start(reader); });

  ASSERT_TRUE(waitFor([&]()
                      { return runner.isPaused(); }));

  // Step until next trade (should skip 5 book updates)
  runner.stepUntil(BacktestMode::StepTrade);
  ASSERT_TRUE(waitFor([&]()
                      { return runner.isPaused() && runner.state().tradeCount >= 1; }));

  auto state = runner.state();
  EXPECT_EQ(state.tradeCount, 1u);
  EXPECT_EQ(state.bookUpdateCount, 5u);
}

TEST(InteractiveRunnerTest, BreakpointAtTime)
{
  auto events = createTestEvents(100);
  MockReader reader(events);

  BacktestRunner runner;

  // Break at timestamp 50ms
  runner.addBreakpoint(Breakpoint::atTime(UnixNanos::fromRaw(50000000)));

  RunnerThread t(runner, [&]()
                 { runner.start(reader); });

  ASSERT_TRUE(waitFor([&]()
                      { return runner.isPaused(); }));
  runner.resume();
  ASSERT_TRUE(waitFor([&]()
                      { return runner.isPaused() && runner.state().currentTimeNs.raw() >= 50000000; }));

  auto state = runner.state();
  EXPECT_TRUE(state.isPaused);
  EXPECT_GE(state.currentTimeNs.raw(), 50000000);
}

TEST(InteractiveRunnerTest, CustomBreakpoint)
{
  auto events = createTestEvents(100);
  MockReader reader(events);

  BacktestRunner runner;

  // Break when price > 10050
  runner.addBreakpoint(Breakpoint::when([](const replay::ReplayEvent& ev)
                                        { return ev.type == replay::EventType::Trade && ev.trade.price_raw > 10050; }));

  RunnerThread t(runner, [&]()
                 { runner.start(reader); });

  ASSERT_TRUE(waitFor([&]()
                      { return runner.isPaused(); }));
  runner.resume();
  ASSERT_TRUE(waitFor([&]()
                      { return runner.isPaused() && runner.state().eventCount >= 51; }));

  auto state = runner.state();
  EXPECT_TRUE(state.isPaused);
  // Should have processed roughly 51 events (0-50 have prices 10000-10050)
  EXPECT_GE(state.eventCount, 51u);
}

TEST(InteractiveRunnerTest, PauseCallback)
{
  auto events = createTestEvents(10);
  MockReader reader(events);

  BacktestRunner runner;
  std::atomic<int> pauseCount{0};

  runner.setPauseCallback([&](const BacktestState&)
                          { ++pauseCount; });

  RunnerThread t(runner, [&]()
                 { runner.start(reader); });

  // Wait for initial pause callback
  ASSERT_TRUE(waitFor([&]()
                      { return pauseCount.load() >= 1; }));

  // Initial pause
  EXPECT_GE(pauseCount.load(), 1);

  // Step should trigger pause callback
  runner.step();
  ASSERT_TRUE(waitFor([&]()
                      { return pauseCount.load() >= 2; }));
  EXPECT_GE(pauseCount.load(), 2);
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

  RunnerThread t(runner, [&]()
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
  ASSERT_TRUE(waitFor([&]()
                      { return runner.isFinished(); }));

  state = runner.state();
  EXPECT_EQ(state.eventCount, 5u);
  EXPECT_EQ(state.tradeCount, 5u);
  EXPECT_TRUE(state.isFinished);
}
