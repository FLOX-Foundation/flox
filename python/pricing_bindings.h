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

#include "flox/common.h"
#include "flox/pricing/american.h"
#include "flox/pricing/black_scholes.h"
#include "flox/pricing/greeks.h"

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
}
