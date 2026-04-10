#pragma once

#include "flox/aggregator/bar.h"

#include <cassert>
#include <span>
#include <vector>

namespace flox::indicator
{

namespace detail
{
template <typename Fn>
inline std::vector<double> extract(std::span<const Bar> bars, Fn fn)
{
  std::vector<double> out(bars.size());
  for (size_t i = 0; i < bars.size(); ++i)
  {
    out[i] = fn(bars[i]);
  }
  return out;
}

template <typename Fn>
inline void extract(std::span<const Bar> bars, std::span<double> out, Fn fn)
{
  assert(out.size() >= bars.size());
  for (size_t i = 0; i < bars.size(); ++i)
  {
    out[i] = fn(bars[i]);
  }
}
}  // namespace detail

inline std::vector<double> close(std::span<const Bar> bars)
{
  return detail::extract(bars, [](const Bar& b)
                         { return b.close.toDouble(); });
}
inline std::vector<double> high(std::span<const Bar> bars)
{
  return detail::extract(bars, [](const Bar& b)
                         { return b.high.toDouble(); });
}
inline std::vector<double> low(std::span<const Bar> bars)
{
  return detail::extract(bars, [](const Bar& b)
                         { return b.low.toDouble(); });
}
inline std::vector<double> open(std::span<const Bar> bars)
{
  return detail::extract(bars, [](const Bar& b)
                         { return b.open.toDouble(); });
}
inline std::vector<double> volume(std::span<const Bar> bars)
{
  return detail::extract(bars, [](const Bar& b)
                         { return b.volume.toDouble(); });
}

inline void close(std::span<const Bar> bars, std::span<double> out)
{
  detail::extract(bars, out, [](const Bar& b)
                  { return b.close.toDouble(); });
}
inline void high(std::span<const Bar> bars, std::span<double> out)
{
  detail::extract(bars, out, [](const Bar& b)
                  { return b.high.toDouble(); });
}
inline void low(std::span<const Bar> bars, std::span<double> out)
{
  detail::extract(bars, out, [](const Bar& b)
                  { return b.low.toDouble(); });
}
inline void open(std::span<const Bar> bars, std::span<double> out)
{
  detail::extract(bars, out, [](const Bar& b)
                  { return b.open.toDouble(); });
}
inline void volume(std::span<const Bar> bars, std::span<double> out)
{
  detail::extract(bars, out, [](const Bar& b)
                  { return b.volume.toDouble(); });
}

}  // namespace flox::indicator
