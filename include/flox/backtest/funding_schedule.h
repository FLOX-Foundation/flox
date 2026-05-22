/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/common.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace flox
{

// One funding payment as recorded by FundingSchedule::tick.
//
// `amount` is signed: positive means the position holder received
// the payment, negative means they paid it. Sign follows the
// convention long pays positive funding (rate>0), short pays
// negative funding.
struct FundingPayment
{
  int64_t timestampNs{0};
  SymbolId symbol{};
  double rate{0.0};
  double markPrice{0.0};
  double positionSigned{0.0};  // signed position (long = +)
  double amount{0.0};          // settled value in margin currency
};

// Funding schedule. Two construction modes:
//
//   constant(interval_ns, rate)
//       Fixed-interval, constant-rate model. Useful for stress tests
//       and quick parametric runs. The rate is applied every
//       interval_ns boundary.
//
//   tape(events)
//       Point-in-time recorded rates from an exchange archive. Each
//       (timestamp_ns, rate) pair triggers one payment at that ts.
//
// Canned profiles match the published intervals for major venues
// (Binance UM 8h, Bybit linear 8h, OKX 8h, Bitget some markets 1h).
// The rate value in canned profiles is zero — callers must supply
// a real rate via a tape or override.
class FundingSchedule
{
 public:
  FundingSchedule() = default;

  static FundingSchedule constant(int64_t intervalNs, double rate)
  {
    FundingSchedule s;
    s._intervalNs = intervalNs;
    s._constantRate = rate;
    return s;
  }

  static FundingSchedule tape(std::vector<std::pair<int64_t, double>> events)
  {
    FundingSchedule s;
    s._tape = std::move(events);
    std::sort(s._tape.begin(), s._tape.end(),
              [](const auto& a, const auto& b)
              { return a.first < b.first; });
    return s;
  }

  static FundingSchedule binance_um_futures()
  {
    // 8-hour fixed cadence, default rate zero (caller overrides).
    return constant(8LL * 3600LL * 1'000'000'000LL, 0.0);
  }

  static FundingSchedule bybit_linear()
  {
    return constant(8LL * 3600LL * 1'000'000'000LL, 0.0);
  }

  static FundingSchedule okx_swap() { return binance_um_futures(); }

  static FundingSchedule bitget_hourly()
  {
    return constant(3600LL * 1'000'000'000LL, 0.0);
  }

  bool isInterval() const noexcept { return _intervalNs > 0; }
  int64_t intervalNs() const noexcept { return _intervalNs; }
  double constantRate() const noexcept { return _constantRate; }
  const std::vector<std::pair<int64_t, double>>& tapeEvents() const noexcept
  {
    return _tape;
  }

  void setConstantRate(double rate) noexcept { _constantRate = rate; }

  // Walk every funding event in (lastTickNs, nowNs] and emit a
  // FundingPayment for each (symbol, position, mark) triple. The
  // caller supplies the current per-symbol position and mark price;
  // the schedule does the timing + math:
  //
  //   amount = -position_signed * mark * rate
  //
  // Sign convention: long (positive position) pays out when rate is
  // positive, so amount is negative for the long; short receives
  // (amount positive). Update internal lastTickNs so subsequent
  // calls do not re-emit the same events.
  std::vector<FundingPayment> tick(int64_t nowNs,
                                   const std::vector<SymbolId>& symbols,
                                   const std::vector<double>& positions,
                                   const std::vector<double>& markPrices)
  {
    std::vector<FundingPayment> out;
    if (_intervalNs > 0)
    {
      // Fixed-interval mode: emit every boundary in (lastTick, now].
      const int64_t firstBoundary = ((_lastTickNs / _intervalNs) + 1) * _intervalNs;
      for (int64_t t = firstBoundary; t <= nowNs; t += _intervalNs)
      {
        if (t <= _lastTickNs)
        {
          continue;
        }
        for (size_t i = 0; i < symbols.size(); ++i)
        {
          if (positions[i] == 0.0)
          {
            continue;
          }
          FundingPayment p;
          p.timestampNs = t;
          p.symbol = symbols[i];
          p.rate = _constantRate;
          p.markPrice = markPrices[i];
          p.positionSigned = positions[i];
          p.amount = -positions[i] * markPrices[i] * _constantRate;
          out.push_back(p);
        }
      }
    }
    else
    {
      // Tape mode: walk recorded events.
      for (const auto& [ts, rate] : _tape)
      {
        if (ts <= _lastTickNs || ts > nowNs)
        {
          continue;
        }
        for (size_t i = 0; i < symbols.size(); ++i)
        {
          if (positions[i] == 0.0)
          {
            continue;
          }
          FundingPayment p;
          p.timestampNs = ts;
          p.symbol = symbols[i];
          p.rate = rate;
          p.markPrice = markPrices[i];
          p.positionSigned = positions[i];
          p.amount = -positions[i] * markPrices[i] * rate;
          out.push_back(p);
        }
      }
    }
    _lastTickNs = nowNs;
    return out;
  }

  // Reset the internal cursor — useful when restarting a backtest.
  void reset() noexcept { _lastTickNs = 0; }
  int64_t lastTickNs() const noexcept { return _lastTickNs; }

 private:
  int64_t _intervalNs{0};
  double _constantRate{0.0};
  std::vector<std::pair<int64_t, double>> _tape;
  int64_t _lastTickNs{0};
};

}  // namespace flox
