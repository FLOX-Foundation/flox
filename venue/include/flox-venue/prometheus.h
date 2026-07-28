/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "flox-venue/metrics.h"

#include <cstdint>
#include <string>

namespace flox::venue
{
namespace prom
{

inline std::string u128ToStr(unsigned __int128 v)
{
  if (v == 0)
  {
    return "0";
  }
  char buf[40];
  char* p = buf + sizeof buf;
  while (v > 0)
  {
    *--p = static_cast<char>('0' + static_cast<int>(v % 10));
    v /= 10;
  }
  return std::string(p, buf + sizeof buf);
}

inline std::string i128ToStr(__int128 v)
{
  if (v < 0)
  {
    return "-" + u128ToStr(static_cast<unsigned __int128>(-v));
  }
  return u128ToStr(static_cast<unsigned __int128>(v));
}

// One counter block: HELP + TYPE + value line.
inline void counter(std::string& out, const char* name, const char* help, uint64_t v)
{
  out += "# HELP ";
  out += name;
  out += ' ';
  out += help;
  out += "\n# TYPE ";
  out += name;
  out += " counter\n";
  out += name;
  out += ' ';
  out += std::to_string(v);
  out += '\n';
}

// One gauge block: HELP + TYPE + value line (value already rendered to string).
inline void gaugeStr(std::string& out, const char* name, const char* help, const std::string& v)
{
  out += "# HELP ";
  out += name;
  out += ' ';
  out += help;
  out += "\n# TYPE ";
  out += name;
  out += " gauge\n";
  out += name;
  out += ' ';
  out += v;
  out += '\n';
}

// Latency summary from the histogram: p50/p99/p99.9 quantiles + _sum + _count.
inline void latencySummary(std::string& out, const char* name, const char* help,
                           const LatencyHistogram& h)
{
  out += "# HELP ";
  out += name;
  out += ' ';
  out += help;
  out += "\n# TYPE ";
  out += name;
  out += " summary\n";
  const double qs[3] = {0.5, 0.99, 0.999};
  const char* ql[3] = {"0.5", "0.99", "0.999"};
  for (int i = 0; i < 3; ++i)
  {
    out += name;
    out += "{quantile=\"";
    out += ql[i];
    out += "\"} ";
    out += std::to_string(h.percentileNs(qs[i]));
    out += '\n';
  }
  out += name;
  out += "_sum ";
  out += std::to_string(static_cast<uint64_t>(h.meanNs() * static_cast<double>(h.count())));
  out += '\n';
  out += name;
  out += "_count ";
  out += std::to_string(h.count());
  out += '\n';
}

// Render a full Metrics snapshot as a Prometheus text page.
inline std::string render(const Metrics& m)
{
  std::string out;
  out.reserve(1024);
  counter(out, "fme_orders_accepted_total", "Orders accepted onto the book", m.accepted);
  counter(out, "fme_trades_total", "Trades matched", m.trades);
  counter(out, "fme_cancels_total", "Orders canceled", m.cancels);
  counter(out, "fme_rejects_total", "Orders rejected", m.rejects);
  counter(out, "fme_modifies_total", "Orders modified", m.modifies);
  counter(out, "fme_liquidations_total", "Positions liquidated", m.liquidations);
  counter(out, "fme_fill_holds_total", "Fills held for last-look", m.holds);

  out += "# HELP fme_traded_volume_raw Cumulative traded notional (quote, fixed-point raw)\n";
  out += "# TYPE fme_traded_volume_raw counter\n";
  out += "fme_traded_volume_raw ";
  out += i128ToStr(m.volumeRaw);
  out += '\n';

  // Per-reason reject breakdown (labeled series). Only nonzero reasons emitted.
  out += "# HELP fme_rejects_by_reason_total Orders rejected, by reason\n";
  out += "# TYPE fme_rejects_by_reason_total counter\n";
  for (size_t i = 0; i < Metrics::kReasons; ++i)
  {
    if (m.rejectsByReason[i] == 0)
    {
      continue;
    }
    out += "fme_rejects_by_reason_total{reason=\"";
    out += toString(static_cast<RejectReason>(i));
    out += "\"} ";
    out += std::to_string(m.rejectsByReason[i]);
    out += '\n';
  }

  latencySummary(out, "fme_submit_latency_ns", "Submit-to-response latency (ns)", m.submitLatency);
  return out;
}

// Render counters + latency AND point-in-time venue gauges (insurance, funding,
// open interest) sampled from the ledger/risk manager.
inline std::string render(const Metrics& m, const Gauges& g)
{
  std::string out = render(m);
  gaugeStr(out, "fme_insurance_fund_raw", "Insurance fund balance (quote, fixed-point raw)",
           i128ToStr(g.insuranceFundRaw));
  gaugeStr(out, "fme_funding_rate", "Last settled funding rate (fraction)",
           std::to_string(g.fundingRate));
  gaugeStr(out, "fme_open_interest_raw", "Aggregate open position notional (quote raw)",
           i128ToStr(g.openInterestRaw));
  gaugeStr(out, "fme_open_positions", "Open perp positions", std::to_string(g.openPositions));
  gaugeStr(out, "fme_resting_orders", "Live resting orders", std::to_string(g.restingOrders));
  gaugeStr(out, "fme_mark_price_age_ns", "Age of the current mark/index (ns; feed-lag alert)",
           std::to_string(g.markPriceAgeNs));
  gaugeStr(out, "fme_liquidations_paused", "1 = liquidation circuit breaker engaged",
           std::to_string(g.liquidationsPaused));
  return out;
}

}  // namespace prom
}  // namespace flox::venue
