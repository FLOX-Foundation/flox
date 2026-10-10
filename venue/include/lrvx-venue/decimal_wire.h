/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * The venue's spelling of the shared decimal wire helpers. The implementation
 * moved to lrvx/util/decimal_wire.h so the FIX initiator on the client side
 * prints and parses decimals through the same code as the venue codecs on the
 * server side; the venue call sites keep the name they had.
 */
#pragma once

#include "lrvx/util/decimal_wire.h"

namespace lrvx::venue::decwire
{

using lrvx::decwire::kFracDigits;
using lrvx::decwire::kScale;

using lrvx::decwire::append;
using lrvx::decwire::appendU64;
using lrvx::decwire::parse;

}  // namespace lrvx::venue::decwire
