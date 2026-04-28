#pragma once

#include "flox/aggregator/bar.h"
#include "flox/common.h"
#include "flox/indicator/indicator_pipeline.h"

#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace flox::indicator
{

// Streaming wrapper around IndicatorGraph.
//
// Accumulates bars one at a time via step(). After each step the batch graph
// runs on the full accumulated history and each node's last value is stored as
// the current output. This guarantees exact parity with a batch run on the same
// bars.
//
//   StreamingIndicatorGraph sg;
//   sg.addNode("ema5", {}, [](auto& g, auto sym) {
//       return EMA(5).compute(g.close(sym));
//   });
//   for (const auto& bar : live_bars)
//   {
//     sg.step(sym, bar);
//     double val = sg.current(sym, "ema5"); // NaN until warm-up done
//   }
class StreamingIndicatorGraph
{
 public:
  using ComputeFn = IndicatorGraph::ComputeFn;

  void addNode(std::string name, std::vector<std::string> deps, ComputeFn fn)
  {
    _nodeNames.push_back(name);
    _graph.addNode(std::move(name), std::move(deps), std::move(fn));
  }

  void step(SymbolId symbol, const Bar& bar)
  {
    _history[symbol].push_back(bar);
    _graph.setBars(symbol, _history[symbol]);
    for (const auto& name : _nodeNames)
    {
      try
      {
        const auto& v = _graph.require(symbol, name);
        _current[{symbol, name}] = v.empty() ? std::nan("") : v.back();
      }
      catch (...)
      {
        _current[{symbol, name}] = std::nan("");
      }
    }
  }

  // Returns the most recent computed value for (symbol, name).
  // Returns NaN if no bars have been stepped or the node is not warm yet.
  double current(SymbolId symbol, const std::string& name) const
  {
    auto it = _current.find({symbol, name});
    return it != _current.end() ? it->second : std::nan("");
  }

  size_t barCount(SymbolId symbol) const
  {
    auto it = _history.find(symbol);
    return it != _history.end() ? it->second.size() : 0;
  }

  void reset(SymbolId symbol)
  {
    _history.erase(symbol);
    _graph.invalidate(symbol);
    for (auto it = _current.begin(); it != _current.end();)
    {
      if (it->first.first == symbol)
      {
        it = _current.erase(it);
      }
      else
      {
        ++it;
      }
    }
  }

  void resetAll()
  {
    _history.clear();
    _graph.invalidateAll();
    _current.clear();
  }

  IndicatorGraph& batchGraph() { return _graph; }

 private:
  IndicatorGraph _graph;
  std::vector<std::string> _nodeNames;
  std::unordered_map<SymbolId, std::vector<Bar>> _history;

  using CurrentKey = std::pair<SymbolId, std::string>;
  struct CurrentKeyHash
  {
    size_t operator()(const CurrentKey& k) const
    {
      return std::hash<SymbolId>{}(k.first) ^ (std::hash<std::string>{}(k.second) * 2654435761u);
    }
  };
  std::unordered_map<CurrentKey, double, CurrentKeyHash> _current;
};

}  // namespace flox::indicator
