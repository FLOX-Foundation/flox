/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include "flox/book/composite_book_matrix.h"
#include "flox/common.h"
#include "flox/engine/abstract_subsystem.h"
#include "flox/util/sync/exchange_clock_sync.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace flox
{

enum class RoutingStrategy : uint8_t
{
  BestPrice,      // Route to exchange with best price
  LowestLatency,  // Route to exchange with lowest latency
  LargestSize,    // Route to exchange with largest size at best price
  RoundRobin      // Round-robin across enabled exchanges
  // (No "Explicit" strategy: route() carries no per-order exchange. To target a
  //  specific venue use routeTo(exchange, ...), which is the explicit API.)
};

enum class FailoverPolicy : uint8_t
{
  Reject,         // Reject order if target exchange unavailable
  FailoverToBest  // Failover to best available exchange
  // (No "Notify" policy: there is no callback member to notify through. A
  //  caller that wants notify-and-wait builds it around Reject.)
};

enum class RoutingError : uint8_t
{
  Success = 0,
  NoExecutor,
  ExchangeDisabled
};

// The router's own narrow order sink. Deliberately NOT
// flox::IOrderExecutor from execution/abstract_executor.h: that one is
// the full executor interface (submitOrder(const Order&), cancelOrder,
// replaceOrder, submitOCO, capabilities) and is what SimulatedExecutor
// implements. Two classes with the same fully-qualified name in one
// program is an ODR violation, so this one carries a distinct name.
class IRoutableExecutor
{
 public:
  virtual ~IRoutableExecutor() = default;

  // Price and Quantity, not two int64_t raws: this is the one call where a
  // swapped pair cannot be recovered afterwards, and the Decimal tags exist
  // precisely to make that swap a compile error.
  virtual void submit(SymbolId symbol,
                      Side side,
                      Price price,
                      Quantity quantity,
                      OrderId orderId) = 0;

  virtual void cancel(OrderId orderId) = 0;
};

template <size_t MaxExchanges = 4>
class OrderRouter : public ISubsystem
{
 public:
  void registerExecutor(ExchangeId exchange, IRoutableExecutor* executor)
  {
    if (exchange < MaxExchanges)
    {
      // Release: a routing thread that acquires the pointer also sees the
      // executor the control thread finished constructing.
      _executors[exchange].store(executor, std::memory_order_release);
      _enabled[exchange].store(executor != nullptr, std::memory_order_release);
    }
  }

  void setEnabled(ExchangeId exchange, bool enabled)
  {
    if (exchange < MaxExchanges)
    {
      const bool hasExecutor = _executors[exchange].load(std::memory_order_acquire) != nullptr;
      _enabled[exchange].store(enabled && hasExecutor, std::memory_order_release);
    }
  }

  bool isEnabled(ExchangeId exchange) const
  {
    return exchange < MaxExchanges && _enabled[exchange].load(std::memory_order_acquire);
  }

  void setCompositeBook(CompositeBookMatrix<MaxExchanges>* book) { _book.store(book, std::memory_order_release); }

  void setClockSync(ExchangeClockSync<MaxExchanges>* clockSync) { _clockSync.store(clockSync, std::memory_order_release); }

  void setRoutingStrategy(RoutingStrategy strategy) { _strategy.store(strategy, std::memory_order_relaxed); }

  void setFailoverPolicy(FailoverPolicy policy) { _failoverPolicy.store(policy, std::memory_order_relaxed); }

  RoutingError route(SymbolId symbol,
                     Side side,
                     Price price,
                     Quantity quantity,
                     OrderId orderId,
                     ExchangeId* outExchange = nullptr)
  {
    ExchangeId target = selectExchange(symbol, side);

    if (target == InvalidExchangeId || target >= MaxExchanges || !executor(target))
    {
      if (_failoverPolicy.load(std::memory_order_relaxed) == FailoverPolicy::FailoverToBest)
      {
        target = findAnyEnabled();
        if (target == InvalidExchangeId)
        {
          return RoutingError::NoExecutor;
        }
      }
      else
      {
        return RoutingError::NoExecutor;
      }
    }

    if (!_enabled[target].load(std::memory_order_acquire))
    {
      return RoutingError::ExchangeDisabled;
    }

    // Re-read after the enabled check rather than reusing the earlier probe:
    // the pointer is what the call below dereferences.
    auto* targetExecutor = executor(target);
    if (!targetExecutor)
    {
      return RoutingError::NoExecutor;
    }

    if (outExchange)
    {
      *outExchange = target;
    }

    targetExecutor->submit(symbol, side, price, quantity, orderId);
    return RoutingError::Success;
  }

  RoutingError routeTo(ExchangeId exchange,
                       SymbolId symbol,
                       Side side,
                       Price price,
                       Quantity quantity,
                       OrderId orderId)
  {
    if (exchange >= MaxExchanges)
    {
      return RoutingError::NoExecutor;
    }
    auto* targetExecutor = executor(exchange);
    if (!targetExecutor)
    {
      return RoutingError::NoExecutor;
    }
    if (!_enabled[exchange].load(std::memory_order_acquire))
    {
      return RoutingError::ExchangeDisabled;
    }

    targetExecutor->submit(symbol, side, price, quantity, orderId);
    return RoutingError::Success;
  }

  RoutingError cancelOn(ExchangeId exchange, OrderId orderId)
  {
    if (exchange >= MaxExchanges)
    {
      return RoutingError::NoExecutor;
    }
    auto* targetExecutor = executor(exchange);
    if (!targetExecutor)
    {
      return RoutingError::NoExecutor;
    }

    targetExecutor->cancel(orderId);
    return RoutingError::Success;
  }

  ExchangeId selectExchange(SymbolId symbol, Side side) const
  {
    switch (_strategy.load(std::memory_order_relaxed))
    {
      case RoutingStrategy::BestPrice:
        return selectByBestPrice(symbol, side);
      case RoutingStrategy::LowestLatency:
        return selectByLowestLatency();
      case RoutingStrategy::LargestSize:
        return selectByLargestSize(symbol, side);
      case RoutingStrategy::RoundRobin:
        return selectRoundRobin();
      default:
        return findAnyEnabled();
    }
  }

  size_t enabledCount() const
  {
    size_t count = 0;
    for (size_t ex = 0; ex < MaxExchanges; ++ex)
    {
      if (_enabled[ex].load(std::memory_order_acquire))
      {
        ++count;
      }
    }
    return count;
  }

 private:
  IRoutableExecutor* executor(ExchangeId exchange) const
  {
    return _executors[exchange].load(std::memory_order_acquire);
  }

  ExchangeId selectByBestPrice(SymbolId symbol, Side side) const
  {
    auto* book = _book.load(std::memory_order_acquire);
    if (!book)
    {
      return findAnyEnabled();
    }

    if (side == Side::BUY)
    {
      // For buy, we want the lowest ask price
      auto ask = book->bestAsk(symbol);
      if (ask.valid && ask.exchange < MaxExchanges && _enabled[ask.exchange].load(std::memory_order_acquire))
      {
        return ask.exchange;
      }
    }
    else
    {
      // For sell, we want the highest bid price
      auto bid = book->bestBid(symbol);
      if (bid.valid && bid.exchange < MaxExchanges && _enabled[bid.exchange].load(std::memory_order_acquire))
      {
        return bid.exchange;
      }
    }

    return findAnyEnabled();
  }

  ExchangeId selectByLowestLatency() const
  {
    auto* clockSync = _clockSync.load(std::memory_order_acquire);
    if (!clockSync)
    {
      return findAnyEnabled();
    }

    ExchangeId best = InvalidExchangeId;
    int64_t bestLatency = std::numeric_limits<int64_t>::max();

    for (size_t ex = 0; ex < MaxExchanges; ++ex)
    {
      if (!_enabled[ex].load(std::memory_order_acquire))
      {
        continue;
      }

      auto est = clockSync->estimate(static_cast<ExchangeId>(ex));
      if (est.sampleCount > 0 && est.latencyNs < bestLatency)
      {
        bestLatency = est.latencyNs;
        best = static_cast<ExchangeId>(ex);
      }
    }

    return best != InvalidExchangeId ? best : findAnyEnabled();
  }

  ExchangeId selectByLargestSize(SymbolId symbol, Side side) const
  {
    auto* book = _book.load(std::memory_order_acquire);
    if (!book)
    {
      return findAnyEnabled();
    }

    ExchangeId best = InvalidExchangeId;
    int64_t bestSize = 0;

    for (size_t ex = 0; ex < MaxExchanges; ++ex)
    {
      if (!_enabled[ex].load(std::memory_order_acquire))
      {
        continue;
      }

      int64_t size = 0;
      if (side == Side::BUY)
      {
        auto ask = book->askForExchange(symbol, static_cast<ExchangeId>(ex));
        if (ask.valid)
        {
          size = ask.qtyRaw;
        }
      }
      else
      {
        auto bid = book->bidForExchange(symbol, static_cast<ExchangeId>(ex));
        if (bid.valid)
        {
          size = bid.qtyRaw;
        }
      }

      if (size > bestSize)
      {
        bestSize = size;
        best = static_cast<ExchangeId>(ex);
      }
    }

    return best != InvalidExchangeId ? best : findAnyEnabled();
  }

  ExchangeId selectRoundRobin() const
  {
    // One fetch_add per probe: two routing threads each get their own slot
    // instead of read-modify-writing a plain counter, and neither can hand
    // the same destination to both.
    for (size_t i = 0; i < MaxExchanges; ++i)
    {
      const size_t idx = _rrIndex.fetch_add(1, std::memory_order_relaxed) % MaxExchanges;
      if (_enabled[idx].load(std::memory_order_acquire))
      {
        return static_cast<ExchangeId>(idx);
      }
    }
    return InvalidExchangeId;
  }

  ExchangeId findAnyEnabled() const
  {
    for (size_t ex = 0; ex < MaxExchanges; ++ex)
    {
      if (_enabled[ex].load(std::memory_order_acquire))
      {
        return static_cast<ExchangeId>(ex);
      }
    }
    return InvalidExchangeId;
  }

  // Every word route() reads is written by a control thread (registerExecutor,
  // setEnabled, the setters) while a strategy thread routes, and _rrIndex is
  // read-modify-written by the routing threads themselves. All of it is atomic
  // and none of it takes a lock: route() stays on the order path.
  std::array<std::atomic<IRoutableExecutor*>, MaxExchanges> _executors{};
  std::array<std::atomic<bool>, MaxExchanges> _enabled{};
  std::atomic<CompositeBookMatrix<MaxExchanges>*> _book{nullptr};
  std::atomic<ExchangeClockSync<MaxExchanges>*> _clockSync{nullptr};
  std::atomic<RoutingStrategy> _strategy{RoutingStrategy::BestPrice};
  std::atomic<FailoverPolicy> _failoverPolicy{FailoverPolicy::Reject};
  mutable std::atomic<size_t> _rrIndex{0};
};

}  // namespace flox
