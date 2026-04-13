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

  // ── STL-compatible iterator ────────────────────────────────────────────────

  template <bool Const>
  class Iterator
  {
   public:
    using map_type =
        std::conditional_t<Const, const SymbolStateMap, SymbolStateMap>;
    using state_ref = std::conditional_t<Const, const State&, State&>;
    using value_type = std::pair<SymbolId, state_ref>;

    Iterator(map_type* map, size_t flatIdx, size_t overflowIdx)
        : _map(map), _flatIdx(flatIdx), _overflowIdx(overflowIdx)
    {
      advanceToValid();
    }

    value_type operator*() const
    {
      if (_flatIdx < kMaxSymbols)
      {
        return {static_cast<SymbolId>(_flatIdx), _map->_flat[_flatIdx]};
      }
      if constexpr (std::is_move_constructible_v<State>)
      {
        auto& entry = _map->overflow()[_overflowIdx];
        return {entry.first, entry.second};
      }
      // Unreachable for non-movable types (no overflow)
#ifdef _MSC_VER
      __assume(false);
#else
      __builtin_unreachable();
#endif
    }

    Iterator& operator++()
    {
      if (_flatIdx < kMaxSymbols)
      {
        ++_flatIdx;
      }
      else
      {
        ++_overflowIdx;
      }
      advanceToValid();
      return *this;
    }

    bool operator!=(const Iterator& other) const
    {
      return _flatIdx != other._flatIdx || _overflowIdx != other._overflowIdx;
    }

    bool operator==(const Iterator& other) const { return !(*this != other); }

   private:
    void advanceToValid()
    {
      while (_flatIdx < kMaxSymbols && !_map->_initialized[_flatIdx])
      {
        ++_flatIdx;
      }
    }

    map_type* _map;
    size_t _flatIdx;
    size_t _overflowIdx;
  };

  using iterator = Iterator<false>;
  using const_iterator = Iterator<true>;

  iterator begin() noexcept
  {
    return iterator(this, 0, 0);
  }

  iterator end() noexcept
  {
    size_t overflowSize = 0;
    if constexpr (std::is_move_constructible_v<State>)
    {
      overflowSize = overflow().size();
    }
    return iterator(this, kMaxSymbols, overflowSize);
  }

  const_iterator begin() const noexcept
  {
    return const_iterator(this, 0, 0);
  }

  const_iterator end() const noexcept
  {
    size_t overflowSize = 0;
    if constexpr (std::is_move_constructible_v<State>)
    {
      overflowSize = overflow().size();
    }
    return const_iterator(this, kMaxSymbols, overflowSize);
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
