/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include <cmath>

namespace flox::detail
{

// Central-difference derivative of f at x, with the step clamped inside
// [lo, hi] so it never probes outside the bracket.
template <typename F>
inline double centralDeriv(F f, double x, double lo, double hi)
{
  double h = 1e-7 * std::fabs(x);
  if (h < 1e-12)
  {
    h = 1e-12;
  }
  double xp = x + h;
  double xm = x - h;
  if (xp > hi)
  {
    xp = hi;
  }
  if (xm < lo)
  {
    xm = lo;
  }
  const double span = xp - xm;
  if (span <= 0.0)
  {
    return 0.0;
  }
  return (f(xp) - f(xm)) / span;
}

// Safeguarded Newton (Numerical Recipes rtsafe): a Newton step when it stays in
// the bracket and makes progress, a bisection step otherwise. [lo, hi] must
// straddle the root (f changes sign across it). scale sets the convergence
// tolerance. Used by the cryptoswap curves, whose invariant is non-monotonic so
// plain Newton is unsafe but a bracketed physical branch is known.
template <typename F>
inline double safeguardedRoot(F f, double lo, double hi, double scale)
{
  const double tol = 1e-12 * (scale > 0.0 ? scale : 1.0);
  const double flo = f(lo);
  const double fhi = f(hi);
  if (flo == 0.0)
  {
    return lo;
  }
  if (fhi == 0.0)
  {
    return hi;
  }
  if ((flo > 0.0) == (fhi > 0.0))
  {
    return 0.5 * (lo + hi);  // not bracketed (degenerate); best effort
  }
  double xl = flo < 0.0 ? lo : hi;  // side where f < 0
  double xh = flo < 0.0 ? hi : lo;
  double x = 0.5 * (lo + hi);
  double dxOld = std::fabs(hi - lo);
  double dx = dxOld;
  double fx = f(x);
  double df = centralDeriv(f, x, lo, hi);
  for (int i = 0; i < 100; ++i)
  {
    const bool newtonOutOfRange = ((x - xh) * df - fx) * ((x - xl) * df - fx) > 0.0;
    const bool newtonSlow = std::fabs(2.0 * fx) > std::fabs(dxOld * df);
    if (newtonOutOfRange || newtonSlow || df == 0.0)
    {
      dxOld = dx;
      dx = 0.5 * (xh - xl);
      x = xl + dx;
    }
    else
    {
      dxOld = dx;
      dx = fx / df;
      x = x - dx;
    }
    if (std::fabs(dx) < tol)
    {
      return x;
    }
    fx = f(x);
    df = centralDeriv(f, x, lo, hi);
    if (fx < 0.0)
    {
      xl = x;
    }
    else
    {
      xh = x;
    }
  }
  return x;
}

}  // namespace flox::detail
