#include <gtest/gtest.h>

#include "flox/pricing/svi.h"

#include <cmath>
#include <vector>

using namespace flox::pricing;

namespace
{
// Sample total variance from a known slice at a spread of log-moneyness points.
void sampleSlice(const SVIParams& truth, std::vector<double>& k, std::vector<double>& w,
                 int n = 11, double kLo = -0.5, double kHi = 0.5)
{
  k.clear();
  w.clear();
  for (int i = 0; i < n; ++i)
  {
    const double kk = kLo + (kHi - kLo) * (static_cast<double>(i) / (n - 1));
    k.push_back(kk);
    w.push_back(truth.totalVariance(kk));
  }
}
}  // namespace

// Calibrating to points sampled from a known arbitrage-free slice must recover a
// fit that reproduces the same total-variance curve.
TEST(SVISurfaceTest, RecoversKnownSlice)
{
  SVIParams truth{0.04, 0.10, -0.30, 0.0, 0.20};
  ASSERT_TRUE(isButterflyFree(truth));

  std::vector<double> k, w;
  sampleSlice(truth, k, w);
  const SVIParams fit = calibrateSVI(k, w);

  for (size_t i = 0; i < k.size(); ++i)
  {
    EXPECT_NEAR(fit.totalVariance(k[i]), truth.totalVariance(k[i]), 1e-4)
        << "k=" << k[i];
  }
  EXPECT_TRUE(isButterflyFree(fit));
}

// The butterfly check flags a slice whose steep wings imply a negative density.
TEST(SVISurfaceTest, ButterflyArbitrageDetected)
{
  SVIParams sane{0.04, 0.10, -0.30, 0.0, 0.20};
  EXPECT_TRUE(isButterflyFree(sane));

  // Large b with extreme skew bends the smile enough to violate convexity.
  SVIParams arb{0.01, 1.20, -0.99, 0.0, 0.02};
  EXPECT_FALSE(isButterflyFree(arb));
}

// Implied vol from the slice is the square root of total variance over time.
TEST(SVISurfaceTest, ImpliedVolMatchesTotalVariance)
{
  SVIParams p{0.04, 0.10, -0.30, 0.0, 0.20};
  const double t = 0.5;
  const double k = 0.1;
  EXPECT_NEAR(p.impliedVol(k, t), std::sqrt(p.totalVariance(k) / t), 1e-12);
}

// Across two stacked slices the surface interpolates in total variance: at an
// intermediate expiry the vol sits between the two, and total variance is
// monotone in time (calendar-free).
TEST(SVISurfaceTest, SurfaceInterpolatesBetweenSlices)
{
  VolSurface s;
  s.addSlice(0.25, SVIParams{0.03, 0.08, -0.2, 0.0, 0.2});
  s.addSlice(1.00, SVIParams{0.06, 0.10, -0.2, 0.0, 0.2});

  const double k = 0.0;
  const double wLo = s.totalVariance(k, 0.25);
  const double wHi = s.totalVariance(k, 1.00);
  const double wMid = s.totalVariance(k, 0.625);  // midpoint in t
  EXPECT_GT(wMid, wLo);
  EXPECT_LT(wMid, wHi);
  EXPECT_NEAR(wMid, 0.5 * (wLo + wHi), 1e-9);  // linear in variance at the midpoint

  EXPECT_TRUE(s.isCalendarFree());
  EXPECT_GT(s.impliedVol(k, 0.625), 0.0);
}

// A surface whose later expiry has LOWER total variance than an earlier one is
// calendar-arbitrageable.
TEST(SVISurfaceTest, CalendarArbitrageDetected)
{
  VolSurface s;
  s.addSlice(0.25, SVIParams{0.06, 0.08, -0.2, 0.0, 0.2});  // high variance, short expiry
  s.addSlice(1.00, SVIParams{0.03, 0.08, -0.2, 0.0, 0.2});  // lower variance, long expiry
  EXPECT_FALSE(s.isCalendarFree());
}

// The point-in-time build must ignore any quote stamped after the as-of date —
// no lookahead. A whole expiry that only has future quotes must not appear.
TEST(SVISurfaceTest, AsOfDateExcludesFutureQuotes)
{
  SVIParams shape{0.04, 0.10, -0.30, 0.0, 0.20};
  std::vector<DatedVolQuote> quotes;

  // Expiry A (t=0.25): quotes stamped at ts=1000 (past).
  // Expiry B (t=1.00): quotes stamped at ts=5000 (future relative to asof=2000).
  for (int i = 0; i < 7; ++i)
  {
    const double k = -0.4 + 0.8 * (i / 6.0);
    const double ivA = std::sqrt(shape.totalVariance(k) / 0.25);
    const double ivB = std::sqrt(shape.totalVariance(k) / 1.00);
    quotes.push_back({1000, 0.25, k, ivA});
    quotes.push_back({5000, 1.00, k, ivB});
  }

  const VolSurface early = buildSurfaceAsOf(quotes, /*asofNs=*/2000);
  EXPECT_EQ(early.sliceCount(), 1u);  // only expiry A survived
  EXPECT_NEAR(early.slices().front().t, 0.25, 1e-9);

  const VolSurface late = buildSurfaceAsOf(quotes, /*asofNs=*/9000);
  EXPECT_EQ(late.sliceCount(), 2u);  // both expiries now visible
}

// A thin expiry (fewer than 5 quotes) cannot identify the 5 SVI parameters and
// is dropped rather than producing a garbage slice.
TEST(SVISurfaceTest, ThinExpiryDropped)
{
  std::vector<DatedVolQuote> quotes;
  for (int i = 0; i < 3; ++i)
  {
    quotes.push_back({100, 0.5, -0.1 + 0.1 * i, 0.6});
  }
  const VolSurface s = buildSurfaceAsOf(quotes, 1000);
  EXPECT_EQ(s.sliceCount(), 0u);
}
