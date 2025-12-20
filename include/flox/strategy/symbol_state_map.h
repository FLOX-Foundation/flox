/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/common.h"

#include <array>
#include <cstddef>
#include <utility>
#include <vector>

namespace flox
{

template <typename State, size_t MaxSymbols = 256>
class SymbolStateMap
{
 public:
  static constexpr size_t kMaxSymbols = MaxSymbols;

  SymbolStateMap() = default;

  [[nodiscard]] State& operator[](SymbolId symbol) noexcept
  {
    if (symbol < kMaxSymbols) [[likely]]
    {
      if (!_initialized[symbol])
      {
        _flat[symbol] = State{};
        _initialized[symbol] = true;
      }
      return _flat[symbol];
    }
    return getOverflow(symbol);
  }

  [[nodiscard]] const State& operator[](SymbolId symbol) const noexcept
  {
    if (symbol < kMaxSymbols) [[likely]]
    {
      return _flat[symbol];
    }
    return getOverflowConst(symbol);
  }

  [[nodiscard]] bool contains(SymbolId symbol) const noexcept
  {
    if (symbol < kMaxSymbols)
    {
      return _initialized[symbol];
    }
    for (const auto& [id, _] : _overflow)
    {
      if (id == symbol)
      {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] State* tryGet(SymbolId symbol) noexcept
  {
    if (symbol < kMaxSymbols)
    {
      return _initialized[symbol] ? &_flat[symbol] : nullptr;
    }
    for (auto& [id, state] : _overflow)
    {
      if (id == symbol)
      {
        return &state;
      }
    }
    return nullptr;
  }

  [[nodiscard]] const State* tryGet(SymbolId symbol) const noexcept
  {
    if (symbol < kMaxSymbols)
    {
      return _initialized[symbol] ? &_flat[symbol] : nullptr;
    }
    for (const auto& [id, state] : _overflow)
    {
      if (id == symbol)
      {
        return &state;
      }
    }
    return nullptr;
  }

  void clear() noexcept
  {
    _flat = {};
    _initialized = {};
    _overflow.clear();
  }

  template <typename Func>
  void forEach(Func&& fn)
  {
    for (size_t i = 0; i < kMaxSymbols; ++i)
    {
      if (_initialized[i])
      {
        fn(static_cast<SymbolId>(i), _flat[i]);
      }
    }
    for (auto& [id, state] : _overflow)
    {
      fn(id, state);
    }
  }

  template <typename Func>
  void forEach(Func&& fn) const
  {
    for (size_t i = 0; i < kMaxSymbols; ++i)
    {
      if (_initialized[i])
      {
        fn(static_cast<SymbolId>(i), _flat[i]);
      }
    }
    for (const auto& [id, state] : _overflow)
    {
      fn(id, state);
    }
  }

  [[nodiscard]] size_t size() const noexcept
  {
    size_t count = 0;
    for (size_t i = 0; i < kMaxSymbols; ++i)
    {
      if (_initialized[i])
      {
        ++count;
      }
    }
    return count + _overflow.size();
  }

 private:
  State& getOverflow(SymbolId symbol)
  {
    for (auto& [id, state] : _overflow)
    {
      if (id == symbol)
      {
        return state;
      }
    }
    _overflow.emplace_back(symbol, State{});
    return _overflow.back().second;
  }

  const State& getOverflowConst(SymbolId symbol) const
  {
    for (const auto& [id, state] : _overflow)
    {
      if (id == symbol)
      {
        return state;
      }
    }
    static const State empty{};
    return empty;
  }

  alignas(64) std::array<State, kMaxSymbols> _flat{};
  std::array<bool, kMaxSymbols> _initialized{};
  std::vector<std::pair<SymbolId, State>> _overflow;
};

}  // namespace flox
