# From alpha to production: dataset, model, inference node

A model trained on flox features runs in production against the same
features, computed by the same code. This page walks the whole loop: tape
to dataset, dataset to model, model to a scoring node in C++.

The C++ inference part needs a build with `-DFLOX_ENABLE_ONNX=ON`
(ONNX Runtime).

## 1. Export a dataset

```python
import flox_py
from flox_py.dataset import build_dataset, to_arrow

ds = build_dataset(
    data_dir="/data/tapes/bybit",      # binary-log tape directory
    interval_seconds=60,
    features=["sma_20", "rsi_14", "parkinson_vol_30"],
    label_horizon_bars=5,              # y = log(close[t+5] / close[t])
)
X, y = ds["X"], ds["y"]
```

Row `t` is computed strictly from bars `0..t`. This is tested
(`python/tests/test_dataset_export.py`), so a model trained here cannot
accidentally learn from the future. `to_arrow(ds)` gives a `pyarrow` table
if the training stack wants parquet files.

## 2. Train and export ONNX

Any framework that exports ONNX works; that is the reason the runtime is
ONNX rather than a binding to one training library. sklearn example:

```python
from sklearn.linear_model import Ridge
from skl2onnx import to_onnx

model = Ridge().fit(X, y)
onnx_model = to_onnx(model, X[:1].astype("float32"))
with open("alpha.onnx", "wb") as f:
    f.write(onnx_model.SerializeToString())
```

## 3. Score in C++

Three execution modes share one loaded model class. Pick by how tightly
the score is coupled to the hot path:

| Mode | Coupling | Use when |
|---|---|---|
| Graph node | none (batch) | backtest, replay, research |
| `BudgetedInference` | in-path, budgeted | the score gates the order |
| `OnnxSidecar` | decoupled | the score enriches state, staleness is acceptable |

As a graph node, for backtest and replay:

```cpp
#include "flox/ml/onnx_inference.h"

auto model = std::make_shared<flox::ml::OnnxModel>("alpha.onnx");
flox::ml::registerOnnxNode(graph, "alpha", {"sma_20", "rsi_14", "pvol_30"}, model);
const auto& alphaColumn = graph.require(symbol, "alpha");
```

Backtest and live execute the same node, so their scores agree by
construction rather than by a separate verification step.

In-path with a budget:

```cpp
flox::ml::OnnxModel hotModel("alpha.onnx");  // intra-op = 1, no thread pool
flox::ml::BudgetedInference inference(hotModel, /*budgetNs=*/20'000);
const auto r = inference.run(featureRow);
```

`r.fresh` is false when the DROP policy served the previous score after a
budget overrun. `inference.histogram()` plugs into the latency contour.

As a sidecar, for heavier models:

```cpp
flox::ml::OnnxSidecar sidecar(std::make_unique<flox::ml::OnnxModel>("alpha.onnx"));
sidecar.submit(featureRow, nowNs);          // returns immediately
const auto p = sidecar.latest();            // values + featureTsNs + seq
```

`nowNs - p.featureTsNs` is the staleness of the score; the strategy
decides how much it tolerates.

## What still needs your attention

Feature order. Train and serve must agree on the order of inputs, and the
node takes its dependency list explicitly for that reason. Everything else
on the parity list is covered by tests: feature values
(`python/tests/test_dataset_export.py`) and node scoring
(`tests/test_onnx_inference.cpp`).
