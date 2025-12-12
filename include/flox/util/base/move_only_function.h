/*
 * FLOX Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace flox
{

/**
 * A move-only callable wrapper similar to std::move_only_function (C++23).
 * Provides type-erased storage for callable objects without requiring copyability.
 * Uses small buffer optimization to avoid heap allocations for small callables.
 */
template <typename Signature>
class MoveOnlyFunction;

template <typename R, typename... Args>
class MoveOnlyFunction<R(Args...)>
{
  static constexpr std::size_t kBufferSize = 32;
  static constexpr std::size_t kBufferAlign = alignof(std::max_align_t);

  struct ICallable
  {
    virtual ~ICallable() = default;
    virtual R invoke(Args...) = 0;
    virtual void moveTo(void* dst) noexcept = 0;
  };

  template <typename F>
  struct Callable final : ICallable
  {
    F func;

    template <typename U>
    explicit Callable(U&& f) : func(std::forward<U>(f))
    {
    }

    R invoke(Args... args) override { return func(std::forward<Args>(args)...); }

    void moveTo(void* dst) noexcept override { new (dst) Callable(std::move(func)); }
  };

  template <typename F>
  static constexpr bool fits_in_buffer =
      sizeof(Callable<F>) <= kBufferSize && alignof(Callable<F>) <= kBufferAlign &&
      std::is_nothrow_move_constructible_v<F>;

  alignas(kBufferAlign) std::byte _buffer[kBufferSize];
  ICallable* _callable = nullptr;
  bool _heap = false;

  void destroy() noexcept
  {
    if (_callable)
    {
      if (_heap)
      {
        delete _callable;
      }
      else
      {
        _callable->~ICallable();
      }
      _callable = nullptr;
      _heap = false;
    }
  }

 public:
  MoveOnlyFunction() noexcept = default;

  MoveOnlyFunction(std::nullptr_t) noexcept {}

  template <typename F,
            typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, MoveOnlyFunction>>>
  MoveOnlyFunction(F&& f)
  {
    using CallableType = Callable<std::decay_t<F>>;
    if constexpr (fits_in_buffer<std::decay_t<F>>)
    {
      _callable = new (_buffer) CallableType(std::forward<F>(f));
      _heap = false;
    }
    else
    {
      _callable = new CallableType(std::forward<F>(f));
      _heap = true;
    }
  }

  ~MoveOnlyFunction() { destroy(); }

  MoveOnlyFunction(const MoveOnlyFunction&) = delete;
  MoveOnlyFunction& operator=(const MoveOnlyFunction&) = delete;

  MoveOnlyFunction(MoveOnlyFunction&& other) noexcept
  {
    if (other._callable)
    {
      if (other._heap)
      {
        _callable = other._callable;
        _heap = true;
      }
      else
      {
        other._callable->moveTo(_buffer);
        _callable = reinterpret_cast<ICallable*>(_buffer);
        _heap = false;
        other._callable->~ICallable();
      }
      other._callable = nullptr;
      other._heap = false;
    }
  }

  MoveOnlyFunction& operator=(MoveOnlyFunction&& other) noexcept
  {
    if (this != &other)
    {
      destroy();
      if (other._callable)
      {
        if (other._heap)
        {
          _callable = other._callable;
          _heap = true;
        }
        else
        {
          other._callable->moveTo(_buffer);
          _callable = reinterpret_cast<ICallable*>(_buffer);
          _heap = false;
          other._callable->~ICallable();
        }
        other._callable = nullptr;
        other._heap = false;
      }
    }
    return *this;
  }

  MoveOnlyFunction& operator=(std::nullptr_t) noexcept
  {
    destroy();
    return *this;
  }

  explicit operator bool() const noexcept { return _callable != nullptr; }

  R operator()(Args... args) { return _callable->invoke(std::forward<Args>(args)...); }
};

}  // namespace flox
