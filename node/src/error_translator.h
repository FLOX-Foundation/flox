/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Translate flox::FloxError thrown from C++ engine code into a JavaScript
// Error with `.code` / `.helpUrl` extra properties set on the instance.
//
// Usage at the boundary of a NAPI method that calls into FLOX:
//
//     Napi::Value myMethod(const Napi::CallbackInfo& info) {
//       return node_flox::tryFlox(info.Env(), [&] {
//         // ... call FLOX C++ that may throw FloxError ...
//         return info.Env().Undefined();
//       });
//     }
//
// On FloxError: a JS Error is thrown with `.code` and `.helpUrl` set,
// and the lambda's nominal return value is replaced with `env.Undefined()`.
// On std::exception: a generic JS Error is thrown with the .what() text.
// On success: the lambda's return value is returned unchanged.

#pragma once

#include <napi.h>

#include "flox/error/flox_error.h"

#include <exception>

namespace node_flox
{

template <typename Fn>
inline Napi::Value tryFlox(Napi::Env env, Fn fn)
{
  try
  {
    return fn();
  }
  catch (const flox::FloxError& e)
  {
    auto err = Napi::Error::New(env, e.message());
    err.Value().Set("code", Napi::String::New(env, e.code()));
    err.Value().Set("helpUrl", Napi::String::New(env, e.helpUrl()));
    err.Value().Set("name", Napi::String::New(env, "FloxError"));
    err.ThrowAsJavaScriptException();
    return env.Undefined();
  }
  catch (const std::exception& e)
  {
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
    return env.Undefined();
  }
}

}  // namespace node_flox
