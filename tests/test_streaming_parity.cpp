// tests/test_streaming_parity.cpp
//
// Tautology check: every indicator in registry.def exposes both compute() and
// update()/value(). After feeding the same input through both paths, the last
// value() must equal compute().back().
//
// This is by-construction parity (compute is reused inside update via mixin),
// so the test is really proving "the mixin wasn't accidentally short-circuited".

#include "flox/indicator/atr.h"
#include "flox/indicator/autocorrelation.h"
#include "flox/indicator/bollinger.h"
#include "flox/indicator/cci.h"
#include "flox/indicator/correlation.h"
#include "flox/indicator/dema.h"
#include "flox/indicator/ema.h"
#include "flox/indicator/kama.h"
#include "flox/indicator/kurtosis.h"
#include "flox/indicator/macd.h"
#include "flox/indicator/parkinson_vol.h"
#include "flox/indicator/rma.h"
#include "flox/indicator/rogers_satchell_vol.h"
#include "flox/indicator/rolling_zscore.h"
#include "flox/indicator/rsi.h"
#include "flox/indicator/shannon_entropy.h"
#include "flox/indicator/skewness.h"
#include "flox/indicator/slope.h"
#include "flox/indicator/sma.h"
#include "flox/indicator/stochastic.h"

#include <gtest/gtest.h>

#include <random>
#include <vector>

namespace
{

std::vector<double> randomSeries(size_t n, uint32_t seed, double lo = 100.0, double hi = 200.0)
{
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> dist(lo, hi);
  std::vector<double> v(n);
  for (size_t i = 0; i < n; ++i)
  {
    v[i] = dist(rng);
  }
  return v;
}

struct Hlc
{
  std::vector<double> high, low, close;
};

Hlc randomHlc(size_t n, uint32_t seed)
{
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> mid(100.0, 200.0);
  std::uniform_real_distribution<double> spread(0.5, 5.0);
  Hlc out;
  out.high.resize(n);
  out.low.resize(n);
  out.close.resize(n);
  for (size_t i = 0; i < n; ++i)
  {
    double m = mid(rng), s = spread(rng);
    out.high[i] = m + s;
    out.low[i] = m - s;
    std::uniform_real_distribution<double> cd(out.low[i], out.high[i]);
    out.close[i] = cd(rng);
  }
  return out;
}

double finiteOrNaN(double v) { return std::isfinite(v) ? v : std::nan(""); }

void expectClose(double a, double b, const char* msg)
{
  if (std::isnan(a) && std::isnan(b))
  {
    return;
  }
  ASSERT_TRUE(std::isfinite(a) && std::isfinite(b)) << msg << ": got " << a << ", " << b;
  EXPECT_NEAR(a, b, 1e-12) << msg;
}

}  // namespace

using namespace flox::indicator;

// ---- Single input ---------------------------------------------------------

template <typename T, typename... Args>
void checkSingleParity(const char* name, Args&&... args)
{
  auto input = randomSeries(50, 42);
  T batch(std::forward<Args>(args)...);
  auto out = batch.compute(input);

  T stream(std::forward<Args>(args)...);
  for (auto v : input)
  {
    stream.update(v);
  }

  expectClose(finiteOrNaN(stream.value()), finiteOrNaN(out.back()), name);
  EXPECT_EQ(stream.count(), input.size());
}

TEST(StreamingParity, EMA) { checkSingleParity<EMA>("EMA", 10); }
TEST(StreamingParity, SMA) { checkSingleParity<SMA>("SMA", 10); }
TEST(StreamingParity, RMA) { checkSingleParity<RMA>("RMA", 10); }
TEST(StreamingParity, RSI) { checkSingleParity<RSI>("RSI", 14); }
TEST(StreamingParity, DEMA) { checkSingleParity<DEMA>("DEMA", 10); }
TEST(StreamingParity, TEMA) { checkSingleParity<TEMA>("TEMA", 10); }
TEST(StreamingParity, KAMA) { checkSingleParity<KAMA>("KAMA", 10); }
TEST(StreamingParity, Slope) { checkSingleParity<Slope>("Slope", 10); }
TEST(StreamingParity, Skewness) { checkSingleParity<Skewness>("Skewness", 10); }
TEST(StreamingParity, Kurtosis) { checkSingleParity<Kurtosis>("Kurtosis", 10); }
TEST(StreamingParity, RollingZScore) { checkSingleParity<RollingZScore>("RollingZScore", 10); }
TEST(StreamingParity, ShannonEntropy) { checkSingleParity<ShannonEntropy>("ShannonEntropy", 10, 5); }
TEST(StreamingParity, AutoCorrelation) { checkSingleParity<AutoCorrelation>("AutoCorrelation", 10, 1); }

// ---- Bar input -----------------------------------------------------------

