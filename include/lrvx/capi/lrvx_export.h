/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

// LRVX_EXPORT(...) — annotation read by tools/codegen at code-generation time.
// Expands to a clang::annotate attribute on clang, no-op everywhere else, so
// the engine compiles unchanged on every supported toolchain.
//
// Codegen inputs lives in include/lrvx/capi/lrvx_capi_spec.hpp. Each annotated
// declaration in that file becomes one entry in the generated lrvx_capi.h.
//
// Annotation argument vocabulary (see .notes/api-idl-rfc.md §Annotation
// convention for the full table):
//
//   c_name="lrvx_x"            — C symbol name in the generated header.
//   group="indicator"          — section/grouping for emitter output.
//   handle="LrvxXHandle"       — declaration creates an opaque handle typedef.
//   on_handle="LrvxXHandle"    — method takes an existing handle as first arg.
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
#define LRVX_EXPORT(...) [[clang::annotate("lrvx::export(" #__VA_ARGS__ ")")]]
#endif
#endif

#ifndef LRVX_EXPORT
#define LRVX_EXPORT(...)
#endif
