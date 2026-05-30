/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstdint>
#include <tuple>
#include <vector>

#include "flox/common.h"
#include "flox/pricing/american.h"
#include "flox/pricing/black_scholes.h"
#include "flox/pricing/greeks.h"
#include "flox/pricing/svi.h"
#include "flox/pricing/vol_cone.h"

namespace py = pybind11;

// Option pricing primitives (flox::pricing). Pure functions, exposed as
// top-level module functions like the batch indicators (adf, hurst_dfa) — not
// C ABI exports, so no IDL group / cross-binding parity requirement.
//
// Cost-of-carry convention b: b=r (stock), b=r-q (dividend), b=0 (future/
// Black-76), b=r-rf (FX). Crypto (Deribit) options are European priced off the
// forward, so rate=carry=0 is the default.
inline void bindPricing(py::module_& m)
{
  if (!py::hasattr(m, "OptionType"))
  {
    py::enum_<flox::OptionType>(m, "OptionType")
        .value("CALL", flox::OptionType::CALL)
        .value("PUT", flox::OptionType::PUT);
  }

  m.def(
      "bs_price",
      [](flox::OptionType type, double spot, double strike, double t, double vol, double rate,
         double carry) -> double
      { return flox::pricing::bsPrice(type, spot, strike, t, rate, carry, vol); },
      py::arg("option_type"), py::arg("spot"), py::arg("strike"), py::arg("t"), py::arg("vol"),
      py::arg("rate") = 0.0, py::arg("carry") = 0.0,
      "Generalized Black-Scholes-Merton price. t in years; rate=discount r; "
      "carry=cost-of-carry b (0 for crypto/Black-76). Returns discounted "
      "intrinsic for expired or zero-vol inputs.");

  m.def(
      "bs_vega",
      [](double spot, double strike, double t, double vol, double rate, double carry) -> double
      { return flox::pricing::bsVega(spot, strike, t, rate, carry, vol); },
      py::arg("spot"), py::arg("strike"), py::arg("t"), py::arg("vol"), py::arg("rate") = 0.0,
      py::arg("carry") = 0.0, "Black-Scholes vega (call/put identical).");

  m.def(
      "implied_vol",
      [](flox::OptionType type, double price, double spot, double strike, double t, double rate,
         double carry) -> py::dict
      {
        const auto r = flox::pricing::impliedVol(type, price, spot, strike, t, rate, carry);
        py::dict d;
        d["vol"] = r.vol;
        d["converged"] = r.converged;
        d["iterations"] = r.iterations;
        return d;
      },
      py::arg("option_type"), py::arg("price"), py::arg("spot"), py::arg("strike"), py::arg("t"),
      py::arg("rate") = 0.0, py::arg("carry") = 0.0,
      "Implied volatility from an option price via Newton-Raphson with bracketed "
      "bisection fallback. Returns dict(vol, converged, iterations); vol is NaN "
      "and converged False when the price violates no-arbitrage bounds.");

  m.def(
      "forward_price",
      [](double spot, double t, double carry) -> double
      { return flox::pricing::forwardPrice(spot, t, carry); },
      py::arg("spot"), py::arg("t"), py::arg("carry"),
      "Forward price spot * exp(carry * t).");

  m.def(
      "binomial_price",
      [](flox::OptionType type, double spot, double strike, double t, double vol, double rate,
         double carry, int steps, bool american) -> double
      {
        return flox::pricing::binomialPrice(type, spot, strike, t, rate, carry, vol, steps,
                                            american);
      },
      py::arg("option_type"), py::arg("spot"), py::arg("strike"), py::arg("t"), py::arg("vol"),
      py::arg("rate") = 0.0, py::arg("carry") = 0.0, py::arg("steps") = 200,
      py::arg("american") = true,
      "Cox-Ross-Rubinstein binomial price. american=True checks early exercise at "
      "every node (American premium); american=False is a European lattice that "
      "converges to bs_price as steps grows.");

  m.def(
      "baw_price",
      [](flox::OptionType type, double spot, double strike, double t, double vol, double rate,
         double carry) -> double
      { return flox::pricing::bawPrice(type, spot, strike, t, rate, carry, vol); },
      py::arg("option_type"), py::arg("spot"), py::arg("strike"), py::arg("t"), py::arg("vol"),
      py::arg("rate") = 0.0, py::arg("carry") = 0.0,
      "Barone-Adesi-Whaley American price: European value plus a closed-form "
      "early-exercise premium. Cheaper than a fine tree, so preferred for greeks "
      "via finite differences. An American call with carry>=rate collapses to the "
      "European price.");

  m.def(
      "greeks",
      [](flox::OptionType type, double spot, double strike, double t, double vol, double rate,
         double carry) -> py::dict
      {
        const auto g = flox::pricing::greeks(type, spot, strike, t, rate, carry, vol);
        py::dict d;
        d["delta"] = g.delta;
        d["gamma"] = g.gamma;
        d["vega"] = g.vega;
        d["theta"] = g.theta;  // per year
        d["rho"] = g.rho;      // b-fixed dV/dr
        return d;
      },
      py::arg("option_type"), py::arg("spot"), py::arg("strike"), py::arg("t"), py::arg("vol"),
      py::arg("rate") = 0.0, py::arg("carry") = 0.0,
      "First-order greeks dict(delta, gamma, vega, theta, rho). theta is per year "
      "(divide by 365 for per-day); rho is the b-fixed partial dV/dr. carry=b "
      "(0 for crypto).");

  m.def(
      "second_order_greeks",
      [](flox::OptionType type, double spot, double strike, double t, double vol, double rate,
         double carry) -> py::dict
      {
        const auto s = flox::pricing::secondOrderGreeks(type, spot, strike, t, rate, carry, vol);
        py::dict d;
        d["vanna"] = s.vanna;  // d(delta)/d(vol)
        d["volga"] = s.volga;  // d(vega)/d(vol)
        d["charm"] = s.charm;  // d(delta)/d(time), per year
        return d;
      },
      py::arg("option_type"), py::arg("spot"), py::arg("strike"), py::arg("t"), py::arg("vol"),
      py::arg("rate") = 0.0, py::arg("carry") = 0.0,
      "Second-order greeks dict(vanna, volga, charm) for vol traders. vanna = "
      "d(delta)/d(vol), volga = d(vega)/d(vol), charm = d(delta)/d(time) per year.");

  // ── SVI implied-volatility surface ──────────────────────────────────────
  m.def(
      "calibrate_svi",
      [](const std::vector<double>& k, const std::vector<double>& w) -> py::dict
      {
        const auto p = flox::pricing::calibrateSVI(k, w);
        py::dict d;
        d["a"] = p.a;
        d["b"] = p.b;
        d["rho"] = p.rho;
        d["m"] = p.m;
        d["sigma"] = p.sigma;
        return d;
      },
      py::arg("log_moneyness"), py::arg("total_variance"),
      "Calibrate a raw-SVI slice to observed (log-moneyness, total-variance) "
      "points by least squares. Needs >= 5 points. Returns dict(a, b, rho, m, "
      "sigma). Total variance is iv**2 * t.");

  if (!py::hasattr(m, "VolSurface"))
  {
    py::class_<flox::pricing::VolSurface>(m, "VolSurface",
                                          "SVI implied-volatility surface: a term structure of "
                                          "calibrated slices that interpolates vol in total-variance "
                                          "space. Mark a backtest to this instead of a flat vol.")
        .def(py::init<>())
        .def(
            "add_slice",
            [](flox::pricing::VolSurface& s, double t, double a, double b, double rho, double mm,
               double sigma)
            { s.addSlice(t, flox::pricing::SVIParams{a, b, rho, mm, sigma}); },
            py::arg("t"), py::arg("a"), py::arg("b"), py::arg("rho"), py::arg("m"), py::arg("sigma"),
            "Add a calibrated SVI slice at expiry t (years).")
        .def("implied_vol", &flox::pricing::VolSurface::impliedVol, py::arg("log_moneyness"),
             py::arg("t"), "Black-Scholes implied vol at (log-moneyness, expiry).")
        .def("total_variance", &flox::pricing::VolSurface::totalVariance, py::arg("log_moneyness"),
             py::arg("t"), "Total implied variance at (log-moneyness, expiry).")
        .def("is_calendar_free", &flox::pricing::VolSurface::isCalendarFree,
             py::arg("k_lo") = -1.5, py::arg("k_hi") = 1.5, py::arg("samples") = 50,
             "True when total variance is non-decreasing in time (no calendar arbitrage).")
        .def("slice_count", &flox::pricing::VolSurface::sliceCount);
  }

  m.def(
      "build_surface_as_of",
      [](const std::vector<std::tuple<int64_t, double, double, double>>& quotes,
         int64_t asof_ns) -> flox::pricing::VolSurface
      {
        std::vector<flox::pricing::DatedVolQuote> dq;
        dq.reserve(quotes.size());
        for (const auto& q : quotes)
        {
          dq.push_back({std::get<0>(q), std::get<1>(q), std::get<2>(q), std::get<3>(q)});
        }
        return flox::pricing::buildSurfaceAsOf(dq, asof_ns);
      },
      py::arg("quotes"), py::arg("asof_ns"),
      "Build a point-in-time VolSurface from (ts_ns, t, log_moneyness, iv) "
      "quotes, using ONLY those stamped on or before asof_ns — the no-lookahead "
      "guarantee for honest backtests. Expiries with < 5 quotes are skipped.");

  // ── Volatility cone (realized vs implied) ───────────────────────────────
  m.def(
      "realized_vol",
      [](const std::vector<double>& returns, double periods_per_year) -> double
      { return flox::pricing::realizedVol(returns, periods_per_year); },
      py::arg("returns"), py::arg("periods_per_year"),
      "Annualized realized vol from log returns: sample stdev * sqrt(periods_per_year).");

  m.def(
      "vol_cone",
      [](const std::vector<double>& prices, const std::vector<size_t>& horizons,
         double periods_per_year) -> py::list
      {
        const auto cone = flox::pricing::volCone(prices, horizons, periods_per_year);
        py::list out;
        for (const auto& c : cone)
        {
          py::dict d;
          d["horizon"] = c.horizon;
          d["min"] = c.min;
          d["p25"] = c.p25;
          d["p50"] = c.p50;
          d["p75"] = c.p75;
          d["max"] = c.max;
          d["samples"] = c.samples;
          out.append(d);
        }
        return out;
      },
      py::arg("prices"), py::arg("horizons"), py::arg("periods_per_year"),
      "Realized-vol cone from a price series: for each horizon (in return "
      "periods) slide a window, annualize realized vol, and report a dict(horizon, "
      "min, p25, p50, p75, max, samples). periods_per_year is 252 (equity) or 365 "
      "(crypto).");

  m.def(
      "implied_percentile_in_cone",
      [](const std::vector<double>& realized_samples, double implied_vol) -> double
      { return flox::pricing::impliedPercentileInCone(realized_samples, implied_vol); },
      py::arg("realized_samples"), py::arg("implied_vol"),
      "Fraction of realized-vol samples at or below implied_vol (0..1). High = "
      "options rich vs realized history; low = cheap. NaN on empty samples.");
}
