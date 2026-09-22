/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Account and MarginMode moved to flox/clearing/account.h. They are not
 * backtest-only: the venue's ledger and clearing price fills against an
 * account, and while the class lived under flox/backtest/ every build that
 * ran a venue had to enable the backtest module to get it.
 *
 * Names, namespace and behaviour are unchanged -- this header forwards, and
 * also keeps flox/backtest/liquidation_engine.h on the include path, which
 * callers used to get through here.
 */

#pragma once

#include "flox/backtest/liquidation_engine.h"
#include "flox/clearing/account.h"
