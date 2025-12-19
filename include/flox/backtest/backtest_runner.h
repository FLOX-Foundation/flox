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
#include "flox/strategy/signal_strategy.h"

#include <vector>

namespace flox
{

class BacktestRunner : public ISignalHandler
{
 public:
  explicit BacktestRunner(const BacktestConfig& config = {});

  void setStrategy(IStrategy* strategy);
  void setSignalStrategy(SignalStrategy* strategy);
  void addExecutionListener(IOrderExecutionListener* listener);

  BacktestResult run(replay::IMultiSegmentReader& reader);

  void onSignal(const Signal& signal) override;

  SimulatedExecutor& executor() noexcept { return _executor; }
  IClock& clock() noexcept { return _clock; }
  const BacktestConfig& config() const noexcept { return _config; }

 private:
  void onOrderEvent(const OrderEvent& ev);
  Order signalToOrder(const Signal& sig);

  BacktestConfig _config;
  SimulatedClock _clock;
  SimulatedExecutor _executor;
  IStrategy* _strategy{nullptr};
  std::vector<IOrderExecutionListener*> _execution_listeners;
  OrderId _nextOrderId{1};
};

}  // namespace flox
