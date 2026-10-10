/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * FeeTier and FeeSchedule moved to lrvx/clearing/fee_schedule.h. A fee ladder
 * is what a venue charges, not something a backtest invented: the matching
 * engine prices every fill through one, so while the class lived under
 * lrvx/backtest/ a venue build had to enable the backtest module to get it.
 *
 * Names, namespace and behaviour are unchanged -- this header forwards, and
 * keeps the includes callers used to get through here.
 */

#pragma once

#include "lrvx/backtest/account.h"
#include "lrvx/clearing/fee_schedule.h"
