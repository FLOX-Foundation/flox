/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

// FLOX_EXPORT(...) — annotation read by tools/codegen at code-generation time.
// Expands to a clang::annotate attribute on clang, no-op everywhere else, so
// the engine compiles unchanged on every supported toolchain.
//
// Codegen inputs lives in include/flox/capi/flox_capi_spec.hpp. Each annotated
// declaration in that file becomes one entry in the generated flox_capi.h.
//
// Annotation argument vocabulary (see .notes/api-idl-rfc.md §Annotation
// convention for the full table):
//
//   c_name="flox_x"            — C symbol name in the generated header.
//   group="indicator"          — section/grouping for emitter output.
//   handle="FloxXHandle"       — declaration creates an opaque handle typedef.
//   on_handle="FloxXHandle"    — method takes an existing handle as first arg.
//   return_kind="order_id"     — return convention hint.
//   pointer_out_wrapper        — auto-generate `*_p` variant for Codon/QuickJS.
//   callback_bundle            — struct is a callback bundle (fn ptrs + user_data).
//   internal_only              — suppress generation (escape hatch).
//
// IMPORTANT: this header must remain dependency-free C/C++ — it is included
// from both engine TUs and the spec file. Do not pull in <type_traits>,
// <utility>, or anything that would bloat the precompiled prefix.

#if defined(__clang__) && defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::annotate)
#define FLOX_EXPORT(...) [[clang::annotate("flox::export(" #__VA_ARGS__ ")")]]
#endif
#endif

#ifndef FLOX_EXPORT
#define FLOX_EXPORT(...)
#endif
