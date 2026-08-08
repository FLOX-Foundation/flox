/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

// ONNX Runtime inference for the indicator graph -- the last mile of the
// alpha pipeline. The feature layer (streaming indicators, dataset export)
// and the training loop already exist; this header executes the trained
// model on the same features in three places with one code path:
//   - batch: a graph node evaluated row by row over the bars (backtest and
//     replay parity for free -- it is the same node);
//   - sync in-path: run() with a latency budget, preallocated tensors, a
//     single-threaded session -- no allocation, no thread pool on the hot
//     path;
//   - async sidecar: OnnxSidecar runs the model next to the hot path;
//     the strategy reads the latest prediction with an explicit staleness
//     age instead of waiting.
//
// ONNX Runtime and not a training framework: one runtime serves models from
// sklearn, torch, xgboost, keras -- whatever the researcher used.

#include "flox/indicator/indicator_pipeline.h"
#include "flox/util/performance/latency_contour.h"
#include "flox/util/performance/latency_histogram.h"

#include <onnxruntime_cxx_api.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace flox::ml
{

// A loaded ONNX model with preallocated input/output tensors. run() performs
// zero heap allocations. One instance is single-threaded by contract (the
// session itself is configured intra-op=1: the hot path must never wake a
// thread pool); use one instance per thread, or the sidecar.
class OnnxModel
{
 public:
  explicit OnnxModel(const std::string& path)
      : _env(ORT_LOGGING_LEVEL_ERROR, "flox"),
        _session(nullptr),
        _memInfo(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
  {
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(1);
    opts.SetInterOpNumThreads(1);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    _session = Ort::Session(_env, path.c_str(), opts);

    Ort::AllocatorWithDefaultOptions alloc;
    _inputName = _session.GetInputNameAllocated(0, alloc).get();
    _outputName = _session.GetOutputNameAllocated(0, alloc).get();

    // Keep the TypeInfo alive: GetTensorTypeAndShapeInfo() returns a view
    // into it, and a one-liner would leave the view dangling.
    const auto inputTypeInfo = _session.GetInputTypeInfo(0);
    const auto info = inputTypeInfo.GetTensorTypeAndShapeInfo();
    auto shape = info.GetShape();
    // Dynamic batch dim (-1) becomes 1: the hot path scores one row.
    _featureCount = shape.empty() ? 0 : static_cast<size_t>(shape.back() < 0 ? 0 : shape.back());
    if (_featureCount == 0)
    {
      throw std::runtime_error("OnnxModel: input must have a static feature dimension");
    }

    _input.resize(_featureCount, 0.0f);
    _inputShape = {1, static_cast<int64_t>(_featureCount)};

    const auto outputTypeInfo = _session.GetOutputTypeInfo(0);
    const auto outInfo = outputTypeInfo.GetTensorTypeAndShapeInfo();
    auto outShape = outInfo.GetShape();
    size_t outCount = 1;
    for (auto d : outShape)
    {
      if (d > 0)
      {
        outCount *= static_cast<size_t>(d);
      }
    }
    _output.resize(outCount, 0.0f);
  }

  size_t featureCount() const noexcept { return _featureCount; }
  size_t outputCount() const noexcept { return _output.size(); }

  // Scores one feature vector. Returns the model outputs; the span stays
  // valid until the next run(). No allocations.
  std::span<const float> run(std::span<const double> features)
  {
    for (size_t i = 0; i < _featureCount; ++i)
    {
      _input[i] = static_cast<float>(features[i]);
    }

    auto inputTensor = Ort::Value::CreateTensor<float>(
        _memInfo, _input.data(), _input.size(), _inputShape.data(), _inputShape.size());

    const char* inNames[] = {_inputName.c_str()};
    const char* outNames[] = {_outputName.c_str()};
    auto results = _session.Run(Ort::RunOptions{nullptr}, inNames, &inputTensor, 1,
                                outNames, 1);

    const auto* data = results[0].GetTensorData<float>();
    const auto n = results[0].GetTensorTypeAndShapeInfo().GetElementCount();
    const auto count = std::min(_output.size(), static_cast<size_t>(n));
    std::copy(data, data + count, _output.begin());
    return {_output.data(), count};
  }

 private:
  Ort::Env _env;
  Ort::Session _session;
  Ort::MemoryInfo _memInfo;
  std::string _inputName;
  std::string _outputName;
  size_t _featureCount{0};
  std::vector<float> _input;
  std::vector<float> _output;
  std::array<int64_t, 2> _inputShape{};
};

// Sync in-path scoring with an enforced latency budget. Over-budget runs are
// counted and, under the DROP policy, subsequent calls return the previous
// output without running the model -- the strategy sees an explicit
// degradation instead of silently blowing its own budget.
class BudgetedInference
{
 public:
  enum class OverrunPolicy : uint8_t
  {
    WARN,  // count + histogram only, keep running the model
    DROP   // after an overrun, skip the model until reset()
  };

  BudgetedInference(OnnxModel& model, int64_t budgetNs,
                    OverrunPolicy policy = OverrunPolicy::WARN)
      : _model(model), _budgetNs(budgetNs), _policy(policy)
  {
  }

  struct Result
  {
    std::span<const float> values;
    bool fresh{true};  // false when DROP served the previous output
  };

  Result run(std::span<const double> features)
  {
    if (_dropped.load(std::memory_order_relaxed) && _policy == OverrunPolicy::DROP)
    {
      return {_last, false};
    }
    const auto t0 = performance::monotonicNs();
    _last = _model.run(features);
    const auto dt = performance::monotonicNs() - t0;
    _hist.record(dt);
    if (dt > _budgetNs)
    {
      _overruns.fetch_add(1, std::memory_order_relaxed);
      if (_policy == OverrunPolicy::DROP)
      {
        _dropped.store(true, std::memory_order_relaxed);
      }
    }
    return {_last, true};
  }

  void reset() { _dropped.store(false, std::memory_order_relaxed); }
  uint64_t overruns() const { return _overruns.load(std::memory_order_relaxed); }
  const performance::LatencyHistogram& histogram() const { return _hist; }

 private:
  OnnxModel& _model;
  int64_t _budgetNs;
  OverrunPolicy _policy;
  std::span<const float> _last{};
  std::atomic<uint64_t> _overruns{0};
  std::atomic<bool> _dropped{false};
  performance::LatencyHistogram _hist;
};

// Async sidecar: the model runs on its own thread; the hot path writes the
// latest feature vector (latest-wins mailbox) and reads the latest
// prediction with an explicit age. The strategy decides what staleness it
// tolerates -- the sidecar never blocks the hot path.
class OnnxSidecar
{
 public:
  explicit OnnxSidecar(std::unique_ptr<OnnxModel> model)
      : _model(std::move(model)),
        _features(_model->featureCount(), 0.0),
        _pendingFeatures(_model->featureCount(), 0.0),
        _prediction(_model->outputCount(), 0.0f)
  {
    _running.store(true, std::memory_order_release);
    _worker = std::thread([this]
                          { loop(); });
  }

  ~OnnxSidecar()
  {
    _running.store(false, std::memory_order_release);
    if (_worker.joinable())
    {
      _worker.join();
    }
  }

  // Hot path: submit the newest features. Never blocks on inference; if the
  // worker is busy, the previous pending vector is overwritten (latest wins).
  void submit(std::span<const double> features, int64_t nowNs)
  {
    {
      std::lock_guard lk(_inMutex);
      std::copy(features.begin(), features.end(), _pendingFeatures.begin());
      _pendingTsNs = nowNs;
      _hasPending = true;
    }
  }

  struct Prediction
  {
    std::vector<float> values;
    int64_t featureTsNs{0};  // timestamp of the features this was computed from
    uint64_t seq{0};         // increments per completed inference
  };

  // Hot path: read the latest completed prediction. ageNs = now - featureTs
  // tells the strategy how stale it is.
  Prediction latest() const
  {
    std::lock_guard lk(_outMutex);
    return {_prediction, _predictionTsNs, _seq.load(std::memory_order_relaxed)};
  }

 private:
  void loop()
  {
    while (_running.load(std::memory_order_acquire))
    {
      int64_t ts = 0;
      bool have = false;
      {
        std::lock_guard lk(_inMutex);
        if (_hasPending)
        {
          std::swap(_features, _pendingFeatures);
          ts = _pendingTsNs;
          _hasPending = false;
          have = true;
        }
      }
      if (!have)
      {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        continue;
      }
      const auto out = _model->run(_features);
      {
        std::lock_guard lk(_outMutex);
        _prediction.assign(out.begin(), out.end());
        _predictionTsNs = ts;
      }
      _seq.fetch_add(1, std::memory_order_relaxed);
    }
  }

  std::unique_ptr<OnnxModel> _model;

  mutable std::mutex _inMutex;
  std::vector<double> _features;
  std::vector<double> _pendingFeatures;
  int64_t _pendingTsNs{0};
  bool _hasPending{false};

  mutable std::mutex _outMutex;
  std::vector<float> _prediction;
  int64_t _predictionTsNs{0};
  std::atomic<uint64_t> _seq{0};

  std::atomic<bool> _running{false};
  std::thread _worker;
};

// Registers the model as a graph node: row t of the output column is the
// model scored on row t of the dependency columns. Because it is a regular
// node, backtest, replay and the streaming path execute the same inference
// the same way -- parity is structural, not procedural. Output column is the
// model's first output.
inline void registerOnnxNode(indicator::IndicatorGraph& graph, const std::string& name,
                             std::vector<std::string> deps,
                             std::shared_ptr<OnnxModel> model)
{
  graph.addNode(name, deps,
                [deps, model](indicator::IndicatorGraph& g, SymbolId sym)
                {
                  std::vector<const std::vector<double>*> cols;
                  cols.reserve(deps.size());
                  for (const auto& d : deps)
                  {
                    cols.push_back(&g.require(sym, d));
                  }
                  const size_t n = cols.empty() ? 0 : cols[0]->size();
                  std::vector<double> row(cols.size(), 0.0);
                  std::vector<double> out(n, std::numeric_limits<double>::quiet_NaN());
                  for (size_t t = 0; t < n; ++t)
                  {
                    bool valid = true;
                    for (size_t f = 0; f < cols.size(); ++f)
                    {
                      row[f] = (*cols[f])[t];
                      valid = valid && !std::isnan(row[f]);
                    }
                    if (!valid)
                    {
                      continue;  // warmup rows stay NaN
                    }
                    const auto res = model->run(row);
                    if (!res.empty())
                    {
                      out[t] = static_cast<double>(res[0]);
                    }
                  }
                  return out;
                });
}

}  // namespace flox::ml
