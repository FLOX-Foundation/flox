/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/aggregator/bar.h"
#include "flox/aggregator/events/bar_event.h"
#include "flox/backtest/backtest_result.h"
#include "flox/backtest/simulated_clock.h"
#include "flox/backtest/simulated_executor.h"
#include "flox/execution/abstract_execution_listener.h"
#include "flox/execution/abstract_executor.h"
#include "flox/killswitch/abstract_killswitch.h"
#include "flox/metrics/abstract_pnl_tracker.h"
#include "flox/position/multi_mode_position_tracker.h"
#include "flox/replay/abstract_event_reader.h"
#include "flox/risk/abstract_risk_manager.h"
#include "flox/strategy/abstract_signal_handler.h"
#include "flox/strategy/strategy.h"
#include "flox/validation/abstract_order_validator.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
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
  void addMarketDataSubscriber(IMarketDataSubscriber* subscriber);
  void addExecutionListener(IOrderExecutionListener* listener);

  /// Replace the built-in SimulatedExecutor with a binding-supplied
  /// IOrderExecutor. When set, all signals (submit / cancel / replace /
  /// OCO / cancelAll) are routed to the custom executor instead of the
  /// built-in simulator. The simulator is left intact for matching live
  /// data into BacktestResult; the custom executor is responsible for
  /// reporting fills via its own ExecutionListener path. Pass nullptr
  /// to revert to the simulated executor.
  /// Caller retains ownership; the runner does not delete the executor.
  void setExecutor(IOrderExecutor* executor) noexcept { _customExecutor = executor; }
  IOrderExecutor* customExecutor() const noexcept { return _customExecutor; }

  /// Pre-trade gate parity with the live `Runner`. All four hooks are
  /// optional; an unset hook is a no-op (let the order through).
  ///
  /// Gates fire on entry-type signals (Market / Limit / Stop* / TP* /
  /// TrailingStop) and on the order they produce. Cancel / CancelAll
  /// / Modify pass through without gating — they reduce, not add,
  /// exposure. Reduce-only orders also bypass: when caps tighten you
  /// do not want to be stuck in a position. This matches how the live
  /// runner is conventionally wired and is documented as a gotcha
  /// surfaced through `lookup_symbol`.
  ///
  /// Caller retains ownership of every hook; the runner holds raw
  /// pointers and does not delete them.
  void setRiskManager(IRiskManager* rm) noexcept { _riskManager = rm; }
  void setOrderValidator(IOrderValidator* ov) noexcept { _orderValidator = ov; }
  void setKillSwitch(IKillSwitch* ks) noexcept { _killSwitch = ks; }
  void setPnLTracker(IPnLTracker* tracker) noexcept { _pnlTracker = tracker; }

  // ========== Non-interactive mode ==========

  /// Run backtest synchronously from start to end
  BacktestResult run(replay::IMultiSegmentReader& reader);

  /// Replay a sequence of pre-built BarEvents through the strategy.
  /// Each bar updates the SimulatedExecutor (so resting orders / SL/TP get
  /// matched against bar.high / bar.low / bar.close) and is dispatched to
  /// Strategy::onBar and any registered IMarketDataSubscriber.
  /// Bars must be in non-decreasing endTime order.
  BacktestResult runBars(const std::vector<BarEvent>& bars);

  /// Run the backtest off a `.floxlog` tape directory. Opens the tape
  /// via `replay::createMultiSegmentReader` and dispatches every event
  /// through `processEvent`. Use this to drive the strategy off a
  /// recorded session (the canonical path: `flox tape record` → write
  /// `.floxlog` → `bt.run_tape(path)`). Throws if `data_dir` is not a
  /// `.floxlog` directory or contains no segments.
  BacktestResult runTape(const std::filesystem::path& data_dir);

  /// Run the backtest off N `.floxlog` tapes merged on read. Symbols
  /// are rekeyed into the engine's symbol registry via
  /// `(metadata.exchange, name)` — strategies that pre-resolved
  /// venue-tagged symbols see the same ids the merger uses. Throws
  /// if any input is not a `.floxlog` directory, or if two inputs
  /// declare overlapping book streams for the same symbol
  /// (`OverlappingBookStreamError`). `runTapes({t})` is equivalent
  /// to `runTape(t)` modulo the rekey.
  BacktestResult runTapes(const std::vector<std::filesystem::path>& data_dirs);

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
  [[nodiscard]] BacktestResult extractResult();

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
  bool passesPreTradeGate(const Order& order);

  BacktestConfig _config;
  SimulatedClock _clock;
  SimulatedExecutor _executor;
  // Optional binding-supplied executor. When non-null, signals are routed
  // here instead of to the built-in SimulatedExecutor.
  IOrderExecutor* _customExecutor{nullptr};
  IStrategy* _strategy{nullptr};
  // Pre-trade gate stack — parity with live Runner. Caller-owned;
  // runner does not delete.
  IRiskManager* _riskManager{nullptr};
  IOrderValidator* _orderValidator{nullptr};
  IKillSwitch* _killSwitch{nullptr};
  IPnLTracker* _pnlTracker{nullptr};
  // Built-in position tracker. Wired as an execution listener at
  // construction and attached to the strategy in setStrategy() so
  // `ctx.position` / `ctx.is_long()` / `ctx.is_flat()` reflect fills
  // the simulator dispatches. Subscriber id is internal — does not
  // collide with user-registered listeners.
  MultiModePositionTracker _positionTracker{0xFEFEFEFEu};
  std::vector<IMarketDataSubscriber*> _marketDataSubscribers;
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
