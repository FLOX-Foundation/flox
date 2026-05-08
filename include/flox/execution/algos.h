/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace flox::execution
{

// Side and order-type are exchanged as small enums to keep the C ABI
// compact. The bindings translate from / to their idiomatic strings.
enum class Side : uint8_t
{
  Buy = 0,
  Sell = 1,
};

enum class OrderType : uint8_t
{
  Market = 0,
  Limit = 1,
};

struct ChildOrder
{
  uint64_t order_id{0};
  int64_t timestamp_ns{0};
  double qty{0.0};
  double price{0.0};
  OrderType type{OrderType::Market};
};

// Abstract base. Subclasses keep state, accumulate child orders into
// `_pending` from `step(now_ns)`. The binding caller reads `pending`,
// dispatches to its own executor, then calls `clearPending()` before
// the next step.
class ExecutionAlgo
{
 public:
  ExecutionAlgo(double target_qty, Side side, uint32_t symbol,
                OrderType type, double limit_price);
  virtual ~ExecutionAlgo() = default;

  virtual void step(int64_t now_ns) = 0;

  // Optional: the user reports child-order fills back so algos that
  // gate on `filled_qty` (Iceberg) can advance.
  void reportFill(double qty);

  // POV-only entry point. No-op on other algos.
  virtual void observeVolume(double /*qty*/) {}

  // Pending child orders since the last `clearPending()`.
  const std::vector<ChildOrder>& pending() const { return _pending; }
  void clearPending() { _pending.clear(); }

  double targetQty() const { return _target_qty; }
  double submittedQty() const { return _submitted_qty; }
  double filledQty() const { return _filled_qty; }
  double remainingQty() const;
  bool isDone() const;

 protected:
  ChildOrder& submit(double qty, int64_t now_ns,
                     OrderType type, double price);

  double _target_qty;
  Side _side;
  uint32_t _symbol;
  OrderType _type;
  double _limit_price;
  uint64_t _next_order_id{1};
  double _submitted_qty{0.0};
  double _filled_qty{0.0};
  std::vector<ChildOrder> _pending;
};

class TWAPExecutor final : public ExecutionAlgo
{
 public:
  TWAPExecutor(double target_qty, Side side, uint32_t symbol,
               OrderType type, double limit_price,
               int64_t duration_ns, uint32_t slice_count,
               int64_t start_time_ns);

  void step(int64_t now_ns) override;

  int64_t sliceIntervalNs() const
  {
    return _slice_count > 0 ? _duration_ns / static_cast<int64_t>(_slice_count) : 0;
  }
  double sliceSize() const
  {
    return _slice_count > 0 ? _target_qty / static_cast<double>(_slice_count) : 0.0;
  }

 private:
  int64_t _duration_ns;
  uint32_t _slice_count;
  int64_t _start_time_ns;
  uint32_t _next_slice_idx{0};
};

class VWAPExecutor final : public ExecutionAlgo
{
 public:
  VWAPExecutor(double target_qty, Side side, uint32_t symbol,
               OrderType type, double limit_price,
               std::vector<std::pair<int64_t, double>> volume_curve);

  void step(int64_t now_ns) override;

 private:
  std::vector<std::pair<int64_t, double>> _curve;
  double _total_volume{0.0};
  size_t _bar_idx{0};
};

class IcebergExecutor final : public ExecutionAlgo
{
 public:
  IcebergExecutor(double target_qty, Side side, uint32_t symbol,
                  OrderType type, double limit_price,
                  double visible_qty);

  void step(int64_t now_ns) override;

 private:
  double _visible_qty;
};

class POVExecutor final : public ExecutionAlgo
{
 public:
  POVExecutor(double target_qty, Side side, uint32_t symbol,
              OrderType type, double limit_price,
              double participation_rate, double min_slice_qty);

  void step(int64_t now_ns) override;
  void observeVolume(double qty) override;

 private:
  double _participation_rate;
  double _min_slice_qty;
  double _observed_volume{0.0};
};

}  // namespace flox::execution
