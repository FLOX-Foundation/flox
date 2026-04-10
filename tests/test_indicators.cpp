#include "flox/indicator/adx.h"
#include "flox/indicator/atr.h"
#include "flox/indicator/bar_fields.h"
#include "flox/indicator/bollinger.h"
#include "flox/indicator/cci.h"
#include "flox/indicator/chop.h"
#include "flox/indicator/cvd.h"
#include "flox/indicator/dema.h"
#include "flox/indicator/ema.h"
#include "flox/indicator/indicator_pipeline.h"
#include "flox/indicator/kama.h"
#include "flox/indicator/macd.h"
#include "flox/indicator/obv.h"
#include "flox/indicator/rma.h"
#include "flox/indicator/rsi.h"
#include "flox/indicator/slope.h"
#include "flox/indicator/sma.h"
#include "flox/indicator/stochastic.h"
#include "flox/indicator/vwap.h"

#include <gtest/gtest.h>
#include <cmath>
#include <vector>

using namespace flox;
using namespace flox::indicator;

static std::vector<double> prices()
{
  return {44, 44.34, 44.09, 43.61, 44.33, 44.83, 45.10, 45.42, 45.84,
          46.08, 45.89, 46.03, 45.61, 46.28, 46.28, 46.00, 46.03, 46.41,
          46.22, 45.64};
}

TEST(SMA, BasicWindow)
{
  SMA sma(5);
  auto result = sma.compute(prices());
  ASSERT_EQ(result.size(), 20u);
  for (int i = 0; i < 4; ++i)
  {
    EXPECT_TRUE(std::isnan(result[i]));
  }
  EXPECT_NEAR(result[4], (44 + 44.34 + 44.09 + 43.61 + 44.33) / 5.0, 1e-10);
}

TEST(SMA, RollingSum)
{
  SMA sma(3);
  std::vector<double> input = {1, 2, 3, 4, 5};
  auto result = sma.compute(input);
  EXPECT_NEAR(result[2], 2.0, 1e-10);
  EXPECT_NEAR(result[3], 3.0, 1e-10);
  EXPECT_NEAR(result[4], 4.0, 1e-10);
}

TEST(EMA, SmaSeed)
{
  EMA ema(10);
  auto result = ema.compute(prices());
  ASSERT_EQ(result.size(), 20u);
  for (int i = 0; i < 9; ++i)
  {
    EXPECT_TRUE(std::isnan(result[i]));
  }
  // Seed = SMA of first 10 values
  double sum = 0;
  auto p = prices();
  for (int i = 0; i < 10; ++i)
  {
    sum += p[i];
  }
  EXPECT_NEAR(result[9], sum / 10.0, 1e-10);
}

TEST(EMA, ExponentialDecay)
{
  EMA ema(10);
  auto p = prices();
  auto result = ema.compute(p);
  double alpha = 2.0 / 11.0;
  double expected = result[9];
  for (size_t i = 10; i < p.size(); ++i)
  {
    expected = alpha * p[i] + (1.0 - alpha) * expected;
    EXPECT_NEAR(result[i], expected, 1e-10);
  }
}

