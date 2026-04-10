#pragma once

#include "flox/aggregator/bar.h"

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

struct AdxResult
{
  std::vector<double> adx;
  std::vector<double> plus_di;
  std::vector<double> minus_di;
};

class ADX
{
 public:
  explicit ADX(size_t period) noexcept : _period(period) {}

  // Matches TA-Lib ADX/PLUS_DI/MINUS_DI exactly.
  AdxResult compute(std::span<const double> high, std::span<const double> low,
                    std::span<const double> close) const
  {
    const size_t n = high.size();
    const size_t p = _period;
    const double dp = static_cast<double>(p);
    AdxResult result;
    result.adx.resize(n, std::nan(""));
    result.plus_di.resize(n, std::nan(""));
    result.minus_di.resize(n, std::nan(""));

    if (n < 2 * p + 1)
    {
      return result;
    }

    std::vector<double> posDm(n, 0.0);
    std::vector<double> negDm(n, 0.0);
    std::vector<double> tr(n, 0.0);
    for (size_t i = 1; i < n; ++i)
    {
      double up = high[i] - high[i - 1];
      double dn = low[i - 1] - low[i];
      if (up > dn && up > 0)
      {
        posDm[i] = up;
      }
      if (dn > up && dn > 0)
      {
        negDm[i] = dn;
      }
      tr[i] = std::max({high[i] - low[i], std::abs(high[i] - close[i - 1]),
                        std::abs(low[i] - close[i - 1])});
    }

    double sPdm = 0, sNdm = 0, sTr = 0;
    for (size_t i = 1; i < p; ++i)
    {
      sPdm += posDm[i];
      sNdm += negDm[i];
      sTr += tr[i];
    }

    std::vector<double> dx(n, std::nan(""));
    for (size_t i = p; i < n; ++i)
    {
      sPdm = sPdm - sPdm / dp + posDm[i];
      sNdm = sNdm - sNdm / dp + negDm[i];
      sTr = sTr - sTr / dp + tr[i];

      if (sTr > 0)
      {
        double pdi = 100.0 * sPdm / sTr;
        double ndi = 100.0 * sNdm / sTr;
        result.plus_di[i] = pdi;
        result.minus_di[i] = ndi;
        double s = pdi + ndi;
        dx[i] = s > 0 ? 100.0 * std::abs(pdi - ndi) / s : 0.0;
      }
    }

    double dxSum = 0.0;
    for (size_t i = p; i < 2 * p; ++i)
    {
      dxSum += std::isnan(dx[i]) ? 0.0 : dx[i];
    }
    size_t adxStart = 2 * p - 1;
    result.adx[adxStart] = dxSum / dp;

    for (size_t i = adxStart + 1; i < n; ++i)
    {
      double v = std::isnan(dx[i]) ? 0.0 : dx[i];
      result.adx[i] = (result.adx[i - 1] * (dp - 1.0) + v) / dp;
    }

    return result;
  }

  AdxResult compute(std::span<const Bar> bars) const
  {
    const size_t n = bars.size();
    std::vector<double> h(n), l(n), c(n);
    for (size_t i = 0; i < n; ++i)
    {
      h[i] = bars[i].high.toDouble();
      l[i] = bars[i].low.toDouble();
      c[i] = bars[i].close.toDouble();
    }
    return compute(h, l, c);
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace flox::indicator

#include "flox/indicator/indicator.h"
static_assert(flox::indicator::BarIndicator<flox::indicator::ADX>);
