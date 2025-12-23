/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/backtest/backtest_result.h"
#include "flox/backtest/simulated_clock.h"
#include "flox/backtest/simulated_executor.h"
#include "flox/execution/abstract_execution_listener.h"
#include "flox/replay/abstract_event_reader.h"
#include "flox/strategy/abstract_signal_handler.h"
#include "flox/strategy/abstract_strategy.h"
#include "flox/strategy/strategy.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace flox
{

/// Execution mode for interactive backtest
enum class BacktestMode
{
  Run,       ///< Run continuously until end or breakpoint
  Step,      ///< Execute one event then pause
  StepTrade  ///< Execute until next trade
};

/// Breakpoint condition
struct Breakpoint
{
  enum class Type
  {
    Time,        ///< Break at specific timestamp
    EventCount,  ///< Break after N events
    TradeCount,  ///< Break after N trades
    Signal,      ///< Break when strategy emits signal
    Custom       ///< Custom predicate
  };

  Type type{Type::EventCount};
  UnixNanos timestampNs{0};
  uint64_t count{0};
  std::function<bool(const replay::ReplayEvent&)> predicate;

  static Breakpoint atTime(UnixNanos ts) { return {.type = Type::Time, .timestampNs = ts}; }
  static Breakpoint afterEvents(uint64_t n) { return {.type = Type::EventCount, .count = n}; }
  static Breakpoint afterTrades(uint64_t n) { return {.type = Type::TradeCount, .count = n}; }
  static Breakpoint onSignal() { return {.type = Type::Signal}; }
  static Breakpoint when(std::function<bool(const replay::ReplayEvent&)> pred)
  {
    return {.type = Type::Custom, .predicate = std::move(pred)};
  }
};

/// Current state snapshot for inspection
struct BacktestState
{
  UnixNanos currentTimeNs{0};
  uint64_t eventCount{0};
  uint64_t tradeCount{0};
  uint64_t bookUpdateCount{0};
  uint64_t signalCount{0};
  bool isRunning{false};
  bool isPaused{false};
  bool isFinished{false};

  std::optional<replay::EventType> lastEventType;
};

/// Backtest runner with optional interactive mode (pause/step/breakpoints)
class BacktestRunner : public ISignalHandler
{
 public:
  using EventCallback = std::function<void(const replay::ReplayEvent&, const BacktestState&)>;
  using PauseCallback = std::function<void(const BacktestState&)>;

  explicit BacktestRunner(const BacktestConfig& config = {});

  // Strategy setup
  void setStrategy(IStrategy* strategy);
  void setStrategy(Strategy* strategy);
  void addExecutionListener(IOrderExecutionListener* listener);

  // ========== Non-interactive mode ==========

  /// Run backtest synchronously from start to end
  BacktestResult run(replay::IMultiSegmentReader& reader);

  // ========== Interactive mode ==========

  /// Start backtest in interactive mode (starts paused, must call from separate thread)
  void start(replay::IMultiSegmentReader& reader);

  /// Run until end or breakpoint
  void resume();

  /// Execute one event then pause
  void step();

  /// Execute until condition (trade, etc.)
  void stepUntil(BacktestMode mode);

  /// Pause execution
  void pause();

  /// Stop execution completely
  void stop();

  // Breakpoints
  void addBreakpoint(Breakpoint bp);
  void clearBreakpoints();
  void setBreakOnSignal(bool enable);

  // State inspection
  [[nodiscard]] BacktestState state() const;
  bool isPaused() const { return _paused.load(std::memory_order_acquire); }
  bool isFinished() const { return _finished.load(std::memory_order_acquire); }

  // Callbacks (interactive mode only)
  void setEventCallback(EventCallback cb) { _eventCallback = std::move(cb); }
  void setPauseCallback(PauseCallback cb) { _pauseCallback = std::move(cb); }

  // Results
  [[nodiscard]] BacktestResult result() const;

  // ISignalHandler
  void onSignal(const Signal& signal) override;

  // Access internals
  SimulatedExecutor& executor() noexcept { return _executor; }
  IClock& clock() noexcept { return _clock; }
  const BacktestConfig& config() const noexcept { return _config; }

 private:
  void processEvent(const replay::ReplayEvent& event);
  bool checkBreakpoints(const replay::ReplayEvent& event);
  void waitForResume();
  void notifyPaused();
  Order signalToOrder(const Signal& sig);

  BacktestConfig _config;
  SimulatedClock _clock;
  SimulatedExecutor _executor;
  IStrategy* _strategy{nullptr};
  std::vector<IOrderExecutionListener*> _executionListeners;
  OrderId _nextOrderId{1};

  // Interactive state
  std::atomic<bool> _running{false};
  std::atomic<bool> _paused{false};
  std::atomic<bool> _finished{false};
  std::atomic<bool> _stepRequested{false};
  std::atomic<BacktestMode> _stepMode{BacktestMode::Step};
  bool _interactiveMode{false};

  uint64_t _eventCount{0};
  uint64_t _tradeCount{0};
  uint64_t _bookUpdateCount{0};
  uint64_t _signalCount{0};
  std::optional<replay::EventType> _lastEventType;

  // Breakpoints
  std::vector<Breakpoint> _breakpoints;
  bool _breakOnSignal{false};
  bool _signalEmitted{false};

  // Synchronization
  mutable std::mutex _mutex;
  std::condition_variable _cv;

  // Callbacks
  EventCallback _eventCallback;
  PauseCallback _pauseCallback;

  // Memory pool for book updates
  std::unique_ptr<std::pmr::monotonic_buffer_resource> _pool;
};

}  // namespace flox
