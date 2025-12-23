/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <flox/execution/order.h>
#include <flox/util/base/move_only_function.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace flox
{

enum class TimeoutPolicy
{
  LOG_ONLY,  // Only log timeout, don't take any action
  REJECT,    // Mark order as rejected via callback
  CALLBACK,  // Call user-provided callback for custom handling
  RECONCILE  // Request order status from exchange (requires reconcile callback)
};

struct OrderTimeoutConfig
{
  int submitTimeoutMs{5000};
  int cancelTimeoutMs{3000};
  int replaceTimeoutMs{5000};
  int checkIntervalMs{100};
  TimeoutPolicy policy{TimeoutPolicy::REJECT};

  /// Callback invoked when an operation times out
  /// Parameters: orderId, operation type ("submit", "cancel", "replace")
  using TimeoutCallback = MoveOnlyFunction<void(OrderId, std::string_view)>;
  TimeoutCallback onTimeout;

  /// Callback for REJECT policy to mark order as rejected
  using RejectCallback = MoveOnlyFunction<void(OrderId, std::string_view reason)>;
  RejectCallback onReject;

  bool isValid() const
  {
    return submitTimeoutMs > 0 && cancelTimeoutMs > 0 && replaceTimeoutMs > 0 &&
           checkIntervalMs > 0;
  }
};

class TimeoutOrderTracker
{
 public:
  explicit TimeoutOrderTracker(OrderTimeoutConfig config);
  ~TimeoutOrderTracker();

  TimeoutOrderTracker(const TimeoutOrderTracker&) = delete;
  TimeoutOrderTracker& operator=(const TimeoutOrderTracker&) = delete;

  void trackSubmit(OrderId orderId);
  void trackCancel(OrderId orderId);
  void trackReplace(OrderId orderId);
  void clearPending(OrderId orderId);
  bool hasPending(OrderId orderId) const;
  size_t pendingCount() const;

  void start();
  void stop();

 private:
  enum class OpType
  {
    SUBMIT,
    CANCEL,
    REPLACE
  };

  struct PendingOp
  {
    OrderId orderId;
    OpType opType;
    std::chrono::steady_clock::time_point startTime;
  };

  void checkTimeouts();
  int getTimeoutMs(OpType opType) const;
  static std::string_view opTypeToString(OpType opType);

  OrderTimeoutConfig _config;

  mutable std::mutex _mutex;
  std::unordered_map<OrderId, PendingOp> _pending;

  std::atomic<bool> _running{false};
  std::thread _checkerThread;
};

}  // namespace flox