template <typename T, typename... Args>
void checkBarParity(const char* name, Args&&... args)
{
  auto bars = randomHlc(50, 42);
  T batch(std::forward<Args>(args)...);
  auto out = batch.compute(std::span<const double>(bars.high), std::span<const double>(bars.low),
                           std::span<const double>(bars.close));

  T stream(std::forward<Args>(args)...);
  for (size_t i = 0; i < bars.high.size(); ++i)
  {
    stream.update(bars.high[i], bars.low[i], bars.close[i]);
  }

  expectClose(finiteOrNaN(stream.value()), finiteOrNaN(out.back()), name);
  EXPECT_EQ(stream.count(), bars.high.size());
}

TEST(StreamingParity, ATR) { checkBarParity<ATR>("ATR", 14); }
TEST(StreamingParity, CCI) { checkBarParity<CCI>("CCI", 20); }

// Stochastic (multi-output bar)
TEST(StreamingParity, Stochastic)
{
  auto bars = randomHlc(50, 42);
  Stochastic s(14, 3);
  auto r = s.compute(std::span<const double>(bars.high), std::span<const double>(bars.low),
                     std::span<const double>(bars.close));
  Stochastic stream(14, 3);
  for (size_t i = 0; i < bars.high.size(); ++i)
  {
    stream.update(bars.high[i], bars.low[i], bars.close[i]);
  }
  expectClose(finiteOrNaN(stream.kValue()), finiteOrNaN(r.k.back()), "Stochastic.k");
  expectClose(finiteOrNaN(stream.dValue()), finiteOrNaN(r.d.back()), "Stochastic.d");
}

// ---- High/Low only -------------------------------------------------------

TEST(StreamingParity, ParkinsonVol)
{
  auto bars = randomHlc(50, 42);
  ParkinsonVol p(14);
  auto out = p.compute(std::span<const double>(bars.high), std::span<const double>(bars.low));
  ParkinsonVol stream(14);
  for (size_t i = 0; i < bars.high.size(); ++i)
  {
    stream.update(bars.high[i], bars.low[i]);
  }
  expectClose(finiteOrNaN(stream.value()), finiteOrNaN(out.back()), "ParkinsonVol");
}

// ---- OHLC ----------------------------------------------------------------

TEST(StreamingParity, RogersSatchellVol)
{
  auto bars = randomHlc(50, 42);
  std::vector<double> open(bars.high.size());
  for (size_t i = 0; i < open.size(); ++i)
  {
    open[i] = (bars.high[i] + bars.low[i]) * 0.5;  // synthetic
  }
  RogersSatchellVol p(14);
  auto out = p.compute(std::span<const double>(open), std::span<const double>(bars.high),
                       std::span<const double>(bars.low), std::span<const double>(bars.close));
  RogersSatchellVol stream(14);
  for (size_t i = 0; i < bars.high.size(); ++i)
  {
    stream.update(open[i], bars.high[i], bars.low[i], bars.close[i]);
  }
  expectClose(finiteOrNaN(stream.value()), finiteOrNaN(out.back()), "RogersSatchellVol");
}

// ---- Pair ----------------------------------------------------------------

TEST(StreamingParity, Correlation)
{
  auto x = randomSeries(50, 42);
  auto y = randomSeries(50, 7);
  Correlation c(10);
  auto out = c.compute(std::span<const double>(x), std::span<const double>(y));
  Correlation stream(10);
  for (size_t i = 0; i < x.size(); ++i)
  {
    stream.update(x[i], y[i]);
  }
  expectClose(finiteOrNaN(stream.value()), finiteOrNaN(out.back()), "Correlation");
}

// ---- Multi-output single input -------------------------------------------

TEST(StreamingParity, MACD)
{
  auto input = randomSeries(50, 42);
  MACD m(5, 10, 4);
  auto r = m.compute(std::span<const double>(input));
  MACD stream(5, 10, 4);
  for (auto v : input)
  {
    stream.update(v);
  }
  expectClose(finiteOrNaN(stream.value()), finiteOrNaN(r.line.back()), "MACD.line");
  expectClose(finiteOrNaN(stream.signalValue()), finiteOrNaN(r.signal.back()), "MACD.signal");
  expectClose(finiteOrNaN(stream.histogramValue()), finiteOrNaN(r.histogram.back()), "MACD.hist");
}

TEST(StreamingParity, Bollinger)
{
  auto input = randomSeries(50, 42);
  Bollinger b(10, 2.0);
  auto r = b.compute(std::span<const double>(input));
  Bollinger stream(10, 2.0);
  for (auto v : input)
  {
    stream.update(v);
  }
  expectClose(finiteOrNaN(stream.middleValue()), finiteOrNaN(r.middle.back()), "Bollinger.middle");
  expectClose(finiteOrNaN(stream.upperValue()), finiteOrNaN(r.upper.back()), "Bollinger.upper");
  expectClose(finiteOrNaN(stream.lowerValue()), finiteOrNaN(r.lower.back()), "Bollinger.lower");
}
