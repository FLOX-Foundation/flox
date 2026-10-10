/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/backtest/grid_search.h"

#include "lrvx/log/log.h"

namespace lrvx
{

void GridSearch::addAxis(std::vector<double> values)
{
  _axes.push_back(std::move(values));
}

void GridSearch::setFactory(BacktestFactory factory)
{
  _factory = std::move(factory);
}

std::size_t GridSearch::totalCombinations() const
{
  if (_axes.empty())
  {
    return 0;
  }
  std::size_t total = 1;
  for (const auto& axis : _axes)
  {
    if (axis.empty())
    {
      return 0;
    }
    total *= axis.size();
  }
  return total;
}

std::vector<double> GridSearch::paramsForIndex(std::size_t index) const
{
  std::vector<double> out;
  out.reserve(_axes.size());
  std::size_t remaining = index;
  // Last axis varies fastest (row-major flattening).
  for (std::size_t i = 0; i < _axes.size(); ++i)
  {
    std::size_t stride = 1;
    for (std::size_t j = i + 1; j < _axes.size(); ++j)
    {
      stride *= _axes[j].size();
    }
    const std::size_t slot = (remaining / stride) % _axes[i].size();
    out.push_back(_axes[i][slot]);
  }
  return out;
}

std::vector<GridSearchResult> GridSearch::run(std::size_t /*numThreads*/)
{
  std::vector<GridSearchResult> results;
  if (!_factory)
  {
    LRVX_LOG_ERROR("GridSearch: no factory set");
    return results;
  }
  const std::size_t total = totalCombinations();
  if (total == 0)
  {
    LRVX_LOG_ERROR("GridSearch: no parameter combinations");
    return results;
  }
  results.reserve(total);
  for (std::size_t i = 0; i < total; ++i)
  {
    GridSearchResult r;
    r.paramIndex = i;
    r.params = paramsForIndex(i);
    BacktestResult bt = _factory(r.params);
    r.stats = bt.computeStats();
    results.push_back(std::move(r));
  }
  return results;
}

}  // namespace lrvx
