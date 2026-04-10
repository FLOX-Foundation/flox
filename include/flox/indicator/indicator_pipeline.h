#pragma once

#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "flox/aggregator/bar.h"
#include "flox/common.h"

namespace flox::indicator
{

// Indicator computation graph with dependency resolution.
//
// Register nodes with dependencies. require() computes only what's needed,
// in the right order, caching results.
//
//   IndicatorGraph g;
//   g.setBars(0, myBars);
//
//   g.addNode("atr14", {}, [](auto& g, auto sym) {
//       return ATR(14).compute(g.high(sym), g.low(sym), g.close(sym));
//   });
//   g.addNode("ema50", {}, [](auto& g, auto sym) {
//       return EMA(50).compute(g.close(sym));
//   });
//   g.addNode("norm_slope", {"ema50", "atr14"}, [](auto& g, auto sym) {
//       auto slope = Slope(1).compute(*g.get(sym, "ema50"));
//       auto& atr = *g.get(sym, "atr14");
//       // ...
//   });
//
//   auto& ns = g.require(0, "norm_slope");
//   // computed: ema50, atr14, norm_slope (only what's needed)

class IndicatorGraph
{
 public:
  using ComputeFn = std::function<std::vector<double>(IndicatorGraph&, SymbolId)>;

  void setBars(SymbolId symbol, std::span<const Bar> bars)
  {
    _bars[symbol] = {bars.begin(), bars.end()};
    invalidate(symbol);
  }

  std::span<const Bar> bars(SymbolId symbol) const
  {
    auto it = _bars.find(symbol);
    return it != _bars.end() ? std::span<const Bar>(it->second) : std::span<const Bar>{};
  }

  const std::vector<double>& close(SymbolId sym)
  {
    return fieldCache(sym, 0, [](const Bar& b)
                      { return b.close.toDouble(); });
  }
  const std::vector<double>& high(SymbolId sym)
  {
    return fieldCache(sym, 1, [](const Bar& b)
                      { return b.high.toDouble(); });
  }
  const std::vector<double>& low(SymbolId sym)
  {
    return fieldCache(sym, 2, [](const Bar& b)
                      { return b.low.toDouble(); });
  }
  const std::vector<double>& volume(SymbolId sym)
  {
    return fieldCache(sym, 3, [](const Bar& b)
                      { return b.volume.toDouble(); });
  }

  void addNode(const std::string& name, std::vector<std::string> deps, ComputeFn fn)
  {
    _nodes[name] = {std::move(deps), std::move(fn)};
  }

  const std::vector<double>& require(SymbolId symbol, const std::string& name)
  {
    auto key = CacheKey{symbol, name};
    auto it = _cache.find(key);
    if (it != _cache.end())
    {
      return it->second;
    }

    auto nodeIt = _nodes.find(name);
    if (nodeIt == _nodes.end())
    {
      throw std::logic_error("unknown node: " + name);
    }

    auto cycleKey = CacheKey{symbol, name};
    if (_computing.count(cycleKey))
    {
      throw std::logic_error("circular dependency: " + name);
    }
    _computing.insert(cycleKey);

    for (const auto& dep : nodeIt->second.deps)
    {
      require(symbol, dep);
    }

    auto result = nodeIt->second.fn(*this, symbol);
    _computing.erase(cycleKey);
    return _cache.emplace(key, std::move(result)).first->second;
  }

  const std::vector<double>* get(SymbolId symbol, const std::string& name) const
  {
    auto it = _cache.find(CacheKey{symbol, name});
    return it != _cache.end() ? &it->second : nullptr;
  }

  void invalidate(SymbolId symbol)
  {
    for (auto it = _cache.begin(); it != _cache.end();)
    {
      if (it->first.first == symbol)
      {
        it = _cache.erase(it);
      }
      else
      {
        ++it;
      }
    }
    for (auto it = _fields.begin(); it != _fields.end();)
    {
      if (it->first.first == symbol)
      {
        it = _fields.erase(it);
      }
      else
      {
        ++it;
      }
    }
  }

  void invalidateAll()
  {
    _cache.clear();
    _fields.clear();
  }

 private:
  struct Node
  {
    std::vector<std::string> deps;
    ComputeFn fn;
  };

  using CacheKey = std::pair<SymbolId, std::string>;
  struct CacheKeyHash
  {
    size_t operator()(const CacheKey& k) const
    {
      return std::hash<SymbolId>{}(k.first) ^ (std::hash<std::string>{}(k.second) * 2654435761u);
    }
  };

  using FieldKey = std::pair<SymbolId, int>;
  struct FieldKeyHash
  {
    size_t operator()(const FieldKey& k) const
    {
      return std::hash<SymbolId>{}(k.first) ^ (std::hash<int>{}(k.second) * 2654435761u);
    }
  };

  template <typename Fn>
  const std::vector<double>& fieldCache(SymbolId symbol, int fieldIdx, Fn fn)
  {
    auto key = FieldKey{symbol, fieldIdx};
    auto it = _fields.find(key);
    if (it != _fields.end())
    {
      return it->second;
    }
    auto b = bars(symbol);
    std::vector<double> out(b.size());
    for (size_t i = 0; i < b.size(); ++i)
    {
      out[i] = fn(b[i]);
    }
    return _fields.emplace(key, std::move(out)).first->second;
  }

  std::unordered_map<SymbolId, std::vector<Bar>> _bars;
  std::unordered_map<std::string, Node> _nodes;
  std::unordered_map<CacheKey, std::vector<double>, CacheKeyHash> _cache;
  std::unordered_map<FieldKey, std::vector<double>, FieldKeyHash> _fields;
  std::unordered_set<CacheKey, CacheKeyHash> _computing;
};

}  // namespace flox::indicator
