/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/strategy/abstract_signal_handler.h"
#include "flox/strategy/abstract_strategy.h"
#include "flox/strategy/signal.h"

namespace flox
{

/**
 * Base class for signal-based strategies.
 *
 * Instead of directly calling executor.submitOrder(), strategies emit signals
 * which are then handled by the backtest runner or live execution engine.
 */
class SignalStrategy : public IStrategy
{
 public:
  void setSignalHandler(ISignalHandler* handler) noexcept { _signalHandler = handler; }

 protected:
  void emit(const Signal& signal)
  {
    if (_signalHandler)
    {
      _signalHandler->onSignal(signal);
    }
  }

  void emitMarketBuy(SymbolId symbol, Quantity qty) { emit(Signal::marketBuy(symbol, qty)); }

  void emitMarketSell(SymbolId symbol, Quantity qty) { emit(Signal::marketSell(symbol, qty)); }

  void emitLimitBuy(SymbolId symbol, Price price, Quantity qty)
  {
    emit(Signal::limitBuy(symbol, price, qty));
  }

  void emitLimitSell(SymbolId symbol, Price price, Quantity qty)
  {
    emit(Signal::limitSell(symbol, price, qty));
  }

  void emitCancel(OrderId orderId) { emit(Signal::cancel(orderId)); }

  void emitCancelAll(SymbolId symbol) { emit(Signal::cancelAll(symbol)); }

 private:
  ISignalHandler* _signalHandler{nullptr};
};

}  // namespace flox
