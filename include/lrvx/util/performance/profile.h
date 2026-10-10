/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#ifdef LRVX_ENABLE_TRACY
#include <tracy/Tracy.hpp>
#define LRVX_PROFILE_SCOPE(name) ZoneScopedN(name)
#define LRVX_PROFILE_FUNCTION() ZoneScoped
#else
#define LRVX_PROFILE_SCOPE(name) ((void)0)
#define LRVX_PROFILE_FUNCTION() ((void)0)
#endif
