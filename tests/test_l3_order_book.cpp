/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/book/l3/l3_order_book.h"
#include "flox/common.h"

#include <gtest/gtest.h>

using namespace flox;

TEST(L3OrderBookTest, Construction)
{
  L3OrderBook<10> orderBook;
}
