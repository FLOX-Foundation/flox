#pragma once

// flox/indicator/streaming.h
//
// CRTP mixins that give any compute()-providing indicator a streaming API
// (update / value / ready / reset / count) on the *same* class.
//
// Design contract:
//
//   - One class per indicator. Same name as before. Same compute() as before.
//   - Inheriting from the appropriate Streaming<Kind><Derived> mixin adds
//     streaming methods that work by accumulating history and re-running
//     compute() on it. value() == compute(history).back().
//   - This guarantees batch ↔ streaming parity by construction; no separate
//     ring buffer / alpha / seed logic to drift.
//
// Cost: O(N) per update where N = accumulated history. Acceptable for
// research / UI / moderate-throughput live trading. For high-frequency: use
// compute(span) directly. Specific indicators may later override update() with
// O(1) state-keeping IF they keep value() output identical (parity test must
// pass).

#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

// -----------------------------------------------------------------------------
// SingleInput — compute(span) -> vector<double>
// -----------------------------------------------------------------------------
template <typename Derived>
class StreamingSingle
{
 public:
  void update(double v)
  {
    _history.push_back(v);
    _dirty = true;
  }

  double value() const
  {
    ensureFresh();
    return _last;
  }

  bool ready() const
  {
    ensureFresh();
    return std::isfinite(_last);
  }

  void reset()
  {
    _history.clear();
    _last = std::nan("");
    _dirty = false;
  }

  size_t count() const noexcept { return _history.size(); }

 protected:
  const std::vector<double>& streamHistory() const noexcept { return _history; }

 private:
  void ensureFresh() const
  {
    if (!_dirty)
    {
      return;
    }
    if (_history.empty())
    {
      _last = std::nan("");
    }
    else
    {
      auto out = static_cast<const Derived*>(this)->compute(
          std::span<const double>(_history));
      _last = out.empty() ? std::nan("") : out.back();
    }
    _dirty = false;
  }

  std::vector<double> _history;
  mutable double _last = std::nan("");
  mutable bool _dirty = false;
};

// -----------------------------------------------------------------------------
// BarInput — compute(high, low, close) -> vector<double>
// -----------------------------------------------------------------------------
template <typename Derived>
class StreamingBar
{
 public:
  void update(double high, double low, double close)
  {
    _high.push_back(high);
    _low.push_back(low);
    _close.push_back(close);
    _dirty = true;
  }

  double value() const
  {
    ensureFresh();
    return _last;
  }

  bool ready() const
  {
    ensureFresh();
    return std::isfinite(_last);
  }

  void reset()
  {
    _high.clear();
    _low.clear();
    _close.clear();
    _last = std::nan("");
    _dirty = false;
  }

  size_t count() const noexcept { return _high.size(); }

 protected:
  const std::vector<double>& streamHigh() const noexcept { return _high; }
  const std::vector<double>& streamLow() const noexcept { return _low; }
  const std::vector<double>& streamClose() const noexcept { return _close; }

 private:
  void ensureFresh() const
  {
    if (!_dirty)
    {
      return;
    }
    if (_high.empty())
    {
      _last = std::nan("");
    }
    else
    {
      auto out = static_cast<const Derived*>(this)->compute(
          std::span<const double>(_high), std::span<const double>(_low),
          std::span<const double>(_close));
      _last = out.empty() ? std::nan("") : out.back();
    }
    _dirty = false;
  }

  std::vector<double> _high, _low, _close;
  mutable double _last = std::nan("");
  mutable bool _dirty = false;
};

// -----------------------------------------------------------------------------
// HighLowInput — compute(high, low) -> vector<double>  (or via output buffer)
// -----------------------------------------------------------------------------
template <typename Derived>
class StreamingHighLow
{
 public:
  void update(double high, double low)
  {
    _high.push_back(high);
    _low.push_back(low);
    _dirty = true;
  }

  double value() const
  {
    ensureFresh();
    return _last;
  }

  bool ready() const
  {
    ensureFresh();
    return std::isfinite(_last);
  }

  void reset()
  {
    _high.clear();
    _low.clear();
    _last = std::nan("");
    _dirty = false;
  }

  size_t count() const noexcept { return _high.size(); }

 private:
  void ensureFresh() const
  {
    if (!_dirty)
    {
      return;
    }
    if (_high.empty())
    {
      _last = std::nan("");
    }
    else
    {
      auto out = static_cast<const Derived*>(this)->compute(
          std::span<const double>(_high), std::span<const double>(_low));
      _last = out.empty() ? std::nan("") : out.back();
    }
    _dirty = false;
  }

