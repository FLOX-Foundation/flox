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
#include <cassert>
#include <cstddef>
#include <type_traits>
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
      _initialized[symbol] = true;
      return _flat[symbol];
    }
    // For non-movable types, assert we're within bounds
    if constexpr (!std::is_move_constructible_v<State>)
    {
      assert(false && "SymbolId exceeds MaxSymbols for non-movable type");
      // Return first element as fallback (UB protection)
      return _flat[0];
    }
    else
    {
      return getOverflow(symbol);
    }
  }

  [[nodiscard]] const State& operator[](SymbolId symbol) const noexcept
  {
    if (symbol < kMaxSymbols) [[likely]]
    {
      return _flat[symbol];
    }
    if constexpr (!std::is_move_constructible_v<State>)
    {
      assert(false && "SymbolId exceeds MaxSymbols for non-movable type");
      return _flat[0];
    }
    else
    {
      return getOverflowConst(symbol);
    }
  }

  [[nodiscard]] bool contains(SymbolId symbol) const noexcept
  {
    if (symbol < kMaxSymbols)
    {
      return _initialized[symbol];
    }
    if constexpr (std::is_move_constructible_v<State>)
    {
      for (const auto& [id, _] : overflow())
      {
        if (id == symbol)
        {
          return true;
        }
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
    if constexpr (std::is_move_constructible_v<State>)
    {
      for (auto& [id, state] : overflow())
      {
        if (id == symbol)
        {
          return &state;
        }
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
    if constexpr (std::is_move_constructible_v<State>)
    {
      for (const auto& [id, state] : overflow())
      {
        if (id == symbol)
        {
          return &state;
        }
      }
    }
    return nullptr;
  }

  void clear() noexcept
  {
    // For non-movable types, just reset the initialized flags
    if constexpr (std::is_move_constructible_v<State>)
    {
      _flat = {};
    }
    _initialized = {};
    _overflowStorage.clear();
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
    if constexpr (std::is_move_constructible_v<State>)
    {
      for (auto& [id, state] : overflow())
      {
        fn(id, state);
      }
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
    if constexpr (std::is_move_constructible_v<State>)
    {
      for (const auto& [id, state] : overflow())
      {
        fn(id, state);
      }
    }
  }

  size_t size() const noexcept
  {
    size_t count = 0;
    for (size_t i = 0; i < kMaxSymbols; ++i)
    {
      if (_initialized[i])
      {
        ++count;
      }
    }
    return count + _overflowStorage.size();
  }

 private:
  State& getOverflow(SymbolId symbol)
    requires std::is_move_constructible_v<State>
  {
    for (auto& [id, state] : overflow())
    {
      if (id == symbol)
      {
        return state;
      }
    }
    overflow().emplace_back(symbol, State{});
    return overflow().back().second;
  }

  const State& getOverflowConst(SymbolId symbol) const
    requires std::is_move_constructible_v<State>
  {
    for (const auto& [id, state] : overflow())
    {
      if (id == symbol)
      {
        return state;
      }
    }
    static const State empty{};
    return empty;
  }

  // Helper to conditionally include overflow storage
  template <typename T, bool Enable>
  struct OverflowStorage
  {
    std::vector<std::pair<SymbolId, T>> data;
    void clear() { data.clear(); }
    size_t size() const { return data.size(); }
  };

  template <typename T>
  struct OverflowStorage<T, false>
  {
    void clear() {}
    size_t size() const { return 0; }
  };

  alignas(64) std::array<State, kMaxSymbols> _flat{};
  std::array<bool, kMaxSymbols> _initialized{};
  OverflowStorage<State, std::is_move_constructible_v<State>> _overflowStorage;

  // Accessor for overflow (only valid for movable types)
  auto& overflow()
    requires std::is_move_constructible_v<State>
  {
    return _overflowStorage.data;
  }

  const auto& overflow() const
    requires std::is_move_constructible_v<State>
  {
    return _overflowStorage.data;
  }
};

}  // namespace flox
