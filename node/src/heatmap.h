// node/src/heatmap.h — heatmap renderer wrapper for Node.js

#pragma once

#include <napi.h>

#include "error_translator.h"
#include "flox/capi/flox_capi.h"

#include <string>
#include <vector>

namespace node_flox
{

// flox.report.heatmapHtml(z, opts?) → string
// z is a 2D number[][]. opts: { rowLabels?, colLabels?, title?,
//   xAxisName?, yAxisName?, metricName? }.
inline Napi::Value heatmapHtml(const Napi::CallbackInfo& info)
{
  return tryFlox(info.Env(), [&]() -> Napi::Value
                 {
    auto env = info.Env();
    if (info.Length() == 0 || !info[0].IsArray())
    {
      throw flox::FloxError(
          "E_LEN_002",
          "heatmapHtml: first argument must be a 2D array of numbers.");
    }
    auto outer = info[0].As<Napi::Array>();
    const uint32_t rows = outer.Length();
    if (rows == 0)
    {
      throw flox::FloxError("E_LEN_002", "heatmapHtml: z is empty.");
    }
    auto firstRow = outer.Get(uint32_t(0)).As<Napi::Array>();
    const uint32_t cols = firstRow.Length();
    if (cols == 0)
    {
      throw flox::FloxError("E_LEN_002", "heatmapHtml: z rows are empty.");
    }
    std::vector<double> z(rows * cols);
    for (uint32_t r = 0; r < rows; ++r)
    {
      auto row = outer.Get(r).As<Napi::Array>();
      if (row.Length() != cols)
      {
        throw flox::FloxError(
            "E_LEN_001",
            "heatmapHtml: all rows in z must have the same length.");
      }
      for (uint32_t c = 0; c < cols; ++c)
      {
        z[r * cols + c] =
            row.Get(c).As<Napi::Number>().DoubleValue();
      }
    }

    auto getStringArr = [&](Napi::Object opts, const char* key,
                            std::vector<std::string>& out)
    {
      if (opts.Has(key))
      {
        auto val = opts.Get(key);
        if (val.IsArray())
        {
          auto arr = val.As<Napi::Array>();
          for (uint32_t i = 0; i < arr.Length(); ++i)
          {
            out.push_back(arr.Get(i).As<Napi::String>().Utf8Value());
          }
        }
      }
    };
    auto getString = [&](Napi::Object opts, const char* key) -> std::string
    {
      if (opts.Has(key))
      {
        auto val = opts.Get(key);
        if (val.IsString())
        {
          return val.As<Napi::String>().Utf8Value();
        }
      }
      return "";
    };

    std::vector<std::string> rowLabels, colLabels;
    std::string title, xAxis, yAxis, metric;
    if (info.Length() > 1 && info[1].IsObject())
    {
      auto opts = info[1].As<Napi::Object>();
      getStringArr(opts, "rowLabels", rowLabels);
      getStringArr(opts, "colLabels", colLabels);
      title = getString(opts, "title");
      xAxis = getString(opts, "xAxisName");
      yAxis = getString(opts, "yAxisName");
      metric = getString(opts, "metricName");
    }

    std::vector<const char*> rowLabelPtrs;
    rowLabelPtrs.reserve(rowLabels.size());
    for (const auto& s : rowLabels){ rowLabelPtrs.push_back(s.c_str());
}
    std::vector<const char*> colLabelPtrs;
    colLabelPtrs.reserve(colLabels.size());
    for (const auto& s : colLabels){ colLabelPtrs.push_back(s.c_str());
}

    FloxHeatmapData d{};
    d.z = z.data();
    d.rows = rows;
    d.cols = cols;
    d.row_labels = rowLabelPtrs.empty() ? nullptr : rowLabelPtrs.data();
    d.num_row_labels = static_cast<uint32_t>(rowLabelPtrs.size());
    d.col_labels = colLabelPtrs.empty() ? nullptr : colLabelPtrs.data();
    d.num_col_labels = static_cast<uint32_t>(colLabelPtrs.size());
    d.title = title.c_str();
    d.x_axis_name = xAxis.c_str();
    d.y_axis_name = yAxis.c_str();
    d.metric_name = metric.c_str();

    uint64_t total = flox_render_heatmap_html(&d, nullptr, 0);
    std::string buf(total, '\0');
    flox_render_heatmap_html(&d, buf.data(), total);
    return Napi::String::New(env, buf); });
}

inline void registerHeatmap(Napi::Env env, Napi::Object exports)
{
  // Group under exports.report.{heatmapHtml}
  Napi::Object report;
  if (exports.Has("report") && exports.Get("report").IsObject())
  {
    report = exports.Get("report").As<Napi::Object>();
  }
  else
  {
    report = Napi::Object::New(env);
    exports.Set("report", report);
  }
  report.Set("heatmapHtml",
             Napi::Function::New(env, &heatmapHtml, "heatmapHtml"));
}

}  // namespace node_flox
