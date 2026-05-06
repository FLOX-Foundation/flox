## Heatmap rendering

`flox.report.heatmap_html` (Python) and `flox.report.heatmapHtml` (Node) render a 2D matrix as a self-contained HTML page with an inline-SVG heatmap. The renderer lives in `flox::report::renderHeatmapHtml` (C++), so all bindings produce byte-identical output for the same input. No external assets, no scripts beyond the SVG `<title>` tooltips.

The most natural pairing is with the [grid search](grid-search.md) results — sweep two axes, project a metric onto the resulting 2D grid, render.

## Python

```python
import flox_py as flox
from flox_py.report import write_heatmap

reg = flox.SymbolRegistry()
btc = reg.add_symbol("exchange", "BTCUSDT", 0.01)


def factory(params):
    fast, slow = int(params[0]), int(params[1])
    if fast >= slow:
        return {"sharpe": 0.0}
    # ... build BacktestRunner, run, return stats dict
    bt = flox.BacktestRunner(reg, 0.0004, 10_000)
    bt.set_strategy(_build(fast, slow))
    return bt.run_csv("data.csv", symbol="BTCUSDT")


fast_axis = [5.0, 10.0, 15.0, 20.0]
slow_axis = [20.0, 30.0, 50.0, 100.0]

gs = flox.GridSearch()
gs.add_axis(fast_axis)
gs.add_axis(slow_axis)
gs.set_factory(factory)
results = gs.run()

# Project sharpe onto the 2D grid (last axis varies fastest).
z = [
    [results[i * len(slow_axis) + j]["stats"]["sharpe"]
     for j in range(len(slow_axis))]
    for i in range(len(fast_axis))
]

write_heatmap(
    "sweep.html", z,
    row_labels=[f"fast={int(v)}" for v in fast_axis],
    col_labels=[f"slow={int(v)}" for v in slow_axis],
    title="SMA crossover sweep",
    x_axis_name="slow period",
    y_axis_name="fast period",
    metric_name="Sharpe ratio",
)
```

## Node

```js
const flox = require('@flox-foundation/flox');

const z = [
  [0.5, -0.3, 1.2],
  [0.8, 1.1, -1.4],
];
const html = flox.report.heatmapHtml(z, {
  rowLabels: ['fast=5', 'fast=10'],
  colLabels: ['slow=20', 'slow=30', 'slow=50'],
  title: 'Sweep',
  xAxisName: 'slow period',
  yAxisName: 'fast period',
  metricName: 'Sharpe ratio',
});
require('fs').writeFileSync('sweep.html', html);
```

## What the colormap means

Cells are coloured on a diverging red-to-green scale around the data midpoint:

- darkest red — minimum value in `z`
- mid grey — interpolation midpoint between min and max
- brightest green — maximum value in `z`

Each cell shows the numeric value on top of the colour and a `<title>` tooltip on hover.

## What the renderer does not do

- No interactive zoom or filtering. The output is a static SVG; if you need interactivity, render the data with Plotly / D3 instead.
- No PDF export. T014 covers PDF later.
- No multi-panel grids. One heatmap per file.

## See also

- [Grid search](grid-search.md) — produces the 2D data.
- [Walk-forward](walk-forward.md) — combine to render walk-forward optimisation surfaces.
- [Backtest HTML report](backtest-html-report.md) — full per-run HTML output.
