/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

// RateLimitPolicy moved to flox/execution/: the same venue quota model is
// consumed by both the backtest SimulatedExecutor and the live
// LiveRateLimitBudgeter -- one mechanism, so the backtest cannot drift from
// what production enforces. This shim keeps existing includes working.
#include "flox/execution/rate_limit_policy.h"
