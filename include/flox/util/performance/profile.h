/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#ifdef FLOX_ENABLE_TRACY
#include <tracy/Tracy.hpp>
#define FLOX_PROFILE_SCOPE(name) ZoneScopedN(name)
#define FLOX_PROFILE_FUNCTION() ZoneScoped
#else
#define FLOX_PROFILE_SCOPE(name) ((void)0)
#define FLOX_PROFILE_FUNCTION() ((void)0)
#endif