TEST(EMA, Composition)
{
  // EMA of EMA -- verify compute(span<double>) works
  EMA ema1(5);
  EMA ema2(3);
  auto p = prices();
  auto first = ema1.compute(p);
  auto second = ema2.compute(std::span<const double>(first));
  // second should have NaN where first has NaN + 2 more warmup bars
  EXPECT_TRUE(std::isnan(second[0]));
  bool found = false;
  for (size_t i = 0; i < second.size(); ++i)
  {
    if (!std::isnan(second[i]))
    {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

TEST(EMA, WriteToBuffer)
{
  EMA ema(5);
  auto p = prices();
  std::vector<double> buf(p.size());
  ema.compute(p, buf);
  auto alloced = ema.compute(p);
  for (size_t i = 0; i < p.size(); ++i)
  {
    if (std::isnan(alloced[i]))
    {
      EXPECT_TRUE(std::isnan(buf[i]));
    }
    else
    {
      EXPECT_DOUBLE_EQ(buf[i], alloced[i]);
    }
  }
}

TEST(ATR, WilderSmoothing)
{
  // Construct bars with known H/L/C
  std::vector<double> h = {48.70, 48.72, 48.90, 48.87, 48.82, 49.05, 49.20, 49.35, 49.92, 50.19,
                           50.12, 49.66, 49.88, 50.19, 50.36, 50.57, 50.65, 50.43, 49.63, 50.33};
  std::vector<double> l = {47.79, 48.14, 48.39, 48.37, 48.24, 48.64, 48.94, 48.86, 49.50, 49.87,
                           49.20, 48.90, 49.43, 49.73, 49.26, 50.09, 50.30, 49.21, 48.98, 49.61};
  std::vector<double> c = {48.16, 48.61, 48.75, 48.63, 48.74, 49.03, 49.07, 49.32, 49.91, 50.13,
                           49.53, 49.50, 49.75, 50.03, 49.99, 50.23, 50.33, 49.24, 49.00, 50.29};

  ATR atr(14);
  auto result = atr.compute(h, l, c);
  ASSERT_EQ(result.size(), 20u);

  // TA-Lib: TR[0] = NaN (no prev close), ATR first valid at index period (14)
  for (int i = 0; i < 14; ++i)
  {
    EXPECT_TRUE(std::isnan(result[i]));
  }
  EXPECT_FALSE(std::isnan(result[14]));

  for (size_t i = 14; i < result.size(); ++i)
  {
    EXPECT_FALSE(std::isnan(result[i]));
    EXPECT_GT(result[i], 0.0);
  }
}

TEST(ATR, BarOverload)
{
  // Create bars and verify Bar overload matches raw span overload
  std::vector<Bar> bars(10);
  for (size_t i = 0; i < 10; ++i)
  {
    bars[i].open = Price::fromDouble(100.0 + i);
    bars[i].high = Price::fromDouble(101.0 + i);
    bars[i].low = Price::fromDouble(99.0 + i);
    bars[i].close = Price::fromDouble(100.5 + i);
  }

  ATR atr(5);
  auto fromBars = atr.compute(std::span<const Bar>(bars));

  auto h = indicator::high(std::span<const Bar>(bars));
  auto l = indicator::low(std::span<const Bar>(bars));
  auto c = indicator::close(std::span<const Bar>(bars));
  auto fromSpans = atr.compute(h, l, c);

  ASSERT_EQ(fromBars.size(), fromSpans.size());
  for (size_t i = 0; i < fromBars.size(); ++i)
  {
    if (std::isnan(fromBars[i]))
    {
      EXPECT_TRUE(std::isnan(fromSpans[i]));
    }
    else
    {
      EXPECT_DOUBLE_EQ(fromBars[i], fromSpans[i]);
    }
  }
}

TEST(RSI, BoundaryValues)
{
  // All gains -> RSI should be 100
  std::vector<double> allUp = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  RSI rsi(14);
  auto result = rsi.compute(allUp);
  for (size_t i = 15; i < result.size(); ++i)
  {
    EXPECT_NEAR(result[i], 100.0, 1e-10);
  }

  // All losses -> RSI should be 0
  std::vector<double> allDown = {16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
  auto result2 = rsi.compute(allDown);
  for (size_t i = 15; i < result2.size(); ++i)
  {
    EXPECT_NEAR(result2[i], 0.0, 1e-10);
  }
}

TEST(RSI, MiddleRange)
{
  RSI rsi(14);
  auto result = rsi.compute(prices());
  for (size_t i = 0; i < 14; ++i)
  {
    EXPECT_TRUE(std::isnan(result[i]));
  }
  EXPECT_FALSE(std::isnan(result[14]));
  for (size_t i = 14; i < result.size(); ++i)
  {
    EXPECT_GE(result[i], 0.0);
    EXPECT_LE(result[i], 100.0);
  }
}

TEST(MACD, ThreeOutputs)
{
  MACD macd(12, 26, 9);
  auto result = macd.compute(prices());
  ASSERT_EQ(result.line.size(), 20u);
  ASSERT_EQ(result.signal.size(), 20u);
  ASSERT_EQ(result.histogram.size(), 20u);
}

TEST(MACD, Composition)
{
  // MACD line values should be finite where signal is valid
  MACD macd(3, 5, 3);
  auto p = prices();
  auto result = macd.compute(p);

  bool hasValid = false;
  for (size_t i = 0; i < p.size(); ++i)
  {
    if (!std::isnan(result.line[i]))
    {
      hasValid = true;
      EXPECT_FALSE(std::isnan(result.signal[i]));
      EXPECT_FALSE(std::isnan(result.histogram[i]));
    }
  }
  EXPECT_TRUE(hasValid);
}

TEST(MACD, HistogramIsLineMinusSignal)
{
  MACD macd(3, 5, 3);
  auto result = macd.compute(prices());
  for (size_t i = 0; i < result.histogram.size(); ++i)
  {
    if (!std::isnan(result.histogram[i]))
    {
      EXPECT_NEAR(result.histogram[i], result.line[i] - result.signal[i], 1e-10);
    }
  }
}

TEST(MACD, WriteToBuffer)
{
  MACD macd(3, 5, 3);
  auto p = prices();
  std::vector<double> line(p.size()), signal(p.size()), hist(p.size());
  macd.compute(p, line, signal, hist);
  auto result = macd.compute(p);
  for (size_t i = 0; i < p.size(); ++i)
  {
    if (std::isnan(result.line[i]))
    {
      EXPECT_TRUE(std::isnan(line[i]));
    }
    else
    {
      EXPECT_DOUBLE_EQ(line[i], result.line[i]);
    }
  }
}

// Edge cases

TEST(SMA, EmptyInput)
{
  SMA sma(5);
  auto result = sma.compute(std::span<const double>{});
  EXPECT_TRUE(result.empty());
}

TEST(EMA, EmptyInput)
{
  EMA ema(5);
  auto result = ema.compute(std::span<const double>{});
  EXPECT_TRUE(result.empty());
}

TEST(ATR, EmptyInput)
{
  ATR atr(5);
  auto result = atr.compute(
      std::span<const double>{}, std::span<const double>{}, std::span<const double>{});
  EXPECT_TRUE(result.empty());
}

TEST(RSI, EmptyInput)
{
  RSI rsi(14);
  auto result = rsi.compute(std::span<const double>{});
  EXPECT_TRUE(result.empty());
}

TEST(MACD, EmptyInput)
{
  MACD macd(3, 5, 3);
  auto result = macd.compute(std::span<const double>{});
  EXPECT_TRUE(result.line.empty());
  EXPECT_TRUE(result.signal.empty());
  EXPECT_TRUE(result.histogram.empty());
}

TEST(SMA, InputShorterThanPeriod)
{
  SMA sma(10);
  std::vector<double> input = {1, 2, 3};
  auto result = sma.compute(input);
  ASSERT_EQ(result.size(), 3u);
  for (auto v : result)
  {
    EXPECT_TRUE(std::isnan(v));
  }
}

TEST(EMA, NaNInputSkipped)
{
  EMA ema(3);
  std::vector<double> input = {std::nan(""), std::nan(""), 1, 2, 3, 4, 5};
  auto result = ema.compute(input);
  // First 2 NaN in input, then EMA seeds at index 4 (3 valid values: 1,2,3)
  EXPECT_TRUE(std::isnan(result[0]));
  EXPECT_TRUE(std::isnan(result[1]));
  EXPECT_TRUE(std::isnan(result[2]));
  EXPECT_TRUE(std::isnan(result[3]));
  EXPECT_FALSE(std::isnan(result[4]));
  EXPECT_NEAR(result[4], 2.0, 1e-10);  // SMA(1,2,3) = 2.0
}

TEST(RSI, NaNInputHandled)
{
  RSI rsi(3);
  std::vector<double> input = {std::nan(""), 1, 2, 3, 4, 5, 4, 3, 2, 1};
  auto result = rsi.compute(input);
  // Should not crash, some values should be valid
  bool hasValid = false;
  for (auto v : result)
  {
    if (!std::isnan(v))
    {
      hasValid = true;
      EXPECT_GE(v, 0.0);
      EXPECT_LE(v, 100.0);
    }
  }
  EXPECT_TRUE(hasValid);
}

TEST(BarFields, ExtractClose)
{
  std::vector<Bar> bars(5);
  for (size_t i = 0; i < 5; ++i)
  {
    bars[i].close = Price::fromDouble(100.0 + i);
  }
  auto closes = indicator::close(std::span<const Bar>(bars));
  ASSERT_EQ(closes.size(), 5u);
  for (size_t i = 0; i < 5; ++i)
  {
    EXPECT_DOUBLE_EQ(closes[i], 100.0 + i);
  }
}

TEST(BarFields, InPlaceExtract)
{
  std::vector<Bar> bars(3);
  bars[0].high = Price::fromDouble(10.0);
  bars[1].high = Price::fromDouble(20.0);
  bars[2].high = Price::fromDouble(30.0);
  std::vector<double> buf(3);
  indicator::high(std::span<const Bar>(bars), buf);
  EXPECT_DOUBLE_EQ(buf[0], 10.0);
  EXPECT_DOUBLE_EQ(buf[1], 20.0);
  EXPECT_DOUBLE_EQ(buf[2], 30.0);
}

// Slope

TEST(Slope, Basic)
{
  Slope slope(1);
  std::vector<double> input = {10, 12, 11, 15, 13};
  auto result = slope.compute(input);
  EXPECT_TRUE(std::isnan(result[0]));
  EXPECT_NEAR(result[1], 2.0, 1e-10);
  EXPECT_NEAR(result[2], -1.0, 1e-10);
  EXPECT_NEAR(result[3], 4.0, 1e-10);
  EXPECT_NEAR(result[4], -2.0, 1e-10);
}

TEST(Slope, LongerPeriod)
{
  Slope slope(3);
  std::vector<double> input = {10, 12, 11, 15, 13};
  auto result = slope.compute(input);
  EXPECT_TRUE(std::isnan(result[0]));
  EXPECT_TRUE(std::isnan(result[1]));
  EXPECT_TRUE(std::isnan(result[2]));
  EXPECT_NEAR(result[3], (15 - 10) / 3.0, 1e-10);
  EXPECT_NEAR(result[4], (13 - 12) / 3.0, 1e-10);
}

// KAMA

TEST(KAMA, SmaSeed)
{
  KAMA kama(10);
  auto p = prices();
  auto result = kama.compute(p);
  ASSERT_EQ(result.size(), p.size());
  // TA-Lib: KAMA first valid at period (10), seeded from close[period-1]
  for (int i = 0; i < 10; ++i)
  {
    EXPECT_TRUE(std::isnan(result[i]));
  }
  EXPECT_FALSE(std::isnan(result[10]));
  for (size_t i = 10; i < p.size(); ++i)
  {
    EXPECT_FALSE(std::isnan(result[i]));
  }
}

// ADX

TEST(ADX, ThreeOutputs)
{
  std::vector<double> h = {48.70, 48.72, 48.90, 48.87, 48.82, 49.05, 49.20, 49.35, 49.92, 50.19,
                           50.12, 49.66, 49.88, 50.19, 50.36, 50.57, 50.65, 50.43, 49.63, 50.33};
  std::vector<double> l = {47.79, 48.14, 48.39, 48.37, 48.24, 48.64, 48.94, 48.86, 49.50, 49.87,
                           49.20, 48.90, 49.43, 49.73, 49.26, 50.09, 50.30, 49.21, 48.98, 49.61};
  std::vector<double> c = {48.16, 48.61, 48.75, 48.63, 48.74, 49.03, 49.07, 49.32, 49.91, 50.13,
                           49.53, 49.50, 49.75, 50.03, 49.99, 50.23, 50.33, 49.24, 49.00, 50.29};

  // ADX(14) needs 2*14+1 = 29 bars minimum. We have 20 -- not enough for ADX.
  // Use a larger dataset.
  std::vector<double> h2, l2, c2;
  for (int rep = 0; rep < 3; ++rep)
  {
    h2.insert(h2.end(), h.begin(), h.end());
    l2.insert(l2.end(), l.begin(), l.end());
    c2.insert(c2.end(), c.begin(), c.end());
  }

  ADX adx(14);
  auto result = adx.compute(h2, l2, c2);
  ASSERT_EQ(result.adx.size(), 60u);
  ASSERT_EQ(result.plus_di.size(), 60u);

  // First valid ADX at index 2*14-1 = 27
  for (size_t i = 0; i < 27; ++i)
  {
    EXPECT_TRUE(std::isnan(result.adx[i]));
  }
  EXPECT_FALSE(std::isnan(result.adx[27]));
  EXPECT_GE(result.adx[27], 0.0);
}

// CHOP

TEST(CHOP, Range)
{
  std::vector<double> h = {48.70, 48.72, 48.90, 48.87, 48.82, 49.05, 49.20, 49.35, 49.92, 50.19,
                           50.12, 49.66, 49.88, 50.19, 50.36, 50.57, 50.65, 50.43, 49.63, 50.33};
  std::vector<double> l = {47.79, 48.14, 48.39, 48.37, 48.24, 48.64, 48.94, 48.86, 49.50, 49.87,
                           49.20, 48.90, 49.43, 49.73, 49.26, 50.09, 50.30, 49.21, 48.98, 49.61};
  std::vector<double> c = {48.16, 48.61, 48.75, 48.63, 48.74, 49.03, 49.07, 49.32, 49.91, 50.13,
                           49.53, 49.50, 49.75, 50.03, 49.99, 50.23, 50.33, 49.24, 49.00, 50.29};

  CHOP chop(14);
  auto result = chop.compute(h, l, c);
  ASSERT_EQ(result.size(), 20u);

  for (size_t i = 0; i < result.size(); ++i)
  {
    if (!std::isnan(result[i]))
    {
      // CHOP should be between 0 and 100
      EXPECT_GE(result[i], 0.0);
      EXPECT_LE(result[i], 100.0);
    }
  }
}

// OBV

TEST(OBV, CumulativeVolume)
{
  std::vector<double> close = {10, 11, 10.5, 12, 11.5};
  std::vector<double> vol = {100, 200, 150, 300, 250};

  OBV obv;
  auto result = obv.compute(close, vol);
  ASSERT_EQ(result.size(), 5u);
  // TA-Lib: OBV[0] = volume[0]
  EXPECT_DOUBLE_EQ(result[0], 100.0);        // volume[0]
  EXPECT_DOUBLE_EQ(result[1], 100.0 + 200);  // up: +200
  EXPECT_DOUBLE_EQ(result[2], 300.0 - 150);  // down: -150
  EXPECT_DOUBLE_EQ(result[3], 150.0 + 300);  // up: +300
  EXPECT_DOUBLE_EQ(result[4], 450.0 - 250);  // down: -250
}

TEST(OBV, EmptyInput)
{
  OBV obv;
  auto result = obv.compute(std::span<const double>{}, std::span<const double>{});
  EXPECT_TRUE(result.empty());
}

// Bollinger

TEST(Bollinger, ThreeBands)
{
  Bollinger bb(5, 2.0);
  auto p = prices();
  auto result = bb.compute(p);
  ASSERT_EQ(result.upper.size(), p.size());
  ASSERT_EQ(result.middle.size(), p.size());
  ASSERT_EQ(result.lower.size(), p.size());

  for (size_t i = 4; i < p.size(); ++i)
  {
    EXPECT_FALSE(std::isnan(result.middle[i]));
    EXPECT_FALSE(std::isnan(result.upper[i]));
    EXPECT_FALSE(std::isnan(result.lower[i]));
    EXPECT_GT(result.upper[i], result.middle[i]);
    EXPECT_LT(result.lower[i], result.middle[i]);
  }
}

TEST(Bollinger, MiddleIsSma)
{
  Bollinger bb(5, 2.0);
  SMA sma(5);
  auto p = prices();
  auto bbResult = bb.compute(p);
  auto smaResult = sma.compute(p);

  for (size_t i = 0; i < p.size(); ++i)
  {
    if (!std::isnan(bbResult.middle[i]))
    {
      EXPECT_NEAR(bbResult.middle[i], smaResult[i], 1e-10);
    }
  }
}

TEST(RMA, WilderSmoothing)
{
  RMA rma(5);
  auto p = prices();
  auto result = rma.compute(p);
  ASSERT_EQ(result.size(), p.size());
  for (int i = 0; i < 4; ++i)
  {
    EXPECT_TRUE(std::isnan(result[i]));
  }
  EXPECT_FALSE(std::isnan(result[4]));
  double sum = 0;
  for (int i = 0; i < 5; ++i)
  {
    sum += p[i];
  }
  EXPECT_NEAR(result[4], sum / 5.0, 1e-10);
}

TEST(DEMA, HasOutput)
{
  DEMA dema(5);
  auto result = dema.compute(prices());
  ASSERT_EQ(result.size(), 20u);
  bool found = false;
  for (auto v : result)
  {
    if (!std::isnan(v))
    {
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

TEST(TEMA, HasOutput)
{
  TEMA tema(5);
  auto result = tema.compute(prices());
  ASSERT_EQ(result.size(), 20u);
  bool found = false;
  for (auto v : result)
  {
    if (!std::isnan(v))
    {
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

TEST(Stochastic, KAndD)
{
  std::vector<double> h = {48.70, 48.72, 48.90, 48.87, 48.82, 49.05, 49.20, 49.35, 49.92, 50.19,
                           50.12, 49.66, 49.88, 50.19, 50.36, 50.57, 50.65, 50.43, 49.63, 50.33};
  std::vector<double> l = {47.79, 48.14, 48.39, 48.37, 48.24, 48.64, 48.94, 48.86, 49.50, 49.87,
                           49.20, 48.90, 49.43, 49.73, 49.26, 50.09, 50.30, 49.21, 48.98, 49.61};
  std::vector<double> c = {48.16, 48.61, 48.75, 48.63, 48.74, 49.03, 49.07, 49.32, 49.91, 50.13,
                           49.53, 49.50, 49.75, 50.03, 49.99, 50.23, 50.33, 49.24, 49.00, 50.29};
  Stochastic stoch(5, 3);
  auto result = stoch.compute(h, l, c);
  ASSERT_EQ(result.k.size(), 20u);
  for (size_t i = 6; i < 20; ++i)
  {
    EXPECT_FALSE(std::isnan(result.k[i]));
    EXPECT_GE(result.k[i], 0.0);
    EXPECT_LE(result.k[i], 100.0);
  }
}

TEST(CCI, Range)
{
  std::vector<double> h = {48.70, 48.72, 48.90, 48.87, 48.82, 49.05, 49.20, 49.35, 49.92, 50.19,
                           50.12, 49.66, 49.88, 50.19, 50.36, 50.57, 50.65, 50.43, 49.63, 50.33};
  std::vector<double> l = {47.79, 48.14, 48.39, 48.37, 48.24, 48.64, 48.94, 48.86, 49.50, 49.87,
                           49.20, 48.90, 49.43, 49.73, 49.26, 50.09, 50.30, 49.21, 48.98, 49.61};
  std::vector<double> c = {48.16, 48.61, 48.75, 48.63, 48.74, 49.03, 49.07, 49.32, 49.91, 50.13,
                           49.53, 49.50, 49.75, 50.03, 49.99, 50.23, 50.33, 49.24, 49.00, 50.29};
  CCI cci(14);
  auto result = cci.compute(h, l, c);
  ASSERT_EQ(result.size(), 20u);
  for (size_t i = 13; i < 20; ++i)
  {
    EXPECT_FALSE(std::isnan(result[i]));
  }
}

TEST(VWAP, Rolling)
{
  std::vector<double> close = {100, 101, 102, 103, 104};
  std::vector<double> volume = {1000, 2000, 1500, 3000, 2500};
  VWAP vwap(3);
  auto result = vwap.compute(close, volume);
  ASSERT_EQ(result.size(), 5u);
  EXPECT_TRUE(std::isnan(result[0]));
  EXPECT_FALSE(std::isnan(result[2]));
  double expected = (100 * 1000.0 + 101 * 2000.0 + 102 * 1500.0) / (1000.0 + 2000.0 + 1500.0);
  EXPECT_NEAR(result[2], expected, 1e-10);
}

TEST(CVD, Cumulative)
{
  std::vector<double> o = {100, 100, 100, 100, 100};
  std::vector<double> h = {102, 102, 102, 102, 102};
  std::vector<double> l = {98, 98, 98, 98, 98};
  std::vector<double> c = {101, 99, 101, 99, 101};
  std::vector<double> v = {100, 100, 100, 100, 100};
  CVD cvd;
  auto result = cvd.compute(o, h, l, c, v);
  ASSERT_EQ(result.size(), 5u);
  EXPECT_GT(result[0], 0);
  EXPECT_LT(result[1], result[0]);
}

TEST(IndicatorGraph, DependencyResolution)
{
  IndicatorGraph g;
  std::vector<Bar> bars(20);
  for (size_t i = 0; i < 20; ++i)
  {
    bars[i].close = Price::fromDouble(100.0 + i);
    bars[i].high = Price::fromDouble(101.0 + i);
    bars[i].low = Price::fromDouble(99.0 + i);
  }
  g.setBars(0, bars);
  g.addNode("ema5", {}, [](IndicatorGraph& g, SymbolId sym)
            { return EMA(5).compute(g.close(sym)); });
  g.addNode("slope1", {"ema5"}, [](IndicatorGraph& g, SymbolId sym)
            { return Slope(1).compute(*g.get(sym, "ema5")); });
  auto& slope = g.require(0, "slope1");
  ASSERT_EQ(slope.size(), 20u);
  auto* ema = g.get(0, "ema5");
  ASSERT_NE(ema, nullptr);
}

TEST(IndicatorGraph, CircularDependencyThrows)
{
  IndicatorGraph g;
  std::vector<Bar> bars(10);
  for (auto& b : bars)
  {
    b.close = Price::fromDouble(100.0);
  }
  g.setBars(0, bars);
  g.addNode("a", {"b"}, [](IndicatorGraph&, SymbolId)
            { return std::vector<double>{}; });
  g.addNode("b", {"a"}, [](IndicatorGraph&, SymbolId)
            { return std::vector<double>{}; });
  EXPECT_THROW(g.require(0, "a"), std::logic_error);
}

TEST(IndicatorGraph, CacheInvalidation)
{
  IndicatorGraph g;
  std::vector<Bar> bars(10);
  for (size_t i = 0; i < 10; ++i)
  {
    bars[i].close = Price::fromDouble(100.0 + i);
  }
  g.setBars(0, bars);
  g.addNode("ema3", {}, [](IndicatorGraph& g, SymbolId sym)
            { return EMA(3).compute(g.close(sym)); });
  auto& r1 = g.require(0, "ema3");
  double first = r1[9];
  bars[9].close = Price::fromDouble(200.0);
  g.setBars(0, bars);
  auto& r2 = g.require(0, "ema3");
  EXPECT_NE(r2[9], first);
}
