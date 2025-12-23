/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox-connectors/execution/timeout_order_tracker.h"

#include <flox/log/log.h>

namespace flox
{

TimeoutOrderTracker::TimeoutOrderTracker(OrderTimeoutConfig config) : _config(std::move(config)) {}

TimeoutOrderTracker::~TimeoutOrderTracker() { stop(); }

void TimeoutOrderTracker::start()
{
  if (_running.exchange(true))
  {
    return;  // Already running
  }

  _checkerThread = std::thread(
      [this]()
      {
        while (_running.load())
        {
          checkTimeouts();

          // Sleep in small intervals to allow quick shutdown
          int sleepMs = _config.checkIntervalMs;
          while (sleepMs > 0 && _running.load())
          {
            int chunk = std::min(sleepMs, 50);
            std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
            sleepMs -= chunk;
          }
        }
      });
}

void TimeoutOrderTracker::stop()
{
  if (!_running.exchange(false))
  {
    return;  // Not running
  }

  if (_checkerThread.joinable())
  {
    _checkerThread.join();
  }
}

void TimeoutOrderTracker::trackSubmit(OrderId orderId)
{
  std::lock_guard lock(_mutex);
  _pending[orderId] = PendingOp{orderId, OpType::SUBMIT, std::chrono::steady_clock::now()};
}

void TimeoutOrderTracker::trackCancel(OrderId orderId)
{
  std::lock_guard lock(_mutex);
  _pending[orderId] = PendingOp{orderId, OpType::CANCEL, std::chrono::steady_clock::now()};
}

void TimeoutOrderTracker::trackReplace(OrderId orderId)
{
  std::lock_guard lock(_mutex);
  _pending[orderId] = PendingOp{orderId, OpType::REPLACE, std::chrono::steady_clock::now()};
}

void TimeoutOrderTracker::clearPending(OrderId orderId)
{
  std::lock_guard lock(_mutex);
  _pending.erase(orderId);
}

bool TimeoutOrderTracker::hasPending(OrderId orderId) const
{
  std::lock_guard lock(_mutex);
  return _pending.find(orderId) != _pending.end();
}

size_t TimeoutOrderTracker::pendingCount() const
{
  std::lock_guard lock(_mutex);
  return _pending.size();
}

void TimeoutOrderTracker::checkTimeouts()
{
  auto now = std::chrono::steady_clock::now();
  std::vector<PendingOp> timedOut;

  {
    std::lock_guard lock(_mutex);
    for (auto it = _pending.begin(); it != _pending.end();)
    {
      const auto& op = it->second;
      auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - op.startTime).count();

      if (elapsed >= getTimeoutMs(op.opType))
      {
        timedOut.push_back(op);
        it = _pending.erase(it);
      }
      else
      {
        ++it;
      }
    }
  }

  // Handle timeouts outside the lock
  for (const auto& op : timedOut)
  {
    auto opStr = opTypeToString(op.opType);

    switch (_config.policy)
    {
      case TimeoutPolicy::LOG_ONLY:
        FLOX_LOG_WARN("[TimeoutOrderTracker] Operation timed out: orderId=" << op.orderId
                                                                            << " op=" << opStr);
        break;

      case TimeoutPolicy::REJECT:
        FLOX_LOG_WARN("[TimeoutOrderTracker] Rejecting timed out order: orderId="
                      << op.orderId << " op=" << opStr);
        if (_config.onReject)
        {
          std::string reason = std::string(opStr) + " timeout";
          _config.onReject(op.orderId, reason);
        }
        break;

      case TimeoutPolicy::CALLBACK:
        if (_config.onTimeout)
        {
          _config.onTimeout(op.orderId, opStr);
        }
        else
        {
          FLOX_LOG_WARN("[TimeoutOrderTracker] Timeout but no callback: orderId="
                        << op.orderId << " op=" << opStr);
        }
        break;

      case TimeoutPolicy::RECONCILE:
        FLOX_LOG_INFO("[TimeoutOrderTracker] Reconcile needed: orderId=" << op.orderId
                                                                         << " op=" << opStr);
        if (_config.onTimeout)
        {
          _config.onTimeout(op.orderId, opStr);
        }
        break;
    }
  }
}

int TimeoutOrderTracker::getTimeoutMs(OpType opType) const
{
  switch (opType)
  {
    case OpType::SUBMIT:
      return _config.submitTimeoutMs;
    case OpType::CANCEL:
      return _config.cancelTimeoutMs;
    case OpType::REPLACE:
      return _config.replaceTimeoutMs;
  }
  return _config.submitTimeoutMs;  // Default
}

std::string_view TimeoutOrderTracker::opTypeToString(OpType opType)
{
  switch (opType)
  {
    case OpType::SUBMIT:
      return "submit";
    case OpType::CANCEL:
      return "cancel";
    case OpType::REPLACE:
      return "replace";
  }
  return "unknown";
}

}  // namespace flox
