/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/aggregator/bar.h"

#include <array>
#include <cstddef>
#include <iterator>

namespace flox
{

template <size_t Capacity = 256>
class BarSeries
{
 public:
  static_assert(Capacity > 0, "Capacity must be greater than 0");
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2 for efficient modulo");

  static constexpr size_t kCapacity = Capacity;

  BarSeries() = default;

  void push(const Bar& bar) noexcept
  {
    _head = (_head + Capacity - 1) & (Capacity - 1);
    _data[_head] = bar;
    if (_size < Capacity)
    {
      ++_size;
    }
  }

  const Bar& operator[](size_t idx) const noexcept
  {
    return _data[(_head + idx) & (Capacity - 1)];
  }

  const Bar* at(size_t idx) const noexcept
  {
    if (idx >= _size)
    {
      return nullptr;
    }
    return &_data[(_head + idx) & (Capacity - 1)];
  }

  size_t size() const noexcept { return _size; }
  constexpr size_t capacity() const noexcept { return Capacity; }
  bool empty() const noexcept { return _size == 0; }
  bool full() const noexcept { return _size == Capacity; }

  void clear() noexcept
  {
    _head = 0;
    _size = 0;
  }

  const Bar& front() const noexcept { return _data[_head]; }

  const Bar& back() const noexcept
  {
    return _data[(_head + _size - 1) & (Capacity - 1)];
  }

  class Iterator
  {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = Bar;
    using difference_type = std::ptrdiff_t;
    using pointer = const Bar*;
    using reference = const Bar&;

    Iterator(const BarSeries* series, size_t idx) : _series(series), _idx(idx) {}

    reference operator*() const { return (*_series)[_idx]; }
    pointer operator->() const { return &(*_series)[_idx]; }

    Iterator& operator++()
    {
      ++_idx;
      return *this;
    }

    Iterator operator++(int)
    {
      Iterator tmp = *this;
      ++_idx;
      return tmp;
    }

    bool operator==(const Iterator& other) const { return _idx == other._idx; }
    bool operator!=(const Iterator& other) const { return _idx != other._idx; }

   private:
    const BarSeries* _series;
    size_t _idx;
  };

  Iterator begin() const { return Iterator(this, 0); }
  Iterator end() const { return Iterator(this, _size); }

 private:
  alignas(64) std::array<Bar, Capacity> _data{};
  size_t _head = 0;
  size_t _size = 0;
};

}  // namespace flox
