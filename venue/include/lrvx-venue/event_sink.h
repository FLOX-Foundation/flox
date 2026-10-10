/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "lrvx-venue/messages.h"

#include <functional>

namespace lrvx::venue
{
using EventSink = std::function<void(const OutboundEvent&)>;
}  // namespace lrvx::venue
