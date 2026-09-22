/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * FeeTier and FeeSchedule moved to flox/clearing/fee_schedule.h. A fee ladder
 * is what a venue charges, not something a backtest invented: the matching
 * engine prices every fill through one, so while the class lived under
 * flox/backtest/ a venue build had to enable the backtest module to get it.
 *
 * Names, namespace and behaviour are unchanged -- this header forwards, and
 * keeps the includes callers used to get through here.
 */

#pragma once

#include "flox/backtest/account.h"
#include "flox/clearing/fee_schedule.h"
