/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * The venue's spelling of the shared decimal wire helpers. The implementation
 * moved to flox/util/decimal_wire.h so the FIX initiator on the client side
 * prints and parses decimals through the same code as the venue codecs on the
 * server side; the venue call sites keep the name they had.
 */
#pragma once

#include "flox/util/decimal_wire.h"

namespace flox::venue::decwire
{

using flox::decwire::kFracDigits;
using flox::decwire::kScale;

using flox::decwire::append;
using flox::decwire::appendU64;
using flox::decwire::parse;

}  // namespace flox::venue::decwire
