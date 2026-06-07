/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include "flox/backtest/ntoken_curve.h"
#include "flox/util/int/i256.h"
#include "flox/util/int/u256.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace flox
{

// Balancer weighted-math primitives, exact. LogExpMath (ln / exp / pow) is a
// direct transcription of the contract's signed fixed-point library, so the
// power x^y = exp(y * ln(x)) rounds the way the contract does. FixedPoint wraps
// it with Balancer's up / down rounding and the powUp error margin.
namespace weighted_detail
{

inline i256 ONE18() { return i256(u256::pow10(18), false); }
inline i256 ONE20() { return i256(u256::pow10(20), false); }
inline i256 ONE36() { return i256(u256::pow10(36), false); }

// e^x for x in 18-decimal fixed point, signed. Transcribed from LogExpMath.exp.
inline i256 expFixed(i256 x)
{
  const i256 one18 = ONE18(), one20 = ONE20();
  if (x < i256(0))
  {
    return (one18 * one18) / expFixed(-x);
  }
  const i256 x0 = i256::fromDec("128000000000000000000");
  const i256 a0 = i256::fromDec("38877084059945950922200000000000000000000000000000000000");
  const i256 x1 = i256::fromDec("64000000000000000000");
  const i256 a1 = i256::fromDec("6235149080811616882910000000");
  i256 firstAN(1);
  if (x >= x0)
  {
    x = x - x0;
    firstAN = a0;
  }
  else if (x >= x1)
  {
    x = x - x1;
    firstAN = a1;
  }
  x = x * i256(100);
  i256 product = one20;
  const char* xs[] = {"3200000000000000000000", "1600000000000000000000", "800000000000000000000",
                      "400000000000000000000", "200000000000000000000", "100000000000000000000",
                      "50000000000000000000", "25000000000000000000"};
  const char* as[] = {"7896296018268069516100000000000000", "888611052050787263676000000",
                      "298095798704172827474000", "5459815003314423907810",
                      "738905609893065022723", "271828182845904523536",
                      "164872127070012814685", "128402541668774148407"};
  for (int k = 0; k < 8; ++k)
  {
    const i256 xi = i256::fromDec(xs[k]);
    if (x >= xi)
    {
      x = x - xi;
      product = (product * i256::fromDec(as[k])) / one20;
    }
  }
  i256 seriesSum = one20;
  i256 term = x;
  seriesSum = seriesSum + term;
  for (int n = 2; n <= 12; ++n)
  {
    term = ((term * x) / one20) / i256(n);
    seriesSum = seriesSum + term;
  }
  return (((product * seriesSum) / one20) * firstAN) / i256(100);
}

// ln(a) for a in 18-decimal fixed point, a > 0. Transcribed from LogExpMath._ln.
inline i256 lnFixed(i256 a)
{
  const i256 one18 = ONE18(), one20 = ONE20();
  if (a < one18)
  {
    return -lnFixed((one18 * one18) / a);
  }
  const i256 a0 = i256::fromDec("38877084059945950922200000000000000000000000000000000000");
  const i256 a1 = i256::fromDec("6235149080811616882910000000");
  const i256 x0 = i256::fromDec("128000000000000000000");
  const i256 x1 = i256::fromDec("64000000000000000000");
  i256 sum(0);
  if (a >= a0 * one18)
  {
    a = a / a0;
    sum = sum + x0;
  }
  if (a >= a1 * one18)
  {
    a = a / a1;
    sum = sum + x1;
  }
  sum = sum * i256(100);
  a = a * i256(100);
  const char* xs[] = {"3200000000000000000000", "1600000000000000000000", "800000000000000000000",
                      "400000000000000000000", "200000000000000000000", "100000000000000000000",
                      "50000000000000000000", "25000000000000000000", "12500000000000000000",
                      "6250000000000000000"};
  const char* as[] = {"7896296018268069516100000000000000", "888611052050787263676000000",
                      "298095798704172827474000", "5459815003314423907810",
                      "738905609893065022723", "271828182845904523536",
                      "164872127070012814685", "128402541668774148407",
                      "113314845306682631683", "106449445891785942956"};
  for (int k = 0; k < 10; ++k)
  {
    const i256 ai = i256::fromDec(as[k]);
    if (a >= ai)
    {
      a = (a * one20) / ai;
      sum = sum + i256::fromDec(xs[k]);
    }
  }
  i256 z = ((a - one20) * one20) / (a + one20);
  i256 zsq = (z * z) / one20;
  i256 num = z;
  i256 seriesSum = num;
  const int d[] = {3, 5, 7, 9, 11};
  for (int k = 0; k < 5; ++k)
  {
    num = (num * zsq) / one20;
    seriesSum = seriesSum + num / i256(d[k]);
  }
  seriesSum = seriesSum * i256(2);
  return (sum + seriesSum) / i256(100);
}

// ln(x) for x near 1, 36-decimal precision. Transcribed from LogExpMath._ln_36.
inline i256 ln36Fixed(i256 x)
{
  const i256 one18 = ONE18(), one36 = ONE36();
  x = x * one18;
  i256 z = ((x - one36) * one36) / (x + one36);
  i256 zsq = (z * z) / one36;
  i256 num = z;
  i256 seriesSum = num;
  const int d[] = {3, 5, 7, 9, 11, 13, 15};
  for (int k = 0; k < 7; ++k)
  {
    num = (num * zsq) / one36;
    seriesSum = seriesSum + num / i256(d[k]);
  }
  return seriesSum * i256(2);
}

// x^y = exp(y * ln(x)), 18-decimal fixed point. Transcribed from LogExpMath.pow.
inline u256 powFixed(const u256& xu, const u256& yu)
{
  if (yu.isZero())
  {
    return u256::pow10(18);
  }
  if (xu.isZero())
  {
    return u256(0);
  }
  const i256 one18 = ONE18();
  const i256 x(xu, false);
  const i256 y(yu, false);
  const i256 lower = one18 - i256(u256::pow10(17), false);
  const i256 upper = one18 + i256(u256::pow10(17), false);
  i256 logxTimesY;
  if (lower < x && x < upper)
  {
    const i256 l36 = ln36Fixed(x);
    logxTimesY = (l36 / one18) * y + ((l36 % one18) * y) / one18;
  }
  else
  {
    logxTimesY = lnFixed(x) * y;
  }
  logxTimesY = logxTimesY / one18;
  return expFixed(logxTimesY).magnitude();
}

// FixedPoint over uint256 (1e18). All Balancer rounding is explicit here.
inline u256 ONE() { return u256::pow10(18); }
inline u256 mulDown(const u256& a, const u256& b) { return a * b / ONE(); }
inline u256 mulUp(const u256& a, const u256& b)
{
  const u256 p = a * b;
  return p.isZero() ? u256(0) : (p - u256(1)) / ONE() + u256(1);
}
inline u256 divDown(const u256& a, const u256& b) { return a.isZero() ? u256(0) : a * ONE() / b; }
inline u256 divUp(const u256& a, const u256& b)
{
  return a.isZero() ? u256(0) : (a * ONE() - u256(1)) / b + u256(1);
}
inline u256 complement(const u256& x) { return x < ONE() ? ONE() - x : u256(0); }

inline u256 powUp(const u256& x, const u256& y)
{
  if (y == ONE())
  {
    return x;
  }
  if (y == u256(2) * ONE())
  {
    return mulUp(x, x);
  }
  if (y == u256(4) * ONE())
  {
    const u256 sq = mulUp(x, x);
    return mulUp(sq, sq);
  }
  const u256 raw = powFixed(x, y);
  const u256 maxError = mulUp(raw, u256(10000)) + u256(1);
  return raw + maxError;
}

}  // namespace weighted_detail

// Balancer weighted pool, n assets, exact in integer math. The swap is
// balanceOut * (1 - (balanceIn / (balanceIn + amountInAfterFee))^(weightIn /
// weightOut)), with the power through Balancer's LogExpMath, so it reproduces a
// weighted pool's quote to the wei. Equal weights reduce to constant-product.
//
// Amounts are native wei; scalingFactors carry each token to the 1e18 space the
// math works in (1e18 for an 18-decimal token). weights are normalized to 1e18
// and sum to 1e18; swapFee is the pool fee in 1e18.
class WeightedCurve : public INTokenCurve
{
 public:
  WeightedCurve(std::vector<u256> balances, std::vector<u256> scalingFactors,
                std::vector<u256> weights, u256 swapFee)
      : _b(std::move(balances)),
        _sf(std::move(scalingFactors)),
        _w(std::move(weights)),
        _fee(swapFee)
  {
  }

  const std::vector<u256>& weights() const { return _w; }
  u256 swapFee() const { return _fee; }

  std::size_t tokenCount() const override { return _b.size(); }
  const std::vector<u256>& balances() const override { return _b; }

  u256 amountOut(std::size_t i, std::size_t j, const u256& dx) const override
  {
    using namespace weighted_detail;
    if (dx.isZero())
    {
      return u256(0);
    }
    const u256 bIn = mulDown(_b[i], _sf[i]);
    const u256 bOut = mulDown(_b[j], _sf[j]);
    const u256 aIn = mulDown(dx, _sf[i]);
    const u256 aInAfterFee = aIn - mulUp(aIn, _fee);
    const u256 denom = bIn + aInAfterFee;
    const u256 base = divUp(bIn, denom);
    const u256 exponent = divDown(_w[i], _w[j]);
    const u256 power = powUp(base, exponent);
    const u256 outUp = mulDown(bOut, complement(power));
    return divDown(outUp, _sf[j]);  // downscale to native
  }

  u256 applySwap(std::size_t i, std::size_t j, const u256& dx) override
  {
    const u256 out = amountOut(i, j, dx);
    _b[i] = _b[i] + dx;
    _b[j] = _b[j] - out;
    return out;
  }

  std::unique_ptr<INTokenCurve> clone() const override
  {
    return std::make_unique<WeightedCurve>(*this);
  }

 private:
  std::vector<u256> _b;
  std::vector<u256> _sf;
  std::vector<u256> _w;
  u256 _fee;
};

}  // namespace flox
