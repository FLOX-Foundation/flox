/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "flox/error/flox_error.h"
#include "flox/report/heatmap_html.h"

#include <fstream>
#include <string>
#include <vector>

namespace py = pybind11;

namespace flox_py
{

inline std::string heatmap_html(
    const std::vector<std::vector<double>>& z,
    const std::vector<std::string>& row_labels,
    const std::vector<std::string>& col_labels,
    const std::string& title,
    const std::string& x_axis_name,
    const std::string& y_axis_name,
    const std::string& metric_name)
{
  if (z.empty())
  {
    throw flox::FloxError(
        "E_LEN_002",
        "heatmap_html: z must be a non-empty 2D list of doubles.");
  }
  const std::size_t rows = z.size();
  const std::size_t cols = z[0].size();
  if (cols == 0)
  {
    throw flox::FloxError(
        "E_LEN_002",
        "heatmap_html: z rows must be non-empty.");
  }
  flox::report::HeatmapData d;
  d.rows = rows;
  d.cols = cols;
  d.z.reserve(rows * cols);
  for (const auto& row : z)
  {
    if (row.size() != cols)
    {
      throw flox::FloxError(
          "E_LEN_001",
          "heatmap_html: all rows in z must have the same length.");
    }
    for (double v : row)
    {
      d.z.push_back(v);
    }
  }
  d.rowLabels = row_labels;
  d.colLabels = col_labels;
  d.title = title;
  d.xAxisName = x_axis_name;
  d.yAxisName = y_axis_name;
  d.metricName = metric_name;
  return flox::report::renderHeatmapHtml(d);
}

inline void write_heatmap(const std::string& path,
                          const std::vector<std::vector<double>>& z,
                          const std::vector<std::string>& row_labels,
                          const std::vector<std::string>& col_labels,
                          const std::string& title,
                          const std::string& x_axis_name,
                          const std::string& y_axis_name,
                          const std::string& metric_name)
{
  std::string html = heatmap_html(z, row_labels, col_labels, title,
                                  x_axis_name, y_axis_name, metric_name);
  std::ofstream f(path);
  if (!f.is_open())
  {
    throw flox::FloxError(
        "E_IO_001",
        "write_heatmap: cannot open output file '" + path + "'.");
  }
  f << html;
}

inline void bindHeatmap(py::module_& m)
{
  auto report_mod = m.def_submodule("_heatmap",
                                    "Inline-SVG heatmap rendering "
                                    "(used by flox_py.report).");
  report_mod.def("heatmap_html", &heatmap_html,
                 py::arg("z"),
                 py::arg("row_labels") = std::vector<std::string>{},
                 py::arg("col_labels") = std::vector<std::string>{},
                 py::arg("title") = "",
                 py::arg("x_axis_name") = "",
                 py::arg("y_axis_name") = "",
                 py::arg("metric_name") = "",
                 "Render a self-contained HTML heatmap and return the "
                 "string. z is a list[list[float]], row-major.");
  report_mod.def("write_heatmap", &write_heatmap,
                 py::arg("path"), py::arg("z"),
                 py::arg("row_labels") = std::vector<std::string>{},
                 py::arg("col_labels") = std::vector<std::string>{},
                 py::arg("title") = "",
                 py::arg("x_axis_name") = "",
                 py::arg("y_axis_name") = "",
                 py::arg("metric_name") = "",
                 "Render and write a heatmap HTML to disk.");
}

}  // namespace flox_py
