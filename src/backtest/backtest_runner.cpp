/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/backtest/backtest_runner.h"

#include "flox/book/events/book_update_event.h"
#include "flox/book/events/trade_event.h"
#include "flox/replay/merged_tape_reader.h"
#include "flox/strategy/strategy.h"

#include <memory_resource>

namespace flox
{

BacktestRunner::BacktestRunner(const BacktestConfig& config)
    : _config(config), _executor(_clock), _pool(std::make_unique<std::pmr::monotonic_buffer_resource>(1024 * 1024))
{
  _executor.applyConfig(_config);
  _executor.setOrderEventCallback(
      [this](const OrderEvent& ev)
      {
        // The built-in position tracker has to receive fills before
        // any user listener — strategies that read `ctx.position`
        // inside an order-event handler need the tracker already
        // updated when their listener fires. User listeners are
        // appended after via addExecutionListener().
        ev.dispatchTo(_positionTracker);
        // PnL tracker parity with live Runner: an attached tracker
        // sees every fill (PARTIALLY_FILLED + FILLED) regardless of
        // whether user listeners are wired. Mirrors the live path,
        // so a strategy validated in backtest produces the same
        // tracker timeline in production.
        if (_pnlTracker != nullptr &&
            (ev.status == OrderEventStatus::FILLED ||
             ev.status == OrderEventStatus::PARTIALLY_FILLED))
        {
          _pnlTracker->onOrderFilled(ev.order);
        }
        for (auto* listener : _executionListeners)
        {
          ev.dispatchTo(*listener);
          listener->onOrderEvent(ev);
        }
        // Forward to the attached strategy so user-side `on_fill` /
        // `on_order_update` hooks fire. Without this the strategy
        // never learns its emitted orders filled — native stops are
        // unusable, and exit logic that wants to confirm a close
        // has no callback to hang on.
        if (_strategy)
        {
          _strategy->onOrderEvent(ev);
        }
      });
}

void BacktestRunner::setStrategy(IStrategy* strategy)
{
  _strategy = strategy;
  strategy->setSignalHandler(this);
  // Without this wire `ctx.position` / `ctx.is_long()` / `ctx.is_flat()`
  // never reflect fills the executor dispatches. Strategy::onTrade /
  // onBar / onBookUpdate refresh the per-symbol context from this
  // tracker before each handler call.
  strategy->setPositionManager(&_positionTracker);
}

void BacktestRunner::addMarketDataSubscriber(IMarketDataSubscriber* subscriber)
{
  _marketDataSubscribers.push_back(subscriber);
}

void BacktestRunner::addExecutionListener(IOrderExecutionListener* listener)
{
  _executionListeners.push_back(listener);
}

// ========== Non-interactive mode ==========

BacktestResult BacktestRunner::run(replay::IMultiSegmentReader& reader)
{
  _interactiveMode = false;
  _running.store(true, std::memory_order_release);
  _paused.store(false, std::memory_order_release);
  _finished.store(false, std::memory_order_release);
  _eventCount = 0;
  _tradeCount = 0;
  _bookUpdateCount = 0;
  _signalCount = 0;

  if (_strategy)
  {
    _strategy->start();
  }

  reader.forEach([this](const replay::ReplayEvent& event) -> bool
                 {
    processEvent(event);
    return true; });

  if (_strategy)
  {
    _strategy->stop();
  }

  _finished.store(true, std::memory_order_release);
  _running.store(false, std::memory_order_release);

  return result();
}

BacktestResult BacktestRunner::runTape(const std::filesystem::path& data_dir)
{
  if (!std::filesystem::exists(data_dir))
  {
    throw std::runtime_error(
        "BacktestRunner.runTape: directory does not exist: " +
        data_dir.string());
  }
  if (!std::filesystem::is_directory(data_dir))
  {
    throw std::runtime_error(
        "BacktestRunner.runTape: path is not a directory: " +
        data_dir.string());
  }
  auto reader = replay::createMultiSegmentReader(data_dir);
  if (!reader)
  {
    throw std::runtime_error(
        "BacktestRunner.runTape: cannot open `.floxlog` directory: " +
        data_dir.string());
  }
  return run(*reader);
}

BacktestResult BacktestRunner::runTapes(
    const std::vector<std::filesystem::path>& data_dirs)
{
  if (data_dirs.empty())
  {
    throw std::runtime_error("BacktestRunner.runTapes: empty paths list");
  }
  for (const auto& d : data_dirs)
  {
    if (!std::filesystem::exists(d) || !std::filesystem::is_directory(d))
    {
      throw std::runtime_error(
          "BacktestRunner.runTapes: not a directory: " + d.string());
    }
  }
  replay::MergedTapeReaderConfig cfg{};
  cfg.tape_dirs = data_dirs;
  replay::MergedTapeReader merger(std::move(cfg));
  auto adapter = merger.asMultiSegmentReader();
  return run(*adapter);
}

BacktestResult BacktestRunner::runBars(const std::vector<BarEvent>& bars)
{
  _interactiveMode = false;
  _running.store(true, std::memory_order_release);
  _paused.store(false, std::memory_order_release);
  _finished.store(false, std::memory_order_release);
  _eventCount = 0;
  _tradeCount = 0;
  _bookUpdateCount = 0;
  _signalCount = 0;

  if (_strategy)
  {
    _strategy->start();
  }

  for (const auto& ev : bars)
  {
    _clock.advanceTo(ev.bar.endTime.time_since_epoch().count());
    ++_eventCount;

    // Feed the bar's close into the executor so resting orders can match.
    // (SimulatedExecutor::onBar already handles SL/TP triggers using close.)
    _executor.onBar(ev.symbol, ev.bar.close);

    if (_strategy)
    {
      _strategy->onBar(ev);
    }
    for (auto* sub : _marketDataSubscribers)
    {
      sub->onBar(ev);
    }
  }

  if (_strategy)
  {
    _strategy->stop();
  }

  _finished.store(true, std::memory_order_release);
  _running.store(false, std::memory_order_release);

  return result();
}

// ========== Interactive mode ==========

void BacktestRunner::start(replay::IMultiSegmentReader& reader)
{
  _interactiveMode = true;
  _running.store(true, std::memory_order_release);
  _paused.store(true, std::memory_order_release);
  _finished.store(false, std::memory_order_release);
  _eventCount = 0;
  _tradeCount = 0;
  _bookUpdateCount = 0;
  _signalCount = 0;

  if (_strategy)
  {
    _strategy->start();
  }

  reader.forEach(
      [this](const replay::ReplayEvent& event) -> bool
      {
        if (!_running.load(std::memory_order_acquire))
        {
          return false;
        }

        // Check if we should pause
        if (_paused.load(std::memory_order_acquire) && !_stepRequested.load(std::memory_order_acquire))
        {
          notifyPaused();
          waitForResume();
          if (!_running.load(std::memory_order_acquire))
          {
            return false;
          }
        }

        // Process the event
        processEvent(event);

        // Check breakpoints after processing
        if (checkBreakpoints(event))
        {
          _paused.store(true, std::memory_order_release);
          notifyPaused();
        }

        // Handle step mode
        if (_stepRequested.load(std::memory_order_acquire))
        {
          BacktestMode mode = _stepMode.load(std::memory_order_acquire);
          bool shouldPause = false;

          switch (mode)
          {
            case BacktestMode::Step:
              shouldPause = true;
              break;
            case BacktestMode::StepTrade:
              shouldPause = (event.type == replay::EventType::Trade);
              break;
            case BacktestMode::Run:
              shouldPause = false;
              break;
          }

          if (shouldPause)
          {
            _stepRequested.store(false, std::memory_order_release);
            _paused.store(true, std::memory_order_release);
          }
        }

        return true;
      });

  _finished.store(true, std::memory_order_release);
  _running.store(false, std::memory_order_release);

  if (_strategy)
  {
    _strategy->stop();
  }

  notifyPaused();
}

void BacktestRunner::processEvent(const replay::ReplayEvent& event)
{
  _clock.advanceTo(event.timestamp_ns);
  ++_eventCount;
  _lastEventType = event.type;

  if (event.type == replay::EventType::Trade)
  {
    ++_tradeCount;

    TradeEvent trade_ev;
    trade_ev.trade.symbol = event.trade.symbol_id;
    trade_ev.trade.price = Price::fromRaw(event.trade.price_raw);
    trade_ev.trade.quantity = Quantity::fromRaw(event.trade.qty_raw);
    trade_ev.trade.isBuy = (event.trade.side == 1);
    trade_ev.trade.exchangeTsNs = event.trade.exchange_ts_ns;
    trade_ev.trade.instrument = static_cast<InstrumentType>(event.trade.instrument);
    trade_ev.exchangeMsgTsNs = event.trade.exchange_ts_ns;

    _executor.onTrade(trade_ev.trade.symbol, trade_ev.trade.price, trade_ev.trade.quantity,
                      trade_ev.trade.isBuy);

    if (_strategy)
    {
      _strategy->onTrade(trade_ev);
    }

    for (auto* subscriber : _marketDataSubscribers)
    {
      subscriber->onTrade(trade_ev);
    }
  }
  else if (event.type == replay::EventType::BookSnapshot || event.type == replay::EventType::BookDelta)
  {
    ++_bookUpdateCount;

    BookUpdateEvent book_ev(_pool.get());
    book_ev.update.symbol = event.book_header.symbol_id;
    book_ev.update.type =
        (event.type == replay::EventType::BookSnapshot) ? BookUpdateType::SNAPSHOT : BookUpdateType::DELTA;
    book_ev.update.instrument = static_cast<InstrumentType>(event.book_header.instrument);
    book_ev.update.exchangeTsNs = event.book_header.exchange_ts_ns;
    book_ev.seq = event.book_header.seq;

    book_ev.update.bids.reserve(event.bids.size());
    book_ev.update.asks.reserve(event.asks.size());

    for (const auto& bid : event.bids)
    {
      book_ev.update.bids.emplace_back(Price::fromRaw(bid.price_raw), Quantity::fromRaw(bid.qty_raw));
    }
    for (const auto& ask : event.asks)
    {
      book_ev.update.asks.emplace_back(Price::fromRaw(ask.price_raw), Quantity::fromRaw(ask.qty_raw));
    }

    _executor.onBookUpdate(book_ev.update.symbol, book_ev.update.bids, book_ev.update.asks);

    if (_strategy)
    {
      _strategy->onBookUpdate(book_ev);
    }

    for (auto* subscriber : _marketDataSubscribers)
    {
      subscriber->onBookUpdate(book_ev);
    }

    _pool->release();
  }

  // Invoke callback if set (interactive mode)
  if (_eventCallback)
  {
    _eventCallback(event, state());
  }
}

bool BacktestRunner::checkBreakpoints(const replay::ReplayEvent& event)
{
  if (!_interactiveMode)
  {
    return false;
  }

  // Check signal breakpoint
  if (_breakOnSignal && _signalEmitted)
  {
    _signalEmitted = false;
    return true;
  }

  for (const auto& bp : _breakpoints)
  {
    switch (bp.type)
    {
      case Breakpoint::Type::Time:
        if (event.timestamp_ns >= static_cast<int64_t>(bp.timestampNs))
        {
          return true;
        }
        break;
      case Breakpoint::Type::EventCount:
        if (_eventCount >= bp.count)
        {
          return true;
        }
        break;
      case Breakpoint::Type::TradeCount:
        if (_tradeCount >= bp.count)
        {
          return true;
        }
        break;
      case Breakpoint::Type::Signal:
        // Handled above with _signalEmitted
        break;
      case Breakpoint::Type::Custom:
        if (bp.predicate && bp.predicate(event))
        {
          return true;
        }
        break;
    }
  }
  return false;
}

void BacktestRunner::waitForResume()
{
  std::unique_lock lock(_mutex);
  _cv.wait(lock,
           [this]
           {
             return !_paused.load(std::memory_order_acquire) || _stepRequested.load(std::memory_order_acquire) ||
                    !_running.load(std::memory_order_acquire);
           });
}

void BacktestRunner::notifyPaused()
{
  if (_pauseCallback)
  {
    _pauseCallback(state());
  }
}

void BacktestRunner::resume()
{
  _stepRequested.store(false, std::memory_order_release);
  _paused.store(false, std::memory_order_release);
  _cv.notify_all();
}

void BacktestRunner::step()
{
  _stepMode.store(BacktestMode::Step, std::memory_order_release);
  _stepRequested.store(true, std::memory_order_release);
  _paused.store(false, std::memory_order_release);
  _cv.notify_all();
}

void BacktestRunner::stepUntil(BacktestMode mode)
{
  _stepMode.store(mode, std::memory_order_release);
  _stepRequested.store(true, std::memory_order_release);
  _paused.store(false, std::memory_order_release);
  _cv.notify_all();
}

void BacktestRunner::pause()
{
  _paused.store(true, std::memory_order_release);
}

void BacktestRunner::stop()
{
  _running.store(false, std::memory_order_release);
  _paused.store(false, std::memory_order_release);
  _cv.notify_all();
}

void BacktestRunner::addBreakpoint(Breakpoint bp)
{
  std::lock_guard lock(_mutex);
  _breakpoints.push_back(std::move(bp));
}

void BacktestRunner::clearBreakpoints()
{
  std::lock_guard lock(_mutex);
  _breakpoints.clear();
}

void BacktestRunner::setBreakOnSignal(bool enable)
{
  _breakOnSignal = enable;
}

BacktestState BacktestRunner::state() const
{
  return BacktestState{.currentTimeNs = static_cast<UnixNanos>(_clock.nowNs()),
                       .eventCount = _eventCount,
                       .tradeCount = _tradeCount,
                       .bookUpdateCount = _bookUpdateCount,
                       .signalCount = _signalCount,
                       .isRunning = _running.load(std::memory_order_acquire),
                       .isPaused = _paused.load(std::memory_order_acquire),
                       .isFinished = _finished.load(std::memory_order_acquire),
                       .lastEventType = _lastEventType};
}

BacktestResult BacktestRunner::result() const
{
  BacktestResult res(_config, _executor.fills().size());
  for (const auto& fill : _executor.fills())
  {
    res.recordFill(fill);
  }
  return res;
}

BacktestResult BacktestRunner::extractResult()
{
  BacktestResult res(_config, _executor.fills().size());
  auto fills = _executor.extractFills();
  for (auto& fill : fills)
  {
    res.recordFill(fill);
  }
  return res;
}

// Pre-trade gate. Returns true if the order should pass through;
// false if any gate rejected it. Reduce-only orders bypass — when
// caps tighten you do not want to be stuck in a position; the live
// runner is conventionally wired this way and `lookup_symbol`
// surfaces a gotcha entry pointing at this contract.
bool BacktestRunner::passesPreTradeGate(const Order& order)
{
  if (order.flags.reduceOnly)
  {
    return true;
  }
  if (_killSwitch != nullptr)
  {
    // Engine contract: check(order) may flip state; isTriggered()
    // reads the current state. Call check first so a per-order rule
    // (e.g. "trip if cumulative loss > X") gets evaluated for this
    // order before we read the trigger.
    _killSwitch->check(order);
    if (_killSwitch->isTriggered())
    {
      return false;
    }
  }
  if (_orderValidator != nullptr)
  {
    std::string reason;
    if (!_orderValidator->validate(order, reason))
    {
      return false;
    }
  }
  if (_riskManager != nullptr && !_riskManager->allow(order))
  {
    return false;
  }
  return true;
}

void BacktestRunner::onSignal(const Signal& signal)
{
  ++_signalCount;
  _signalEmitted = true;

  // Route to custom executor if attached, else to the built-in simulator.
  IOrderExecutor& exec = (_customExecutor != nullptr)
                             ? *_customExecutor
                             : static_cast<IOrderExecutor&>(_executor);

  switch (signal.type)
  {
    case SignalType::Market:
    case SignalType::Limit:
    case SignalType::StopMarket:
    case SignalType::StopLimit:
    case SignalType::TakeProfitMarket:
    case SignalType::TakeProfitLimit:
    case SignalType::TrailingStop:
    {
      Order order = signalToOrder(signal);
      if (!passesPreTradeGate(order))
      {
        return;
      }
      exec.submitOrder(order);
      break;
    }
    case SignalType::Cancel:
      // Cancel / CancelAll / Modify do not add exposure — same
      // pass-through semantics as the live runner.
      exec.cancelOrder(signal.orderId);
      break;
    case SignalType::CancelAll:
      exec.cancelAllOrders(signal.symbol);
      break;
    case SignalType::Modify:
    {
      Order newOrder{.id = _nextOrderId++, .price = signal.newPrice, .quantity = signal.newQuantity};
      exec.replaceOrder(signal.orderId, newOrder);
      break;
    }
    case SignalType::OCO:
    {
      // OCO creates two linked orders
      Order order1 = signalToOrder(signal);
      order1.type = OrderType::LIMIT;
      order1.price = signal.price;

      Order order2{.id = _nextOrderId++,
                   .side = signal.side,
                   .price = signal.triggerPrice,
                   .quantity = signal.quantity,
                   .type = OrderType::LIMIT,
                   .symbol = signal.symbol,
                   .timeInForce = signal.timeInForce,
                   .flags = {.reduceOnly = signal.reduceOnly, .postOnly = signal.postOnly}};

      // Both legs of an OCO need to pass; rejecting either drops the pair.
      if (!passesPreTradeGate(order1) || !passesPreTradeGate(order2))
      {
        return;
      }
      OCOParams params{.order1 = order1, .order2 = order2};
      exec.submitOCO(params);
      break;
    }
  }
}

Order BacktestRunner::signalToOrder(const Signal& sig)
{
  OrderId id = (sig.orderId != 0) ? sig.orderId : _nextOrderId++;

  OrderType orderType{};
  switch (sig.type)
  {
    case SignalType::Market:
      orderType = OrderType::MARKET;
      break;
    case SignalType::Limit:
      orderType = OrderType::LIMIT;
      break;
    case SignalType::StopMarket:
      orderType = OrderType::STOP_MARKET;
      break;
    case SignalType::StopLimit:
      orderType = OrderType::STOP_LIMIT;
      break;
    case SignalType::TakeProfitMarket:
      orderType = OrderType::TAKE_PROFIT_MARKET;
      break;
    case SignalType::TakeProfitLimit:
      orderType = OrderType::TAKE_PROFIT_LIMIT;
      break;
    case SignalType::TrailingStop:
      orderType = OrderType::TRAILING_STOP;
      break;
    default:
      orderType = OrderType::MARKET;
      break;
  }

  return Order{.id = id,
               .side = sig.side,
               .price = sig.price,
               .quantity = sig.quantity,
               .type = orderType,
               .symbol = sig.symbol,
               .timeInForce = sig.timeInForce,
               .flags = {.reduceOnly = sig.reduceOnly, .postOnly = sig.postOnly},
               .triggerPrice = sig.triggerPrice,
               .trailingOffset = sig.trailingOffset,
               .trailingCallbackRate = sig.trailingCallbackRate};
}

}  // namespace flox
