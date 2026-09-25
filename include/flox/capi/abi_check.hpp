/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// abi_check.hpp -- the startup handshake flox_capi.h asks every caller to
// run, in one place instead of four.
//
// The structs on the C boundary carry no reserved tail, so a header from
// one version used against a library from another produces wrong numbers
// rather than a failed load. The header's instruction is to "compare
// FLOX_CAPI_ABI_VERSION against flox_capi_abi_version() once at startup
// and refuse the mismatch"; every binding does that here, at load, and
// refuses to come up when the two disagree.
//
// The refusal names both numbers because neither half can be read off a
// crash report otherwise: the compiled-against version is a macro baked
// into the binding, the runtime version comes from whichever libflox the
// loader found.

#pragma once

#include "flox/capi/flox_capi.h"

#include <cstdint>
#include <string>

namespace flox::capi
{

// Pass FLOX_CAPI_ABI_VERSION as seen by the translation unit that builds
// the binding. Returns true when it matches the loaded library. On a
// mismatch *message, when given, carries the text for the load error.
inline bool checkAbiVersion(uint32_t compiledVersion, std::string* message)
{
  const uint32_t runtimeVersion = flox_capi_abi_version();
  if (compiledVersion == runtimeVersion)
  {
    if (message != nullptr)
    {
      message->clear();
    }
    return true;
  }

  if (message != nullptr)
  {
    *message = "flox C ABI version mismatch: this binding was built against version " +
               std::to_string(compiledVersion) + ", the loaded flox library reports version " +
               std::to_string(runtimeVersion) +
               ". The structs on that boundary have no reserved tail, so continuing would "
               "read wrong numbers rather than fail. Rebuild the binding against the "
               "library it loads, or install the library the binding was built for.";
  }
  return false;
}

// The message a binding shows when it refuses to load, for the common
// case of a binding compiled against the header it ships with.
inline std::string abiVersionMismatchMessage(uint32_t compiledVersion)
{
  std::string message;
  checkAbiVersion(compiledVersion, &message);
  return message;
}

}  // namespace flox::capi
