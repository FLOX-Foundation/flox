/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

/// Cross-platform force inline macro.
/// Use on hot path functions where inlining is critical for performance.
#ifdef _MSC_VER
#define LRVX_FORCE_INLINE __forceinline
#else
#define LRVX_FORCE_INLINE __attribute__((always_inline)) inline
#endif
