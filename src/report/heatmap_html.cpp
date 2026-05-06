/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/report/heatmap_html.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace flox::report
{

namespace
{

std::string escape(const std::string& s)
{
  std::string out;
  out.reserve(s.size());
  for (char c : s)
  {
    switch (c)
    {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      case '\'':
        out += "&#39;";
        break;
      default:
        out += c;
    }
  }
  return out;
}

std::string fmtNum(double v, int prec = 4)
{
  if (!std::isfinite(v))
  {
    return "—";
  }
  std::ostringstream os;
  os << std::fixed << std::setprecision(prec) << v;
  return os.str();
}

// Diverging colormap: red-low, white-mid, green-high.
// t in [0, 1], mid is the midpoint.
std::string colorFor(double t)
{
  if (!std::isfinite(t))
  {
    t = 0.5;
  }
  t = std::clamp(t, 0.0, 1.0);
  // 0.0 → red (#f85149), 0.5 → near-white (#222), 1.0 → green (#3fb950)
  // Lerp through dark midpoint for dark theme.
  int r, g, b;
  if (t < 0.5)
  {
    double k = t / 0.5;
    r = static_cast<int>(248 + (40 - 248) * k);
    g = static_cast<int>(81 + (40 - 81) * k);
    b = static_cast<int>(73 + (50 - 73) * k);
  }
  else
  {
    double k = (t - 0.5) / 0.5;
    r = static_cast<int>(40 + (63 - 40) * k);
    g = static_cast<int>(40 + (185 - 40) * k);
    b = static_cast<int>(50 + (80 - 50) * k);
  }
  std::ostringstream os;
  os << "#" << std::hex << std::setfill('0') << std::setw(2) << r
     << std::setw(2) << g << std::setw(2) << b;
  return os.str();
}

}  // namespace

std::string renderHeatmapHtml(const HeatmapData& d)
{
  const std::size_t rows = d.rows;
  const std::size_t cols = d.cols;

  double zmin = std::numeric_limits<double>::infinity();
  double zmax = -std::numeric_limits<double>::infinity();
  for (double v : d.z)
  {
    if (!std::isfinite(v))
    {
      continue;
    }
    zmin = std::min(zmin, v);
    zmax = std::max(zmax, v);
  }
  if (!std::isfinite(zmin) || !std::isfinite(zmax) || zmax <= zmin)
  {
    zmax = zmin + 1.0;
  }

  // SVG geometry.
  const int leftPad = 90;
  const int topPad = 50;
  const int cellW = 56;
  const int cellH = 32;
  const int rightPad = 16;
  const int bottomPad = 30;
  const int width = leftPad + static_cast<int>(cols) * cellW + rightPad;
  const int height = topPad + static_cast<int>(rows) * cellH + bottomPad;

  std::ostringstream svg;
  svg << "<svg viewBox=\"0 0 " << width << " " << height
      << "\" xmlns=\"http://www.w3.org/2000/svg\">\n";

  // Title bar.
  if (!d.title.empty())
  {
    svg << "<text x=\"" << (leftPad + 4) << "\" y=\"20\" "
        << "fill=\"#e6edf3\" font-size=\"14\" font-weight=\"600\">"
        << escape(d.title) << "</text>\n";
  }
  if (!d.metricName.empty())
  {
    svg << "<text x=\"" << (leftPad + 4) << "\" y=\"38\" "
        << "fill=\"#7d8590\" font-size=\"11\">metric: "
        << escape(d.metricName) << "</text>\n";
  }

  // Column labels (top axis).
  for (std::size_t c = 0; c < cols; ++c)
  {
    int cx = leftPad + static_cast<int>(c) * cellW + cellW / 2;
    std::string lbl = c < d.colLabels.size() ? d.colLabels[c]
                                             : std::to_string(c);
    svg << "<text x=\"" << cx << "\" y=\"" << (topPad - 6)
        << "\" fill=\"#7d8590\" font-size=\"11\" text-anchor=\"middle\">"
        << escape(lbl) << "</text>\n";
  }

  // Row labels (left axis).
  for (std::size_t r = 0; r < rows; ++r)
  {
    int cy = topPad + static_cast<int>(r) * cellH + cellH / 2 + 4;
    std::string lbl = r < d.rowLabels.size() ? d.rowLabels[r]
                                             : std::to_string(r);
    svg << "<text x=\"" << (leftPad - 6) << "\" y=\"" << cy
        << "\" fill=\"#7d8590\" font-size=\"11\" text-anchor=\"end\">"
        << escape(lbl) << "</text>\n";
  }

  // Cells.
  for (std::size_t r = 0; r < rows; ++r)
  {
    for (std::size_t c = 0; c < cols; ++c)
    {
      const double v = d.z[r * cols + c];
      double t = (v - zmin) / (zmax - zmin);
      if (!std::isfinite(t))
      {
        t = 0.5;
      }
      int x = leftPad + static_cast<int>(c) * cellW;
      int y = topPad + static_cast<int>(r) * cellH;
      svg << "<rect x=\"" << x << "\" y=\"" << y
          << "\" width=\"" << cellW << "\" height=\"" << cellH
          << "\" fill=\"" << colorFor(t) << "\" stroke=\"#0e1116\">"
          << "<title>" << escape(d.metricName.empty() ? "value" : d.metricName)
          << ": " << fmtNum(v) << "</title></rect>\n";
      // Cell label on top of color.
      svg << "<text x=\"" << (x + cellW / 2) << "\" y=\"" << (y + cellH / 2 + 4)
          << "\" fill=\"#e6edf3\" font-size=\"10\" text-anchor=\"middle\" "
          << "style=\"pointer-events:none\">" << fmtNum(v, 3) << "</text>\n";
    }
  }

  // Axis names.
  if (!d.xAxisName.empty())
  {
    int cx = leftPad + static_cast<int>(cols) * cellW / 2;
    svg << "<text x=\"" << cx << "\" y=\"" << (height - 8)
        << "\" fill=\"#e6edf3\" font-size=\"11\" text-anchor=\"middle\">"
        << escape(d.xAxisName) << "</text>\n";
  }
  if (!d.yAxisName.empty())
  {
    int cy = topPad + static_cast<int>(rows) * cellH / 2;
    svg << "<text x=\"14\" y=\"" << cy
        << "\" fill=\"#e6edf3\" font-size=\"11\" text-anchor=\"middle\" "
        << "transform=\"rotate(-90 14 " << cy << ")\">"
        << escape(d.yAxisName) << "</text>\n";
  }
  svg << "</svg>";

  std::ostringstream html;
  html << "<!doctype html>\n<html lang=\"en\">\n<head>\n"
       << "<meta charset=\"utf-8\">\n"
       << "<title>" << escape(d.title.empty() ? "FLOX heatmap" : d.title)
       << "</title>\n"
       << "<style>\n"
       << "  body{margin:0;background:#0e1116;color:#e6edf3;"
       << "font:13px/1.4 -apple-system,Inter,sans-serif;}\n"
       << "  main{padding:24px 32px;max-width:1100px;margin:0 auto;}\n"
       << "  svg{background:#161b22;border:1px solid #30363d;"
       << "border-radius:6px;padding:12px;}\n"
       << "</style>\n</head>\n<body>\n<main>\n"
       << svg.str() << "\n</main>\n</body>\n</html>\n";
  return html.str();
}

}  // namespace flox::report
