/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace flox::report
{

/// Heatmap data — a 2D grid of metric values with axis labels.
struct HeatmapData
{
  /// Row-major: z[row * cols + col]. Length = rows * cols.
  std::vector<double> z;
  std::size_t rows{0};
  std::size_t cols{0};
  /// Length = rows. Optional. Empty → row indices.
  std::vector<std::string> rowLabels;
  /// Length = cols. Optional. Empty → column indices.
  std::vector<std::string> colLabels;
  std::string title;
  std::string xAxisName;
  std::string yAxisName;
  std::string metricName;  ///< Shown next to the value tooltip.
};

/// Render a self-contained HTML page with an inline-SVG heatmap.
/// Cells are coloured on a green-to-red diverging scale around the
/// data midpoint. No external assets, no scripts beyond what SVG
/// needs for hover tooltips.
std::string renderHeatmapHtml(const HeatmapData& data);

}  // namespace flox::report
