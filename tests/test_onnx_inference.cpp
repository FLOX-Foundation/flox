/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include "flox/ml/onnx_inference.h"

using flox::ml::BudgetedInference;
using flox::ml::OnnxModel;
using flox::ml::OnnxSidecar;

namespace
{

// Fixture: y = 2*x0 + 3*x1 + 0.5 (tests/fixtures/linear2.onnx). The path is
// injected by CMake so the test works from any build directory.
#ifndef FLOX_TEST_FIXTURE_DIR
#define FLOX_TEST_FIXTURE_DIR "tests/fixtures"
#endif

std::string modelPath()
{
  return std::string(FLOX_TEST_FIXTURE_DIR) + "/linear2.onnx";
}

}  // namespace

TEST(OnnxModel, LoadsAndScoresLinearModel)
{
  OnnxModel model(modelPath());
  EXPECT_EQ(model.featureCount(), 2u);
  ASSERT_GE(model.outputCount(), 1u);

  const std::vector<double> f{1.0, 2.0};
  const auto out = model.run(f);
  ASSERT_FALSE(out.empty());
  EXPECT_NEAR(out[0], 2.0 * 1.0 + 3.0 * 2.0 + 0.5, 1e-5);

  // Second run reuses buffers; different inputs, correct output.
  const std::vector<double> g{-1.0, 4.0};
  const auto out2 = model.run(g);
  EXPECT_NEAR(out2[0], 2.0 * -1.0 + 3.0 * 4.0 + 0.5, 1e-5);
}

TEST(BudgetedInference, CountsOverrunsAndRecordsHistogram)
{
  OnnxModel model(modelPath());
  // 1ns budget: every run overruns.
  BudgetedInference tight(model, 1, BudgetedInference::OverrunPolicy::WARN);
  const std::vector<double> f{1.0, 1.0};
  for (int i = 0; i < 5; ++i)
  {
    const auto r = tight.run(f);
    EXPECT_TRUE(r.fresh);
  }
  EXPECT_EQ(tight.overruns(), 5u);
  EXPECT_EQ(tight.histogram().count(), 5u);

  // Generous budget: no overruns.
  BudgetedInference roomy(model, 1'000'000'000, BudgetedInference::OverrunPolicy::WARN);
  roomy.run(f);
  EXPECT_EQ(roomy.overruns(), 0u);
}

TEST(BudgetedInference, DropPolicyServesStaleAfterOverrun)
{
  OnnxModel model(modelPath());
  BudgetedInference inf(model, 1, BudgetedInference::OverrunPolicy::DROP);
  const std::vector<double> f{1.0, 1.0};

  const auto first = inf.run(f);
  EXPECT_TRUE(first.fresh);

  const auto second = inf.run(f);
  EXPECT_FALSE(second.fresh);  // dropped: model not run

  inf.reset();
  const auto third = inf.run(f);
  EXPECT_TRUE(third.fresh);
}

TEST(OnnxSidecar, DeliversPredictionsWithStalenessMetadata)
{
  OnnxSidecar sidecar(std::make_unique<OnnxModel>(modelPath()));

  const std::vector<double> f{2.0, 2.0};
  sidecar.submit(f, /*nowNs=*/12345);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  OnnxSidecar::Prediction p;
  while (std::chrono::steady_clock::now() < deadline)
  {
    p = sidecar.latest();
    if (p.seq > 0)
    {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_GT(p.seq, 0u);
  ASSERT_FALSE(p.values.empty());
  EXPECT_NEAR(p.values[0], 2.0 * 2.0 + 3.0 * 2.0 + 0.5, 1e-5);
  EXPECT_EQ(p.featureTsNs, 12345);
}

TEST(OnnxGraphNode, BatchColumnMatchesModelRowByRow)
{
  using flox::indicator::IndicatorGraph;

  IndicatorGraph graph;
  std::vector<flox::Bar> bars(50);
  std::vector<double> close(50);
  for (size_t i = 0; i < bars.size(); ++i)
  {
    close[i] = 100.0 + double(i);
    bars[i].open = flox::Price::fromDouble(close[i]);
    bars[i].high = flox::Price::fromDouble(close[i] + 1);
    bars[i].low = flox::Price::fromDouble(close[i] - 1);
    bars[i].close = flox::Price::fromDouble(close[i]);
    bars[i].volume = flox::Volume::fromDouble(1.0);
  }
  graph.setBars(0, bars);

  graph.addNode("sma_3", {},
                [](IndicatorGraph& g, flox::SymbolId sym)
                {
                  const auto& c = g.close(sym);
                  std::vector<double> out(c.size(), std::numeric_limits<double>::quiet_NaN());
                  for (size_t i = 2; i < c.size(); ++i)
                  {
                    out[i] = (c[i] + c[i - 1] + c[i - 2]) / 3.0;
                  }
                  return out;
                });
  graph.addNode("close_id", {},
                [](IndicatorGraph& g, flox::SymbolId sym)
                { return std::vector<double>(g.close(sym).begin(), g.close(sym).end()); });

  auto model = std::make_shared<OnnxModel>(modelPath());
  flox::ml::registerOnnxNode(graph, "alpha", {"close_id", "sma_3"}, model);

  const auto& col = graph.require(0, "alpha");
  ASSERT_EQ(col.size(), bars.size());

  // Warmup rows (sma_3 NaN) stay NaN in the model column.
  EXPECT_TRUE(std::isnan(col[0]));
  EXPECT_TRUE(std::isnan(col[1]));

  // Every valid row equals the model applied to that row's features.
  const auto& sma = graph.require(0, "sma_3");
  for (size_t t = 2; t < col.size(); ++t)
  {
    const double expected = 2.0 * close[t] + 3.0 * sma[t] + 0.5;
    EXPECT_NEAR(col[t], expected, 1e-4) << "row " << t;
  }
}
