/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

// RateLimitPolicy moved to lrvx/execution/: the same venue quota model is
// consumed by both the backtest SimulatedExecutor and the live
// LiveRateLimitBudgeter -- one mechanism, so the backtest cannot drift from
// what production enforces. This shim keeps existing includes working.
#include "lrvx/execution/rate_limit_policy.h"
