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

// A stand-in for the library's own flox_capi_abi_version.
//
// The handshake this file tests only ever does anything when the binding
// and the library it loaded disagree, and a single build tree cannot
// produce that: one header, one compiler, one number. The executable's own
// definition of the symbol satisfies the reference libflox_capi leaves
// undefined here, so the version "the library reports" becomes something
// a test can set -- which is what makes the refusal path reachable at all,
// and what tells a check that reads flox_capi_abi_version() apart from one
// that reads the FLOX_CAPI_ABI_VERSION macro and calls it the same thing.
namespace
{
uint32_t g_reportedAbiVersion = FLOX_CAPI_ABI_VERSION;

// Sets what the library reports for the duration of a test.
class ReportedAbiVersion
{
 public:
  explicit ReportedAbiVersion(uint32_t version) : _saved(g_reportedAbiVersion)
  {
    g_reportedAbiVersion = version;
  }
  ~ReportedAbiVersion() { g_reportedAbiVersion = _saved; }

  ReportedAbiVersion(const ReportedAbiVersion&) = delete;
  ReportedAbiVersion& operator=(const ReportedAbiVersion&) = delete;

 private:
  uint32_t _saved;
};
}  // namespace

extern "C" uint32_t flox_capi_abi_version(void)
{
  return g_reportedAbiVersion;
}

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

// ── What the check reads, and what it says ────────────────────────────

// The double has to actually be the symbol the check calls, or every test
// below it proves nothing.
TEST(CapiAbiCheckTest, TheReportedVersionIsWhatTheCheckReads)
{
  EXPECT_EQ(flox_capi_abi_version(), static_cast<uint32_t>(FLOX_CAPI_ABI_VERSION));
  {
    ReportedAbiVersion reported(FLOX_CAPI_ABI_VERSION + 7);
    EXPECT_EQ(flox_capi_abi_version(), static_cast<uint32_t>(FLOX_CAPI_ABI_VERSION + 7));
  }
  EXPECT_EQ(flox_capi_abi_version(), static_cast<uint32_t>(FLOX_CAPI_ABI_VERSION));
}

// The whole point of the handshake: the number on the other side comes
// from the library that was actually loaded, not from the macro the
// binding was compiled with. A check that compares the macro against
// itself agrees with the correct one in every same-tree build and is
// silent in the one case the handshake exists for.
TEST(CapiAbiCheckTest, ReadsTheLibrarysVersionRatherThanItsOwnMacro)
{
  ReportedAbiVersion reported(FLOX_CAPI_ABI_VERSION + 7);

  std::string message;
  EXPECT_FALSE(flox::capi::checkAbiVersion(FLOX_CAPI_ABI_VERSION, &message))
      << "the check agreed with a library reporting a different version, so it "
         "is not reading flox_capi_abi_version()";
  EXPECT_NE(message.find(std::to_string(FLOX_CAPI_ABI_VERSION + 7)), std::string::npos)
      << "the message does not carry the version the library reported: " << message;
}

// Both numbers appear in the refusal either way round; which is which is
// the entire content of the message. Assert the labels, not the digits.
TEST(CapiAbiCheckTest, RefusalMessageSaysWhichNumberIsWhich)
{
  const std::string compiled = std::to_string(FLOX_CAPI_ABI_VERSION + 1);
  const std::string runtime = std::to_string(FLOX_CAPI_ABI_VERSION);

  std::string message;
  ASSERT_FALSE(flox::capi::checkAbiVersion(FLOX_CAPI_ABI_VERSION + 1, &message));
  EXPECT_NE(message.find("built against version " + compiled), std::string::npos)
      << "the compiled-against version is not the one labelled as such: " << message;
  EXPECT_NE(message.find("reports version " + runtime), std::string::npos)
      << "the library's version is not the one labelled as such: " << message;
}

// The same message, with the two numbers the other way round: a swap that
// reads plausibly in either direction is only caught by pinning both.
TEST(CapiAbiCheckTest, RefusalMessageKeepsTheLabelsWhenTheSkewIsTheOtherWay)
{
  ReportedAbiVersion reported(FLOX_CAPI_ABI_VERSION + 5);

  std::string message;
  ASSERT_FALSE(flox::capi::checkAbiVersion(FLOX_CAPI_ABI_VERSION, &message));
  EXPECT_NE(message.find("built against version " + std::to_string(FLOX_CAPI_ABI_VERSION)),
            std::string::npos)
      << message;
  EXPECT_NE(message.find("reports version " + std::to_string(FLOX_CAPI_ABI_VERSION + 5)),
            std::string::npos)
      << message;
}

// abiVersionMismatchMessage is what a binding hands its host on refusal;
// it goes through the same comparison and must see the same skew.
TEST(CapiAbiCheckTest, TheConvenienceMessageSeesTheSameSkew)
{
  ReportedAbiVersion reported(FLOX_CAPI_ABI_VERSION + 3);

  const std::string message = flox::capi::abiVersionMismatchMessage(FLOX_CAPI_ABI_VERSION);
  EXPECT_FALSE(message.empty()) << "a mismatch produced no message";
  EXPECT_NE(message.find("reports version " + std::to_string(FLOX_CAPI_ABI_VERSION + 3)),
            std::string::npos)
      << message;
}

#endif  // FLOX_TEST_HAS_ABI_CHECK
