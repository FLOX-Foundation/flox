/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Account and MarginMode moved to lrvx/clearing/account.h. They are not
 * backtest-only: the venue's ledger and clearing price fills against an
 * account, and while the class lived under lrvx/backtest/ every build that
 * ran a venue had to enable the backtest module to get it.
 *
 * Names, namespace and behaviour are unchanged -- this header forwards, and
 * also keeps lrvx/backtest/liquidation_engine.h on the include path, which
 * callers used to get through here.
 */

#pragma once

#include "lrvx/backtest/liquidation_engine.h"
#include "lrvx/clearing/account.h"
