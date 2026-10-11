/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/strategy/signal.h"

#include <span>

namespace lrvx
{

class ISignalHandler
{
 public:
  virtual ~ISignalHandler() = default;

  virtual void onSignal(const Signal& signal) = 0;

  // Batch entry. The default walks the batch through onSignal, so every
  // existing handler keeps its behaviour; a live bridge overrides this to
  // carry one tick's signals — a quoter's bid/ask pair, a ladder requote —
  // into a single IOrderExecutor::submitOrders call instead of N independent
  // submits. The batch is one decision made together, which is also what
  // makes it safe to coalesce: orders that must not share a request should
  // not share a batch.
  virtual void onSignalBatch(std::span<const Signal> signals)
  {
    for (const auto& signal : signals)
    {
      onSignal(signal);
    }
  }
};

}  // namespace lrvx
