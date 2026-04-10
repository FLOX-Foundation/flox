#pragma once

#include "flox/aggregator/bar.h"
#include "flox/aggregator/events/bar_event.h"
#include "flox/common.h"

#include <atomic>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

namespace flox::indicator
{

inline std::unordered_map<SymbolId, std::vector<Bar>> partitionBySymbol(
    std::span<const Bar> bars, std::span<const SymbolId> symbols)
{
  std::unordered_map<SymbolId, std::vector<Bar>> result;
  for (size_t i = 0; i < bars.size(); ++i)
  {
    result[symbols[i]].push_back(bars[i]);
  }
  return result;
}

inline std::unordered_map<SymbolId, std::vector<Bar>> partitionBySymbol(
    std::span<const BarEvent> events)
{
  std::unordered_map<SymbolId, std::vector<Bar>> result;
  for (const auto& ev : events)
  {
    result[ev.symbol].push_back(ev.bar);
  }
  return result;
}

template <typename Fn>
void forEachSymbol(const std::unordered_map<SymbolId, std::vector<Bar>>& data, Fn&& fn)
{
  for (const auto& [sym, bars] : data)
  {
    fn(sym, std::span<const Bar>(bars));
  }
}

// fn must be thread-safe.
template <typename Fn>
void forEachSymbolParallel(const std::unordered_map<SymbolId, std::vector<Bar>>& data,
                           Fn&& fn, int threads = 0)
{
  if (data.empty())
  {
    return;
  }
  if (threads <= 0)
  {
    threads = static_cast<int>(std::thread::hardware_concurrency());
  }

  std::vector<std::pair<SymbolId, const std::vector<Bar>*>> items;
  items.reserve(data.size());
  for (const auto& [sym, bars] : data)
  {
    items.emplace_back(sym, &bars);
  }

  std::atomic<size_t> next{0};
  auto worker = [&]()
  {
    while (true)
    {
      size_t idx = next.fetch_add(1, std::memory_order_relaxed);
      if (idx >= items.size())
      {
        break;
      }
      fn(items[idx].first, std::span<const Bar>(*items[idx].second));
    }
  };

  int actual = std::min(threads, static_cast<int>(items.size()));
  std::vector<std::thread> pool;
  pool.reserve(actual);
  for (int i = 0; i < actual; ++i)
  {
    pool.emplace_back(worker);
  }
  for (auto& t : pool)
  {
    t.join();
  }
}

}  // namespace flox::indicator
