/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// The ABI-version handshake every binding has to run at load.
//
// flox_capi.h tells a caller to "compare FLOX_CAPI_ABI_VERSION against
// flox_capi_abi_version() once at startup and refuse the mismatch" --
// the structs on that boundary have no reserved tail, so a header from
// one version used against a library from another produces wrong numbers
// rather than a failed load. Every binding was doing the comparison
// nowhere, and each of the four would otherwise write its own.
//
// So the comparison lives in one place, flox/capi/abi_check.hpp, and this
// is its test: it accepts the version it was compiled against, it refuses
// any other one, and the refusal message names both numbers so the person
// reading a crash report can tell which half is stale.

#include "flox/capi/flox_capi.h"

#include <gtest/gtest.h>

#include <string>

#if __has_include("flox/capi/abi_check.hpp")
#include "flox/capi/abi_check.hpp"
#define FLOX_TEST_HAS_ABI_CHECK 1
#endif

namespace
{

constexpr const char* kMissing =
    "include/flox/capi/abi_check.hpp is missing: no binding can share the "
    "header's startup check without it. Expected "
    "bool flox::capi::checkAbiVersion(uint32_t compiledVersion, "
    "std::string* message).";

}  // namespace

TEST(CapiAbiCheckTest, TheSharedCheckExists)
{
#ifndef FLOX_TEST_HAS_ABI_CHECK
  FAIL() << kMissing;
#else
  SUCCEED();
#endif
}

#ifdef FLOX_TEST_HAS_ABI_CHECK

// The library and the header in this build tree are the same version by
// construction, so the check has to pass here -- a check that fails on a
// matched pair would take every binding down on a correct install.
TEST(CapiAbiCheckTest, AcceptsTheVersionItWasCompiledAgainst)
{
  std::string message = "untouched";
  EXPECT_TRUE(flox::capi::checkAbiVersion(FLOX_CAPI_ABI_VERSION, &message));
  EXPECT_TRUE(message.empty()) << message;
}

// The half a test can fake: a caller compiled against a different header.
TEST(CapiAbiCheckTest, RefusesAVersionTheLibraryDoesNotReport)
{
  std::string message;
  EXPECT_FALSE(flox::capi::checkAbiVersion(FLOX_CAPI_ABI_VERSION + 1, &message));
  EXPECT_FALSE(message.empty()) << "a refusal with no message";

  // Both numbers, so the message says which half is stale.
  const std::string compiled = std::to_string(FLOX_CAPI_ABI_VERSION + 1);
  const std::string runtime = std::to_string(flox_capi_abi_version());
  EXPECT_NE(message.find(compiled), std::string::npos)
      << "message does not name the compiled-against version: " << message;
  EXPECT_NE(message.find(runtime), std::string::npos)
      << "message does not name the runtime version: " << message;
}

TEST(CapiAbiCheckTest, RefusesAVersionBelowTheLibrarysToo)
{
  std::string message;
  EXPECT_FALSE(flox::capi::checkAbiVersion(FLOX_CAPI_ABI_VERSION - 1, &message));
  EXPECT_FALSE(message.empty());
}

// A binding that has nowhere to put the text still gets an answer.
TEST(CapiAbiCheckTest, ANullMessagePointerIsNotAnError)
{
  EXPECT_TRUE(flox::capi::checkAbiVersion(FLOX_CAPI_ABI_VERSION, nullptr));
  EXPECT_FALSE(flox::capi::checkAbiVersion(FLOX_CAPI_ABI_VERSION + 1, nullptr));
}

#endif  // FLOX_TEST_HAS_ABI_CHECK
