/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

/// Cross-platform force inline macro.
/// Use on hot path functions where inlining is critical for performance.
#ifdef _MSC_VER
#define FLOX_FORCE_INLINE __forceinline
#else
#define FLOX_FORCE_INLINE __attribute__((always_inline)) inline
#endif
