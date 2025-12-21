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
#include <cstdint>

namespace flox
{

enum class RoutingStrategy : uint8_t
{
  BestPrice,      // Route to exchange with best price
  LowestLatency,  // Route to exchange with lowest latency
  LargestSize,    // Route to exchange with largest size at best price
  RoundRobin,     // Round-robin across enabled exchanges
  Explicit        // Use explicit exchange from order
};

enum class FailoverPolicy : uint8_t
{
  Reject,          // Reject order if target exchange unavailable
  FailoverToBest,  // Failover to best available exchange
  Notify           // Notify via callback and wait
};

enum class RoutingError : uint8_t
{
  Success = 0,
  NoExecutor,
  ExchangeDisabled,
  InvalidSymbol,
  RejectedByPolicy
};

class IOrderExecutor
{
 public:
  virtual ~IOrderExecutor() = default;

  virtual void submit(SymbolId symbol,
                      Side side,
                      int64_t priceRaw,
                      int64_t quantityRaw,
                      OrderId orderId) = 0;

  virtual void cancel(OrderId orderId) = 0;
};

template <size_t MaxExchanges = 4>
class OrderRouter : public ISubsystem
{
 public:
  void registerExecutor(ExchangeId exchange, IOrderExecutor* executor)
  {
    if (exchange < MaxExchanges)
    {
      _executors[exchange] = executor;
      _enabled[exchange] = (executor != nullptr);
    }
  }

  void setEnabled(ExchangeId exchange, bool enabled)
  {
    if (exchange < MaxExchanges)
    {
      _enabled[exchange] = enabled && (_executors[exchange] != nullptr);
    }
  }

  bool isEnabled(ExchangeId exchange) const
  {
    return exchange < MaxExchanges && _enabled[exchange];
  }

  void setCompositeBook(CompositeBookMatrix<MaxExchanges>* book) { _book = book; }

  void setClockSync(ExchangeClockSync<MaxExchanges>* clockSync) { _clockSync = clockSync; }

  void setRoutingStrategy(RoutingStrategy strategy) { _strategy = strategy; }

  void setFailoverPolicy(FailoverPolicy policy) { _failoverPolicy = policy; }

  RoutingError route(SymbolId symbol,
                     Side side,
                     int64_t priceRaw,
                     int64_t quantityRaw,
                     OrderId orderId,
                     ExchangeId* outExchange = nullptr)
  {
    ExchangeId target = selectExchange(symbol, side);

    if (target == InvalidExchangeId || target >= MaxExchanges || !_executors[target])
    {
      if (_failoverPolicy == FailoverPolicy::FailoverToBest)
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

    if (!_enabled[target])
    {
      return RoutingError::ExchangeDisabled;
    }

    if (outExchange)
    {
      *outExchange = target;
    }

    _executors[target]->submit(symbol, side, priceRaw, quantityRaw, orderId);
    return RoutingError::Success;
  }

  RoutingError routeTo(ExchangeId exchange,
                       SymbolId symbol,
                       Side side,
                       int64_t priceRaw,
                       int64_t quantityRaw,
                       OrderId orderId)
  {
    if (exchange >= MaxExchanges)
    {
      return RoutingError::NoExecutor;
    }
    if (!_executors[exchange])
    {
      return RoutingError::NoExecutor;
    }
    if (!_enabled[exchange])
    {
      return RoutingError::ExchangeDisabled;
    }

    _executors[exchange]->submit(symbol, side, priceRaw, quantityRaw, orderId);
    return RoutingError::Success;
  }

  RoutingError cancelOn(ExchangeId exchange, OrderId orderId)
  {
    if (exchange >= MaxExchanges)
    {
      return RoutingError::NoExecutor;
    }
    if (!_executors[exchange])
    {
      return RoutingError::NoExecutor;
    }

    _executors[exchange]->cancel(orderId);
    return RoutingError::Success;
  }

  ExchangeId selectExchange(SymbolId symbol, Side side) const
  {
    switch (_strategy)
    {
      case RoutingStrategy::BestPrice:
        return selectByBestPrice(symbol, side);
      case RoutingStrategy::LowestLatency:
        return selectByLowestLatency();
      case RoutingStrategy::LargestSize:
        return selectByLargestSize(symbol, side);
      case RoutingStrategy::RoundRobin:
        return selectRoundRobin();
      case RoutingStrategy::Explicit:
      default:
        return findAnyEnabled();
    }
  }

  size_t enabledCount() const
  {
    size_t count = 0;
    for (size_t ex = 0; ex < MaxExchanges; ++ex)
    {
      if (_enabled[ex])
      {
        ++count;
      }
    }
    return count;
  }

 private:
  ExchangeId selectByBestPrice(SymbolId symbol, Side side) const
  {
    if (!_book)
    {
      return findAnyEnabled();
    }

    if (side == Side::BUY)
    {
      // For buy, we want the lowest ask price
      auto ask = _book->bestAsk(symbol);
      if (ask.valid && _enabled[ask.exchange])
      {
        return ask.exchange;
      }
    }
    else
    {
      // For sell, we want the highest bid price
      auto bid = _book->bestBid(symbol);
      if (bid.valid && _enabled[bid.exchange])
      {
        return bid.exchange;
      }
    }

    return findAnyEnabled();
  }

  ExchangeId selectByLowestLatency() const
  {
    if (!_clockSync)
    {
      return findAnyEnabled();
    }

    ExchangeId best = InvalidExchangeId;
    int64_t bestLatency = std::numeric_limits<int64_t>::max();

    for (size_t ex = 0; ex < MaxExchanges; ++ex)
    {
      if (!_enabled[ex])
      {
        continue;
      }

      auto est = _clockSync->estimate(static_cast<ExchangeId>(ex));
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
    if (!_book)
    {
      return findAnyEnabled();
    }

    ExchangeId best = InvalidExchangeId;
    int64_t bestSize = 0;

    for (size_t ex = 0; ex < MaxExchanges; ++ex)
    {
      if (!_enabled[ex])
      {
        continue;
      }

      int64_t size = 0;
      if (side == Side::BUY)
      {
        auto ask = _book->askForExchange(symbol, static_cast<ExchangeId>(ex));
        if (ask.valid)
        {
          size = ask.qtyRaw;
        }
      }
      else
      {
        auto bid = _book->bidForExchange(symbol, static_cast<ExchangeId>(ex));
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
    for (size_t i = 0; i < MaxExchanges; ++i)
    {
      _rrIndex = (_rrIndex + 1) % MaxExchanges;
      if (_enabled[_rrIndex])
      {
        return static_cast<ExchangeId>(_rrIndex);
      }
    }
    return InvalidExchangeId;
  }

  ExchangeId findAnyEnabled() const
  {
    for (size_t ex = 0; ex < MaxExchanges; ++ex)
    {
      if (_enabled[ex])
      {
        return static_cast<ExchangeId>(ex);
      }
    }
    return InvalidExchangeId;
  }

  std::array<IOrderExecutor*, MaxExchanges> _executors{};
  std::array<bool, MaxExchanges> _enabled{};
  CompositeBookMatrix<MaxExchanges>* _book{nullptr};
  ExchangeClockSync<MaxExchanges>* _clockSync{nullptr};
  RoutingStrategy _strategy{RoutingStrategy::BestPrice};
  FailoverPolicy _failoverPolicy{FailoverPolicy::Reject};
  mutable size_t _rrIndex{0};
};

}  // namespace flox