  std::vector<double> _high, _low;
  mutable double _last = std::nan("");
  mutable bool _dirty = false;
};

// -----------------------------------------------------------------------------
// OhlcInput — compute(open, high, low, close) -> vector<double>
// -----------------------------------------------------------------------------
template <typename Derived>
class StreamingOhlc
{
 public:
  void update(double open, double high, double low, double close)
  {
    _o.push_back(open);
    _h.push_back(high);
    _l.push_back(low);
    _c.push_back(close);
    _dirty = true;
  }

  double value() const
  {
    ensureFresh();
    return _last;
  }

  bool ready() const
  {
    ensureFresh();
    return std::isfinite(_last);
  }

  void reset()
  {
    _o.clear();
    _h.clear();
    _l.clear();
    _c.clear();
    _last = std::nan("");
    _dirty = false;
  }

  size_t count() const noexcept { return _o.size(); }

 private:
  void ensureFresh() const
  {
    if (!_dirty)
    {
      return;
    }
    if (_o.empty())
    {
      _last = std::nan("");
    }
    else
    {
      auto out = static_cast<const Derived*>(this)->compute(
          std::span<const double>(_o), std::span<const double>(_h),
          std::span<const double>(_l), std::span<const double>(_c));
      _last = out.empty() ? std::nan("") : out.back();
    }
    _dirty = false;
  }

  std::vector<double> _o, _h, _l, _c;
  mutable double _last = std::nan("");
  mutable bool _dirty = false;
};

// -----------------------------------------------------------------------------
// PairInput — compute(x, y) -> vector<double>   (e.g. Correlation)
// -----------------------------------------------------------------------------
template <typename Derived>
class StreamingPair
{
 public:
  void update(double x, double y)
  {
    _x.push_back(x);
    _y.push_back(y);
    _dirty = true;
  }

  double value() const
  {
    ensureFresh();
    return _last;
  }

  bool ready() const
  {
    ensureFresh();
    return std::isfinite(_last);
  }

  void reset()
  {
    _x.clear();
    _y.clear();
    _last = std::nan("");
    _dirty = false;
  }

  size_t count() const noexcept { return _x.size(); }

 private:
  void ensureFresh() const
  {
    if (!_dirty)
    {
      return;
    }
    if (_x.empty())
    {
      _last = std::nan("");
    }
    else
    {
      auto out = static_cast<const Derived*>(this)->compute(
          std::span<const double>(_x), std::span<const double>(_y));
      _last = out.empty() ? std::nan("") : out.back();
    }
    _dirty = false;
  }

  std::vector<double> _x, _y;
  mutable double _last = std::nan("");
  mutable bool _dirty = false;
};

// -----------------------------------------------------------------------------
// MultiOutput on single-input — compute(span) -> Result struct
//
// Result must expose `line` (or `middle`) `signal` / `histogram` / `upper` /
// `lower` as vector<double>. Mixin requires Derived to provide:
//   using ResultT = ...;
//   ResultT compute(span) const;
//   static double primary(const ResultT&);          // value()
//   static double named(const ResultT&, std::string_view)  // optional
//
// We keep streaming generic with a primary() function pointer; named getters
// can be added per-indicator on the public class.
// -----------------------------------------------------------------------------
template <typename Derived, typename Result>
class StreamingMultiOutputSingle
{
 public:
  void update(double v)
  {
    _history.push_back(v);
    _dirty = true;
  }

  const Result& result() const
  {
    ensureFresh();
    return _last;
  }

  bool ready() const
  {
    ensureFresh();
    return _hasValue;
  }

  void reset()
  {
    _history.clear();
    _last = Result{};
    _dirty = false;
    _hasValue = false;
  }

  size_t count() const noexcept { return _history.size(); }

 protected:
  const std::vector<double>& streamHistory() const noexcept { return _history; }

 private:
  void ensureFresh() const
  {
    if (!_dirty)
    {
      return;
    }
    if (_history.empty())
    {
      _last = Result{};
      _hasValue = false;
    }
    else
    {
      _last = static_cast<const Derived*>(this)->compute(
          std::span<const double>(_history));
      _hasValue = !_last.line.empty() && std::isfinite(_last.line.back());
    }
    _dirty = false;
  }

  std::vector<double> _history;
  mutable Result _last;
  mutable bool _dirty = false;
  mutable bool _hasValue = false;
};

}  // namespace flox::indicator
