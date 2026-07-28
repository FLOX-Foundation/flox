/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#include "flox/util/crypto.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace flox::crypto;

namespace
{
int g_failures = 0;
int g_checks = 0;
void check(bool ok, const char* e, int line)
{
  ++g_checks;
  if (!ok)
  {
    ++g_failures;
    std::printf("  FAIL line %d: %s\n", line, e);
  }
}
#define CHECK(x) check((x), #x, __LINE__)
std::string h(const std::string& s)
{
  auto d = sha256(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  return toHex(d.data(), d.size());
}
}  // namespace

TEST(Crypto, EngineSuite)
{
  std::printf("test_crypto\n");
  {
    auto s = sha1(reinterpret_cast<const uint8_t*>("abc"), 3);
    CHECK(toHex(s.data(), s.size()) == "a9993e364706816aba3e25717850c26c9cd0d89d");
  }
  CHECK(h("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(h("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  {
    const std::string key = "key", msg = "The quick brown fox jumps over the lazy dog";
    auto mac = hmacSha256(reinterpret_cast<const uint8_t*>(key.data()), key.size(),
                          reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
    CHECK(toHex(mac.data(), mac.size()) ==
          "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
  }
  CHECK(base64(reinterpret_cast<const uint8_t*>("Man"), 3) == "TWFu");
  CHECK(base64(reinterpret_cast<const uint8_t*>("Ma"), 2) == "TWE=");
  CHECK(base64(reinterpret_cast<const uint8_t*>("M"), 1) == "TQ==");

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  EXPECT_EQ(g_failures, 0);
}
