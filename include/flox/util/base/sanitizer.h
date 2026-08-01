/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// FLOX_TSAN_ENABLED is 1 when the build runs under ThreadSanitizer, else 0.
// Lives in its own header because the __has_feature fallback define confuses
// some clang-format versions about everything that follows it in a file.

#pragma once

#if defined(__SANITIZE_THREAD__)
#define FLOX_TSAN_ENABLED 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define FLOX_TSAN_ENABLED 1
#endif
#endif

#ifndef FLOX_TSAN_ENABLED
#define FLOX_TSAN_ENABLED 0
#endif
