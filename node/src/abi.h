// node/src/abi.h -- the C ABI version handshake the addon runs when it
// loads.
//
// flox_capi.h asks every caller to "compare FLOX_CAPI_ABI_VERSION against
// flox_capi_abi_version() once at startup and refuse the mismatch". The
// addon compiles against the header in this tree and links whichever
// libflox the loader found, and the structs on that boundary carry no
// reserved tail: a skew reads as wrong numbers rather than a failed load,
// so require() is the last place where noticing it is still cheap.
//
// registerAbi throws from Init on a mismatch, which fails the require
// with a message naming both versions. The pair is exported too, so a
// caller can report what it is running against.

#pragma once
#include <napi.h>

#include "flox/capi/abi_check.hpp"

#include <string>

namespace node_flox
{

inline bool registerAbi(Napi::Env env, Napi::Object exports)
{
  std::string message;
  if (!flox::capi::checkAbiVersion(FLOX_CAPI_ABI_VERSION, &message))
  {
    Napi::Error::New(env, message).ThrowAsJavaScriptException();
    return false;
  }

  exports.Set("CAPI_ABI_VERSION",
              Napi::Number::New(env, static_cast<double>(FLOX_CAPI_ABI_VERSION)));
  exports.Set("capiAbiVersion",
              Napi::Function::New(env,
                                  [](const Napi::CallbackInfo& info)
                                  {
                                    return Napi::Number::New(
                                        info.Env(),
                                        static_cast<double>(flox_capi_abi_version()));
                                  }));
  return true;
}

}  // namespace node_flox
