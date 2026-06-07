/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/backtest/ntoken_curve.h"
#include "flox/connector/amm_dex_connector.h"
#include "flox/replay/abstract_replay_source.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace flox
{

// One snapshot of an AMM pool: the pool's state at a point in time, captured as a
// ready-to-price curve. The curve is where the state lives, built from recorded
// reserves / ticks by whatever captured them (e.g. the dex-lab readers), so a
// snapshot is curve-agnostic and works for every venue.
struct AmmPoolSnapshot
{
  int64_t tsNs{0};
  std::unique_ptr<INTokenCurve> curve;
};

// Replays a recorded sequence of pool-state snapshots through an AmmDexConnector:
// at each snapshot it points the connector at that curve and republishes the
// synthetic book, stamped at the snapshot's time. This is the engine-side glue the
// AmmDexConnector skeleton calls out as missing -- the curves are exact, this
// turns a pool's state-over-time into the book stream a backtest consumes, and it
// is reproducible (the same snapshots replay identically).
//
// Snapshots must be sorted ascending by tsNs. The connector is borrowed and must
// outlive the source; the snapshots' curves are owned here.
class AmmPoolReplaySource
{
 public:
  AmmPoolReplaySource(AmmDexConnector& connector, std::vector<AmmPoolSnapshot> snapshots)
      : _conn(connector), _snaps(std::move(snapshots))
  {
  }

  std::size_t size() const { return _snaps.size(); }
  bool isFinished() const { return _pos >= _snaps.size(); }
  std::size_t position() const { return _pos; }

  std::optional<TimeRange> dataRange() const
  {
    if (_snaps.empty())
    {
      return std::nullopt;
    }
    return TimeRange{_snaps.front().tsNs, _snaps.back().tsNs};
  }

  // Advance one snapshot: point the connector at this curve and republish its
  // book at the snapshot's time. Returns false when the tape is exhausted.
  bool step()
  {
    if (isFinished())
    {
      return false;
    }
    const AmmPoolSnapshot& s = _snaps[_pos];
    if (s.curve)
    {
      _conn.setCurve(*s.curve);
      _conn.republish(s.tsNs);
    }
    ++_pos;
    return true;
  }

  // Replay all remaining snapshots in order.
  void run()
  {
    while (step())
    {
    }
  }

  // Restart from the beginning (the curves are unchanged; pricing is stateless).
  void reset() { _pos = 0; }

 private:
  AmmDexConnector& _conn;
  std::vector<AmmPoolSnapshot> _snaps;
  std::size_t _pos{0};
};

}  // namespace flox
