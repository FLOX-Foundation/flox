/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/execution/algos.h"

#include <algorithm>

namespace flox::execution
{

namespace
{
void requirePositive(double v, const char* name)
{
  if (v <= 0.0)
  {
    throw std::invalid_argument(std::string(name) + " must be positive");
  }
}

void requireNonNegative(double v, const char* name)
{
  if (v < 0.0)
  {
    throw std::invalid_argument(std::string(name) + " must be non-negative");
  }
}
}  // namespace

ExecutionAlgo::ExecutionAlgo(double target_qty, Side side, uint32_t symbol,
                             OrderType type, double limit_price)
    : _target_qty(target_qty),
      _side(side),
      _symbol(symbol),
      _type(type),
      _limit_price(limit_price)
{
  requirePositive(target_qty, "target_qty");
  if (type == OrderType::Limit)
  {
    requirePositive(limit_price, "price");
  }
}

double ExecutionAlgo::remainingQty() const
{
  return std::max(0.0, _target_qty - _submitted_qty);
}

bool ExecutionAlgo::isDone() const { return remainingQty() <= 1e-9; }

void ExecutionAlgo::reportFill(double qty)
{
  requireNonNegative(qty, "fill qty");
  _filled_qty += qty;
}

ChildOrder& ExecutionAlgo::submit(double qty, int64_t now_ns,
                                  OrderType type, double price)
{
  ChildOrder rec;
  rec.order_id = _next_order_id++;
  rec.timestamp_ns = now_ns;
  rec.qty = qty;
  rec.price = price;
  rec.type = type;
  _pending.push_back(rec);
  _submitted_qty += qty;
  return _pending.back();
}

// ── TWAP ─────────────────────────────────────────────────────────

TWAPExecutor::TWAPExecutor(double target_qty, Side side, uint32_t symbol,
                           OrderType type, double limit_price,
                           int64_t duration_ns, uint32_t slice_count,
                           int64_t start_time_ns)
    : ExecutionAlgo(target_qty, side, symbol, type, limit_price),
      _duration_ns(duration_ns),
      _slice_count(slice_count),
      _start_time_ns(start_time_ns)
{
  if (slice_count == 0)
  {
    throw std::invalid_argument("slice_count must be > 0");
  }
  if (duration_ns <= 0)
  {
    throw std::invalid_argument("duration_ns must be > 0");
  }
}

void TWAPExecutor::step(int64_t now_ns)
{
  while (_next_slice_idx < _slice_count && now_ns >= _start_time_ns + static_cast<int64_t>(_next_slice_idx) * sliceIntervalNs())
  {
    const double q = std::min(sliceSize(), remainingQty());
    if (q <= 0.0)
    {
      break;
    }
    submit(q, now_ns, _type, _limit_price);
    ++_next_slice_idx;
  }
}

// ── VWAP ─────────────────────────────────────────────────────────

VWAPExecutor::VWAPExecutor(double target_qty, Side side, uint32_t symbol,
                           OrderType type, double limit_price,
                           std::vector<std::pair<int64_t, double>> volume_curve)
    : ExecutionAlgo(target_qty, side, symbol, type, limit_price),
      _curve(std::move(volume_curve))
{
  if (_curve.empty())
  {
    throw std::invalid_argument("VWAPExecutor needs a non-empty volume_curve");
  }
  for (const auto& [_, vol] : _curve)
  {
    requireNonNegative(vol, "volume_curve volume");
    _total_volume += vol;
  }
  if (_total_volume <= 0.0)
  {
    throw std::invalid_argument("volume_curve total volume must be positive");
  }
}

void VWAPExecutor::step(int64_t now_ns)
{
  while (_bar_idx < _curve.size())
  {
    const auto [bar_ts, bar_vol] = _curve[_bar_idx];
    if (now_ns < bar_ts)
    {
      break;
    }
    ++_bar_idx;
    if (bar_vol <= 0.0)
    {
      continue;
    }
    const double share = bar_vol / _total_volume;
    const double q = std::min(share * _target_qty, remainingQty());
    if (q <= 0.0)
    {
      break;
    }
    submit(q, now_ns, _type, _limit_price);
  }
}

// ── Iceberg ──────────────────────────────────────────────────────

IcebergExecutor::IcebergExecutor(double target_qty, Side side, uint32_t symbol,
                                 OrderType type, double limit_price,
                                 double visible_qty)
    : ExecutionAlgo(target_qty, side, symbol, type, limit_price),
      _visible_qty(visible_qty)
{
  requirePositive(visible_qty, "visible_qty");
  if (visible_qty > target_qty)
  {
    throw std::invalid_argument("visible_qty must not exceed target_qty");
  }
}

void IcebergExecutor::step(int64_t now_ns)
{
  if (isDone())
  {
    return;
  }
  const double outstanding = _submitted_qty - _filled_qty;
  if (outstanding > 1e-9)
  {
    return;
  }
  const double q = std::min(_visible_qty, remainingQty());
  if (q <= 0.0)
  {
    return;
  }
  submit(q, now_ns, _type, _limit_price);
}

// ── POV ──────────────────────────────────────────────────────────

POVExecutor::POVExecutor(double target_qty, Side side, uint32_t symbol,
                         OrderType type, double limit_price,
                         double participation_rate, double min_slice_qty)
    : ExecutionAlgo(target_qty, side, symbol, type, limit_price),
      _participation_rate(participation_rate),
      _min_slice_qty(min_slice_qty)
{
  if (!(participation_rate > 0.0 && participation_rate <= 1.0))
  {
    throw std::invalid_argument("participation_rate must be in (0, 1]");
  }
  requireNonNegative(min_slice_qty, "min_slice_qty");
}

void POVExecutor::observeVolume(double qty)
{
  requireNonNegative(qty, "observed volume");
  _observed_volume += qty;
}

void POVExecutor::step(int64_t now_ns)
{
  if (isDone())
  {
    return;
  }
  const double target = _participation_rate * _observed_volume;
  double slice = target - _submitted_qty;
  if (slice < _min_slice_qty)
  {
    return;
  }
  slice = std::min(slice, remainingQty());
  if (slice <= 0.0)
  {
    return;
  }
  submit(slice, now_ns, _type, _limit_price);
}

}  // namespace flox::execution
